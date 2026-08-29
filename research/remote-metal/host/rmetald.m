/* Remote Metal host daemon -- runs on macOS, owns the real MTLDevice.
 *
 * The guest never sees a host pointer. Every Objective-C object it can refer
 * to lives in a generation-tagged table here, and the guest holds only an
 * index. See protocol.h for why that matters.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include "../protocol.h"

/* ---- handle table ------------------------------------------------------ */

typedef struct {
    __unsafe_unretained id obj;   /* retained manually via CFBridgingRetain */
    uint32_t generation;
    int      in_use;
} rm_slot;

static rm_slot  *g_slots;
static uint32_t  g_slot_count;
static uint32_t  g_slot_cap;
static uint32_t  g_live;

static uint64_t rm_intern(id obj) {
    if (!obj) return RM_NULL_HANDLE;
    uint32_t slot = UINT32_MAX;
    for (uint32_t i = 0; i < g_slot_count; i++)
        if (!g_slots[i].in_use) { slot = i; break; }
    if (slot == UINT32_MAX) {
        if (g_slot_count == g_slot_cap) {
            g_slot_cap = g_slot_cap ? g_slot_cap * 2 : 256;
            g_slots = realloc(g_slots, g_slot_cap * sizeof(rm_slot));
        }
        slot = g_slot_count++;
        g_slots[slot].generation = 1;
    }
    CFBridgingRetain(obj);              /* +1, balanced in rm_release_handle */
    g_slots[slot].obj = obj;
    g_slots[slot].in_use = 1;
    g_live++;
    return RM_HANDLE(g_slots[slot].generation, slot);
}

/* Returns nil and sets *err when the handle is stale or bogus. Callers must
 * check: silently treating a stale handle as valid is exactly the corruption
 * this table exists to prevent. */
static id rm_resolve(uint64_t h, uint32_t *err) {
    *err = RM_OK;
    if (h == RM_NULL_HANDLE) return nil;
    uint32_t slot = RM_HANDLE_SLOT(h), gen = RM_HANDLE_GEN(h);
    if (slot >= g_slot_count || !g_slots[slot].in_use) { *err = RM_ERR_BAD_HANDLE; return nil; }
    if (g_slots[slot].generation != gen)                { *err = RM_ERR_STALE_HANDLE; return nil; }
    return g_slots[slot].obj;
}

static uint32_t rm_release_handle(uint64_t h) {
    uint32_t err; id obj = rm_resolve(h, &err);
    if (err != RM_OK) return err;
    if (!obj) return RM_OK;
    uint32_t slot = RM_HANDLE_SLOT(h);
    CFRelease((__bridge CFTypeRef)g_slots[slot].obj);
    g_slots[slot].obj = nil;
    g_slots[slot].in_use = 0;
    g_slots[slot].generation++;      /* invalidates every outstanding handle */
    g_live--;
    return RM_OK;
}

/* ---- drawable pairing --------------------------------------------------
 *
 * NEXT_DRAWABLE interns TWO handles, and both must die together. Deriving the
 * pairing later -- by trusting the caller's color_texture, or by scanning the
 * table for the texture object -- is wrong on exactly the paths that matter:
 * a malformed stream would release whatever texture the caller named, and a
 * drawable/texture mismatch would release the drawable while stranding its
 * real texture handle. Record the truth at acquisition instead, and route
 * every terminal path through one consume helper. */
#define RM_MAX_DRAWABLES 8
static struct { uint64_t drawable, texture; } g_pairs[RM_MAX_DRAWABLES];

static void rm_pair_record(uint64_t d, uint64_t t) {
    for (int i = 0; i < RM_MAX_DRAWABLES; i++)
        if (!g_pairs[i].drawable) { g_pairs[i].drawable = d; g_pairs[i].texture = t; return; }
    fprintf(stderr, "[rmetald] drawable pair table full -- leaking %llu\n",
            (unsigned long long)d);
}

/* Release a drawable and its texture together. Safe to call on any terminal
 * path, including ones where the drawable was never presented. */
static void rm_consume_drawable(uint64_t d) {
    if (!d) return;
    for (int i = 0; i < RM_MAX_DRAWABLES; i++)
        if (g_pairs[i].drawable == d) {
            rm_release_handle(g_pairs[i].texture);
            g_pairs[i].drawable = g_pairs[i].texture = 0;
            break;
        }
    rm_release_handle(d);
}

/* ---- host window ------------------------------------------------------
 *
 * AppKit owns the main thread; RPC runs on a worker. Every window and layer
 * mutation is bounced to the main queue, because touching either from the
 * socket thread is undefined and fails intermittently rather than loudly.
 */
static NSWindow    *g_window;
static CAMetalLayer *g_layer;

static void host_window_build(id<MTLDevice> dev) {
        NSRect r = NSMakeRect(120, 120, 640, 480);
        g_window = [[NSWindow alloc] initWithContentRect:r
            styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                       NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable)
            backing:NSBackingStoreBuffered defer:NO];
        [g_window setTitle:@"Madeira remote Metal"];
        g_layer = [CAMetalLayer layer];
        g_layer.device = dev;
        g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_layer.framebufferOnly = NO;          /* readback allowed for verification */
        /* Bounded acquisition: a drawable that never arrives must become an
         * ANSWER, not a hung RPC thread. */
        g_layer.allowsNextDrawableTimeout = YES;
        NSView *v = [g_window contentView];
        [v setWantsLayer:YES];
        [v setLayer:g_layer];
        g_layer.frame = v.bounds;
        g_layer.drawableSize = CGSizeMake(v.bounds.size.width, v.bounds.size.height);
        [g_window makeKeyAndOrderFront:nil];
}

/* Safe from either thread: build directly when already on main. */
static void host_window_create(id<MTLDevice> dev) {
    if ([NSThread isMainThread]) host_window_build(dev);
    else dispatch_sync(dispatch_get_main_queue(), ^{ host_window_build(dev); });
}

/* ---- framed IO --------------------------------------------------------- */

static int rd(int fd, void *p, size_t n) {
    uint8_t *b = p;
    while (n) { ssize_t r = read(fd, b, n); if (r <= 0) return -1; b += r; n -= (size_t)r; }
    return 0;
}
static int wr(int fd, const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) { ssize_t r = write(fd, b, n); if (r <= 0) return -1; b += r; n -= (size_t)r; }
    return 0;
}

static int reply(int fd, struct rm_hdr *req, uint32_t status, const void *payload, uint32_t len) {
    struct rm_hdr h = { RM_MAGIC, RM_VERSION, req->opcode, req->seq, status, len, 0 };
    if (wr(fd, &h, sizeof h)) return -1;
    return len ? wr(fd, payload, len) : 0;
}

/* Release every handle this connection interned. Without it the table grows
 * for the lifetime of the daemon and GPU memory is never reclaimed -- a
 * long-lived debugging rig would leak every resource of every past session. */
static void rm_reset_table(void) {
    for (uint32_t i = 0; i < g_slot_count; i++)
        if (g_slots[i].in_use) {
            CFRelease((__bridge CFTypeRef)g_slots[i].obj);
            g_slots[i].obj = nil;
            g_slots[i].in_use = 0;
            g_slots[i].generation++;
        }
    g_live = 0;
    memset(g_pairs, 0, sizeof g_pairs);
}

/* Resource uploads are megabytes, not kilobytes. A fixed 64KB payload buffer
 * silently closed the connection on the first 1MB buffer upload, which the
 * guest saw only as SIGPIPE -- grow on demand instead, with a cap so a bogus
 * length field cannot exhaust host memory. */
#define RM_MAX_PAYLOAD (256u << 20)

static void serve(int fd) {
    uint8_t *payload = NULL;
    uint32_t payload_cap = 0;
    for (;;) {
        struct rm_hdr h;
        if (rd(fd, &h, sizeof h)) break;
        if (h.magic != RM_MAGIC)   { reply(fd, &h, RM_ERR_BAD_MAGIC, NULL, 0); break; }
        if (h.version != RM_VERSION) { reply(fd, &h, RM_ERR_BAD_VERSION, NULL, 0); break; }
        if (h.payload_len > RM_MAX_PAYLOAD) {
            fprintf(stderr, "[rmetald] payload %u exceeds cap\n", h.payload_len);
            reply(fd, &h, RM_ERR_SHORT_PAYLOAD, NULL, 0); break;
        }
        if (h.payload_len > payload_cap) {
            uint8_t *np = realloc(payload, h.payload_len);
            if (!np) { reply(fd, &h, RM_ERR_SHORT_PAYLOAD, NULL, 0); break; }
            payload = np; payload_cap = h.payload_len;
        }
        if (h.payload_len && rd(fd, payload, h.payload_len)) break;

        @autoreleasepool {
        uint32_t err = RM_OK;
        switch (h.opcode) {
        case RM_OP_PING:
            reply(fd, &h, RM_OK, NULL, 0); break;

        case RM_OP_COPY_ALL_DEVICES: {
            id dev = MTLCreateSystemDefaultDevice();
            NSArray *arr = dev ? @[dev] : @[];
            struct rm_ret_handle r = { rm_intern(arr) };
            reply(fd, &h, RM_OK, &r, sizeof r); break;
        }
        case RM_OP_ARRAY_COUNT: {
            if (h.payload_len < sizeof(struct rm_arg_handle)) { reply(fd,&h,RM_ERR_SHORT_PAYLOAD,NULL,0); break; }
            id o = rm_resolve(((struct rm_arg_handle *)payload)->handle, &err);
            if (err != RM_OK) { reply(fd,&h,err,NULL,0); break; }
            if (![o isKindOfClass:[NSArray class]]) { reply(fd,&h,RM_ERR_WRONG_CLASS,NULL,0); break; }
            struct rm_ret_u64 r = { [(NSArray *)o count] };
            reply(fd, &h, RM_OK, &r, sizeof r); break;
        }
        case RM_OP_ARRAY_OBJECT: {
            struct rm_arg_handle_u64 *a = (void *)payload;
            if (h.payload_len < sizeof *a) { reply(fd,&h,RM_ERR_SHORT_PAYLOAD,NULL,0); break; }
            id o = rm_resolve(a->handle, &err);
            if (err != RM_OK) { reply(fd,&h,err,NULL,0); break; }
            if (![o isKindOfClass:[NSArray class]]) { reply(fd,&h,RM_ERR_WRONG_CLASS,NULL,0); break; }
            NSArray *arr = o;
            if (a->arg >= arr.count) { reply(fd,&h,RM_ERR_BAD_HANDLE,NULL,0); break; }
            struct rm_ret_handle r = { rm_intern(arr[(NSUInteger)a->arg]) };
            reply(fd, &h, RM_OK, &r, sizeof r); break;
        }
        case RM_OP_DEVICE_NAME: {
            id o = rm_resolve(((struct rm_arg_handle *)payload)->handle, &err);
            if (err != RM_OK) { reply(fd,&h,err,NULL,0); break; }
            const char *n = [[(id<MTLDevice>)o name] UTF8String] ?: "";
            reply(fd, &h, RM_OK, n, (uint32_t)strlen(n)); break;
        }
        case RM_OP_SUPPORTS_FAMILY: {
            struct rm_arg_handle_u64 *a = (void *)payload;
            id o = rm_resolve(a->handle, &err);
            if (err != RM_OK) { reply(fd,&h,err,NULL,0); break; }
            struct rm_ret_u64 r = { [(id<MTLDevice>)o supportsFamily:(MTLGPUFamily)a->arg] ? 1 : 0 };
            reply(fd, &h, RM_OK, &r, sizeof r); break;
        }
        case RM_OP_SUPPORTS_BC: {
            id o = rm_resolve(((struct rm_arg_handle *)payload)->handle, &err);
            if (err != RM_OK) { reply(fd,&h,err,NULL,0); break; }
            struct rm_ret_u64 r = { [(id<MTLDevice>)o supportsBCTextureCompression] ? 1 : 0 };
            reply(fd, &h, RM_OK, &r, sizeof r); break;
        }
        case RM_OP_ALLOCATED_SIZE: {
            id o = rm_resolve(((struct rm_arg_handle *)payload)->handle, &err);
            if (err != RM_OK) { reply(fd,&h,err,NULL,0); break; }
            struct rm_ret_u64 r = { [(id<MTLDevice>)o currentAllocatedSize] };
            reply(fd, &h, RM_OK, &r, sizeof r); break;
        }
        case RM_OP_RETAIN: {
            id o = rm_resolve(((struct rm_arg_handle *)payload)->handle, &err);
            if (err != RM_OK) { reply(fd,&h,err,NULL,0); break; }
            struct rm_ret_handle r = { rm_intern(o) };   /* new handle, own refcount */
            reply(fd, &h, RM_OK, &r, sizeof r); break;
        }
        case RM_OP_RELEASE:
            reply(fd, &h, rm_release_handle(((struct rm_arg_handle *)payload)->handle), NULL, 0); break;

        case RM_OP_STATS: {
            struct rm_ret_u64 r = { g_live };
            reply(fd, &h, RM_OK, &r, sizeof r); break;
        }
        case RM_OP_NEW_COMMAND_QUEUE: {
            id o = rm_resolve(((struct rm_arg_handle *)payload)->handle, &err);
            if (err != RM_OK) { reply(fd,&h,err,NULL,0); break; }
            struct rm_ret_handle r = { rm_intern([(id<MTLDevice>)o newCommandQueue]) };
            reply(fd, &h, RM_OK, &r, sizeof r); break;
        }
        case RM_OP_NEW_BUFFER: {
            struct rm_new_buffer *a = (void *)payload;
            if (h.payload_len < sizeof *a) { reply(fd,&h,RM_ERR_SHORT_PAYLOAD,NULL,0); break; }
            id o = rm_resolve(a->device, &err);
            if (err != RM_OK) { reply(fd,&h,err,NULL,0); break; }
            uint32_t inline_len = h.payload_len - (uint32_t)sizeof *a;
            if (inline_len && a->length != inline_len) {   /* declared vs delivered */
                reply(fd, &h, RM_ERR_SHORT_PAYLOAD, NULL, 0); break;
            }
            const void *init = inline_len ? payload + sizeof *a : NULL;
            id<MTLBuffer> b = init
                ? [(id<MTLDevice>)o newBufferWithBytes:init length:a->length options:MTLResourceStorageModeShared]
                : [(id<MTLDevice>)o newBufferWithLength:a->length options:MTLResourceStorageModeShared];
            struct rm_ret_handle r = { rm_intern(b) };
            reply(fd, &h, RM_OK, &r, sizeof r); break;
        }
        case RM_OP_NEW_TEXTURE: {
            struct rm_new_texture *a = (void *)payload;
            id o = rm_resolve(a->device, &err);
            if (err != RM_OK) { reply(fd,&h,err,NULL,0); break; }
            MTLTextureDescriptor *d = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:(MTLPixelFormat)a->pixel_format
                width:(NSUInteger)a->width height:(NSUInteger)a->height mipmapped:NO];
            d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            d.storageMode = MTLStorageModeShared;
            struct rm_ret_handle r = { rm_intern([(id<MTLDevice>)o newTextureWithDescriptor:d]) };
            reply(fd, &h, RM_OK, &r, sizeof r); break;
        }
        case RM_OP_NEW_LIBRARY: {
            struct rm_arg_handle *a = (void *)payload;
            id o = rm_resolve(a->handle, &err);
            if (err != RM_OK) { reply(fd,&h,err,NULL,0); break; }
            NSString *src = [[NSString alloc] initWithBytes:payload + sizeof *a
                             length:h.payload_len - sizeof *a encoding:NSUTF8StringEncoding];
            NSError *e = nil;
            id<MTLLibrary> lib = [(id<MTLDevice>)o newLibraryWithSource:src options:nil error:&e];
            if (!lib) { /* surface the compiler diagnostic; a silent nil is useless */
                const char *m = [[e localizedDescription] UTF8String] ?: "shader compile failed";
                fprintf(stderr, "[rmetald] library: %s\n", m);
                reply(fd, &h, RM_ERR_WRONG_CLASS, NULL, 0); break;
            }
            struct rm_ret_handle r = { rm_intern(lib) };
            reply(fd, &h, RM_OK, &r, sizeof r); break;
        }
        case RM_OP_NEW_FUNCTION: {
            struct rm_arg_handle *a = (void *)payload;
            id o = rm_resolve(a->handle, &err);
            if (err != RM_OK) { reply(fd,&h,err,NULL,0); break; }
            NSString *n = [[NSString alloc] initWithBytes:payload + sizeof *a
                           length:h.payload_len - sizeof *a encoding:NSUTF8StringEncoding];
            struct rm_ret_handle r = { rm_intern([(id<MTLLibrary>)o newFunctionWithName:n]) };
            reply(fd, &h, RM_OK, &r, sizeof r); break;
        }
        case RM_OP_NEW_RENDER_PIPELINE: {
            struct rm_new_pipeline *a = (void *)payload;
            id dev = rm_resolve(a->device, &err); if (err != RM_OK) { reply(fd,&h,err,NULL,0); break; }
            id vfn = rm_resolve(a->vfn, &err);    if (err != RM_OK) { reply(fd,&h,err,NULL,0); break; }
            id ffn = rm_resolve(a->ffn, &err);    if (err != RM_OK) { reply(fd,&h,err,NULL,0); break; }
            MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
            d.vertexFunction = vfn; d.fragmentFunction = ffn;
            d.colorAttachments[0].pixelFormat = (MTLPixelFormat)a->pixel_format;
            NSError *e = nil;
            id ps = [(id<MTLDevice>)dev newRenderPipelineStateWithDescriptor:d error:&e];
            if (!ps) { fprintf(stderr, "[rmetald] pipeline: %s\n",
                               [[e localizedDescription] UTF8String] ?: "?");
                       reply(fd, &h, RM_ERR_WRONG_CLASS, NULL, 0); break; }
            struct rm_ret_handle r = { rm_intern(ps) };
            reply(fd, &h, RM_OK, &r, sizeof r); break;
        }
        case RM_OP_SUBMIT_RENDER_PASS: {
            struct rm_render_pass *a = (void *)payload;
            /* A producer that forgets to zero present_drawable would send
             * stack garbage here, and resolving it silently fails the whole
             * pass with a confusing STALE_HANDLE. Validate it as a handle only
             * when non-zero, and say which field was wrong. */
            if (h.payload_len < sizeof *a ||
                a->cmd_bytes > h.payload_len - sizeof *a) {   /* stream must fit the frame */
                reply(fd,&h,RM_ERR_SHORT_PAYLOAD,NULL,0); break;
            }
            /* From here on, any early return must give the drawable back. */
            id q = rm_resolve(a->queue, &err);
            if (err != RM_OK) { rm_consume_drawable(a->present_drawable);
                                reply(fd,&h,err,NULL,0); break; }
            id tex = rm_resolve(a->color_texture, &err);
            if (err != RM_OK) { rm_consume_drawable(a->present_drawable);
                                reply(fd,&h,err,NULL,0); break; }
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = tex;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(a->clear_r, a->clear_g, a->clear_b, a->clear_a);
            id<MTLCommandBuffer> cb = [(id<MTLCommandQueue>)q commandBuffer];
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];

            /* Walk the CONTIGUOUS stream by size, never by pointer. */
            const uint8_t *p = payload + sizeof *a;
            uint32_t off = 0, bad = 0;
            while (off + sizeof(struct rm_enc_hdr) <= a->cmd_bytes) {
                const struct rm_enc_hdr *rec = (const void *)(p + off);
                if (rec->size < sizeof *rec || off + rec->size > a->cmd_bytes) { bad = 1; break; }
                /* Every record must be at least its own struct size. Without
                 * this a short record would be read past its end -- the
                 * command stream is attacker-controlled input. */
                {
                    uint32_t need = 0;
                    switch (rec->type) {
                    case RM_ENC_SET_PIPELINE:     need = sizeof(struct rm_enc_pipeline); break;
                    case RM_ENC_SET_VERTEX_BUFFER:need = sizeof(struct rm_enc_vbuf);     break;
                    case RM_ENC_SET_VIEWPORT:     need = sizeof(struct rm_enc_viewport); break;
                    case RM_ENC_DRAW:             need = sizeof(struct rm_enc_draw);     break;
                    default: need = 0xffffffff; break;
                    }
                    if (rec->size < need) { bad = 1; break; }
                }
                switch (rec->type) {
                case RM_ENC_SET_PIPELINE: {
                    const struct rm_enc_pipeline *c = (const void *)rec;
                    id ps = rm_resolve(c->pipeline, &err); if (err != RM_OK) { bad = 1; break; }
                    [enc setRenderPipelineState:ps]; break;
                }
                case RM_ENC_SET_VERTEX_BUFFER: {
                    const struct rm_enc_vbuf *c = (const void *)rec;
                    id b = rm_resolve(c->buffer, &err); if (err != RM_OK) { bad = 1; break; }
                    [enc setVertexBuffer:b offset:(NSUInteger)c->offset atIndex:(NSUInteger)c->index]; break;
                }
                case RM_ENC_SET_VIEWPORT: {
                    const struct rm_enc_viewport *c = (const void *)rec;
                    [enc setViewport:(MTLViewport){c->x, c->y, c->w, c->h_, c->znear, c->zfar}]; break;
                }
                case RM_ENC_DRAW: {
                    const struct rm_enc_draw *c = (const void *)rec;
                    [enc drawPrimitives:(MTLPrimitiveType)c->primitive
                            vertexStart:(NSUInteger)c->start vertexCount:(NSUInteger)c->count]; break;
                }
                default: bad = 1; break;
                }
                if (bad) break;
                off += rec->size;
            }
            [enc endEncoding];
            if (bad) {
                /* A malformed command stream must not strand the drawable: it
                 * is single-use, and leaking one per failed frame exhausts the
                 * pool and then blocks acquisition forever. */
                rm_consume_drawable(a->present_drawable);
                reply(fd, &h, err ? err : RM_ERR_BAD_OPCODE, NULL, 0); break;
            }
            /* Present on THIS command buffer, before commit -- Metal's
             * intended sequencing. Presenting from a separate call after this
             * buffer had committed would race the display. */
            if (a->present_drawable) {
                id d = rm_resolve(a->present_drawable, &err);
                /* The pass must render into THIS drawable's texture. Presenting
                 * a drawable whose texture was never drawn shows a stale or
                 * blank frame, which looks like a renderer bug rather than a
                 * protocol misuse. */
                if (err != RM_OK) { rm_consume_drawable(a->present_drawable);
                                    reply(fd,&h,err,NULL,0); break; }
                if (d && tex != [(id<CAMetalDrawable>)d texture]) {
                    fprintf(stderr, "[rmetald] color_texture is not the drawable's texture\n");
                    rm_consume_drawable(a->present_drawable);
                    reply(fd, &h, RM_ERR_WRONG_CLASS, NULL, 0); break;
                }
                if (err != RM_OK) {
                    fprintf(stderr, "[rmetald] present_drawable=%llu invalid (%u) -- "
                            "an uninitialised field in the caller's render pass?\n",
                            (unsigned long long)a->present_drawable, err);
                    reply(fd,&h,err,NULL,0); break;
                }
                [cb presentDrawable:(id<CAMetalDrawable>)d];
            }
            [cb commit];
            [cb waitUntilCompleted];   /* synchronous by design */
            /* Consume the drawable AND its texture. A drawable is single-use,
             * and NEXT_DRAWABLE interns two handles for it -- releasing only
             * the drawable leaked one texture handle per frame, which a 600
             * frame run made obvious (611 live handles). The texture belongs
             * to the drawable, so its handle dies with it. */
            rm_consume_drawable(a->present_drawable);
            struct rm_ret_u64 r = { (uint64_t)[cb status] };
            reply(fd, &h, RM_OK, &r, sizeof r); break;
        }
        case RM_OP_LAYER_SIZE: {
            __block CGSize sz = CGSizeZero;
            dispatch_sync(dispatch_get_main_queue(), ^{ sz = g_layer.drawableSize; });
            struct rm_ret_drawable r = { 0, 0, (uint64_t)sz.width, (uint64_t)sz.height };
            reply(fd, &h, RM_OK, &r, sizeof r); break;
        }
        case RM_OP_NEXT_DRAWABLE: {
            __block CGSize sz = CGSizeZero;
            /* Only the window/layer MUTATION belongs on main. nextDrawable can
             * block -- on the drawable pool, or indefinitely while the window
             * is minimised -- and blocking the main queue there would freeze
             * AppKit event processing, so the user could not even restore the
             * window that is causing the block. Acquire on this thread. */
            dispatch_sync(dispatch_get_main_queue(), ^{
                NSView *v = [g_window contentView];
                g_layer.frame = v.bounds;
                CGFloat s = g_window.backingScaleFactor ?: 1.0;
                CGSize want = CGSizeMake(v.bounds.size.width * s, v.bounds.size.height * s);
                if (!CGSizeEqualToSize(want, g_layer.drawableSize) && want.width > 0)
                    g_layer.drawableSize = want;
                sz = g_layer.drawableSize;
            });
            id<CAMetalDrawable> d = [g_layer nextDrawable];
            if (!d) { reply(fd, &h, RM_ERR_NO_DRAWABLE, NULL, 0); break; }
            uint64_t dh = rm_intern(d), th = rm_intern(d.texture);
            rm_pair_record(dh, th);
            struct rm_ret_drawable r = { dh, th, (uint64_t)sz.width, (uint64_t)sz.height };
            reply(fd, &h, RM_OK, &r, sizeof r); break;
        }
        case RM_OP_DISCARD_DRAWABLE: {
            /* Acquired but never submitted. Without this the guest has no way
             * to give a drawable back, and the single-use pool drains. */
            rm_consume_drawable(((struct rm_arg_handle *)payload)->handle);
            reply(fd, &h, RM_OK, NULL, 0); break;
        }
        case RM_OP_TEXTURE_GETBYTES: {
            id o = rm_resolve(((struct rm_arg_handle *)payload)->handle, &err);
            if (err != RM_OK) { reply(fd,&h,err,NULL,0); break; }
            id<MTLTexture> t = o;
            NSUInteger w = t.width, ht = t.height, bpr = w * 4, total = bpr * ht;
            uint8_t *px = malloc(total);
            [t getBytes:px bytesPerRow:bpr fromRegion:MTLRegionMake2D(0,0,w,ht) mipmapLevel:0];
            reply(fd, &h, RM_OK, px, (uint32_t)total);
            free(px); break;
        }
        default:
            reply(fd, &h, RM_ERR_BAD_OPCODE, NULL, 0); break;
        }
        }
    }
    free(payload);
}

/* A shared secret, required as the first frame of every connection. The daemon
 * compiles arbitrary shader source and allocates GPU memory on request, so an
 * unauthenticated listener on a routable interface is a remote code-execution
 * surface for anyone on the network. Bound to an explicit address as well. */
static char g_token[64];

static int authenticate(int fd) {
    struct rm_hdr h;
    if (rd(fd, &h, sizeof h)) return 0;
    if (h.magic != RM_MAGIC || h.opcode != RM_OP_PING) return 0;
    if (h.payload_len != strlen(g_token)) return 0;
    char got[sizeof g_token];
    if (rd(fd, got, h.payload_len)) return 0;
    if (memcmp(got, g_token, h.payload_len) != 0) return 0;
    reply(fd, &h, RM_OK, NULL, 0);
    return 1;
}

static int g_listen_fd;

static void *rpc_thread(void *unused) {
    (void)unused;
    for (;;) {
        int c = accept(g_listen_fd, NULL, NULL);
        if (c < 0) continue;
        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        struct timeval tv = { .tv_sec = 30 };
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        if (!authenticate(c)) {
            fprintf(stderr, "[rmetald] rejected unauthenticated client\n");
            close(c); continue;
        }
        fprintf(stderr, "[rmetald] client authenticated\n");
        serve(c);
        close(c);
        fprintf(stderr, "[rmetald] client gone; releasing %u session handles\n", g_live);
        rm_reset_table();
    }
    return NULL;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        id<MTLDevice> d = MTLCreateSystemDefaultDevice();
        if (!d) { fprintf(stderr, "no Metal device\n"); return 1; }
        fprintf(stderr, "[rmetald] host GPU: %s\n", [[d name] UTF8String]);
        fprintf(stderr, "[rmetald] Apple7=%d Apple8=%d Apple9=%d BC=%d\n",
                [d supportsFamily:MTLGPUFamilyApple7], [d supportsFamily:MTLGPUFamilyApple8],
                [d supportsFamily:MTLGPUFamilyApple9], [d supportsBCTextureCompression]);
    }
    const char *bind_addr = (argc > 1) ? argv[1] : "127.0.0.1";
    const char *tok = getenv("RMETAL_TOKEN");
    if (!tok || !*tok) { fprintf(stderr, "set RMETAL_TOKEN to a shared secret\n"); return 1; }
    snprintf(g_token, sizeof g_token, "%s", tok);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(RM_PORT) };
    if (inet_pton(AF_INET, bind_addr, &a.sin_addr) != 1) {
        fprintf(stderr, "bad bind address %s\n", bind_addr); return 1;
    }
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 4)) { perror("bind/listen"); return 1; }
    fprintf(stderr, "[rmetald] listening on %s:%d (token required)\n", bind_addr, RM_PORT);
    g_listen_fd = s;

    /* AppKit owns the main thread; RPC runs on a worker. The window is created
     * here, directly -- NOT via dispatch_sync to the main queue, which from the
     * main thread before [NSApp run] is an immediate deadlock. dispatch_sync is
     * only correct from the RPC thread, where it is used. */
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        host_window_create(MTLCreateSystemDefaultDevice());
        [NSApp activateIgnoringOtherApps:YES];
        pthread_t t;
        pthread_create(&t, NULL, rpc_thread, NULL);
        [NSApp run];
    }
    return 0;
}
