/* Remote Metal guest spike -- runs natively on the iOS guest, no FEX involved.
 *
 * Answers the questions that must be settled before committing weeks to a
 * remote Metal backend: does the round trip work at all from inside the VM,
 * what does a call cost, and does the generation-tagged handle table actually
 * reject a stale handle instead of quietly addressing a recycled object.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include "../protocol.h"

static int g_fd;
static uint32_t g_seq;

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

/* One synchronous call. Returns the status; response payload copied to out. */
static uint32_t call(uint16_t op, const void *arg, uint32_t arglen,
                     void *out, uint32_t outcap, uint32_t *outlen) {
    struct rm_hdr h = { RM_MAGIC, RM_VERSION, op, ++g_seq, 0, arglen, 0 };
    if (wr(g_fd, &h, sizeof h)) return 0xffffffff;
    if (arglen && wr(g_fd, arg, arglen)) return 0xffffffff;
    struct rm_hdr r;
    if (rd(g_fd, &r, sizeof r)) return 0xffffffff;
    if (r.magic != RM_MAGIC || r.seq != h.seq) {
        fprintf(stderr, "  PROTOCOL DESYNC: magic=%08x seq=%u expected %u\n", r.magic, r.seq, h.seq);
        return 0xffffffff;
    }
    uint8_t scratch[65536];
    uint32_t n = r.payload_len;
    if (n) { if (rd(g_fd, scratch, n)) return 0xffffffff; }
    if (out && n) { uint32_t c = n < outcap ? n : outcap; memcpy(out, scratch, c); }
    if (outlen) *outlen = n;
    return r.status;
}

static double now_ms(void) {
    struct timeval t; gettimeofday(&t, NULL);
    return t.tv_sec * 1000.0 + t.tv_usec / 1000.0;
}

static const char *statname(uint32_t s) {
    switch (s) {
    case RM_OK: return "OK";
    case RM_ERR_STALE_HANDLE: return "STALE_HANDLE";
    case RM_ERR_BAD_HANDLE: return "BAD_HANDLE";
    case RM_ERR_WRONG_CLASS: return "WRONG_CLASS";
    default: return "err";
    }
}

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "10.0.1.53";
    g_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(RM_PORT) };
    inet_pton(AF_INET, host, &a.sin_addr);
    if (connect(g_fd, (struct sockaddr *)&a, sizeof a)) { perror("connect"); return 1; }
    int one = 1; setsockopt(g_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    printf("[rmtest] connected to %s:%d\n\n", host, RM_PORT);

    /* --- device enumeration through the handle table --- */
    struct rm_ret_handle rh; uint32_t st;
    st = call(RM_OP_COPY_ALL_DEVICES, NULL, 0, &rh, sizeof rh, NULL);
    printf("  CopyAllDevices      -> %s handle=%llu (gen=%u slot=%u)\n",
           statname(st), (unsigned long long)rh.handle,
           RM_HANDLE_GEN(rh.handle), RM_HANDLE_SLOT(rh.handle));
    uint64_t arr = rh.handle;

    struct rm_ret_u64 ru; struct rm_arg_handle ah = { arr };
    call(RM_OP_ARRAY_COUNT, &ah, sizeof ah, &ru, sizeof ru, NULL);
    printf("  ArrayCount          -> %llu\n", (unsigned long long)ru.value);

    struct rm_arg_handle_u64 a2 = { arr, 0 };
    call(RM_OP_ARRAY_OBJECT, &a2, sizeof a2, &rh, sizeof rh, NULL);
    uint64_t dev = rh.handle;
    printf("  ArrayObject[0]      -> handle=%llu\n", (unsigned long long)dev);

    char name[256] = {0}; uint32_t nl = 0;
    struct rm_arg_handle ad = { dev };
    call(RM_OP_DEVICE_NAME, &ad, sizeof ad, name, sizeof name - 1, &nl);
    name[nl < sizeof name ? nl : sizeof name - 1] = 0;
    printf("  DeviceName          -> \"%s\"\n", name);

    /* the capabilities the paravirtual device could not provide */
    for (int fam = 1001; fam <= 1009; fam += 2) {
        struct rm_arg_handle_u64 af = { dev, (uint64_t)fam };
        call(RM_OP_SUPPORTS_FAMILY, &af, sizeof af, &ru, sizeof ru, NULL);
        printf("  supportsFamily(%d) -> %llu\n", fam, (unsigned long long)ru.value);
    }
    call(RM_OP_SUPPORTS_BC, &ad, sizeof ad, &ru, sizeof ru, NULL);
    printf("  supportsBC          -> %llu\n", (unsigned long long)ru.value);
    call(RM_OP_ALLOCATED_SIZE, &ad, sizeof ad, &ru, sizeof ru, NULL);
    printf("  allocatedSize       -> %llu\n", (unsigned long long)ru.value);

    /* --- stale handle must be REJECTED, not silently reused --- */
    printf("\n  [handle lifetime]\n");
    struct rm_arg_handle rel = { dev };
    st = call(RM_OP_RELEASE, &rel, sizeof rel, NULL, 0, NULL);
    printf("    release(dev)      -> %s\n", statname(st));
    st = call(RM_OP_ALLOCATED_SIZE, &ad, sizeof ad, &ru, sizeof ru, NULL);
    printf("    use after release -> %s  %s\n", statname(st),
           st == RM_ERR_BAD_HANDLE || st == RM_ERR_STALE_HANDLE ? "(correctly rejected)" : "*** LEAKED ***");
    /* re-intern something so the slot is recycled, then retry the OLD handle */
    call(RM_OP_COPY_ALL_DEVICES, NULL, 0, &rh, sizeof rh, NULL);
    st = call(RM_OP_ALLOCATED_SIZE, &ad, sizeof ad, &ru, sizeof ru, NULL);
    printf("    stale after reuse -> %s  %s\n", statname(st),
           st == RM_ERR_STALE_HANDLE || st == RM_ERR_BAD_HANDLE || st == RM_ERR_WRONG_CLASS
               ? "(correctly rejected)" : "*** ADDRESSED A DIFFERENT OBJECT ***");

    /* --- latency --- */
    printf("\n  [latency, %d calls each]\n", 2000);
    double t0 = now_ms();
    for (int i = 0; i < 2000; i++) call(RM_OP_PING, NULL, 0, NULL, 0, NULL);
    double ping = (now_ms() - t0) / 2000.0;
    struct rm_arg_handle a3 = { arr };
    t0 = now_ms();
    for (int i = 0; i < 2000; i++) call(RM_OP_ARRAY_COUNT, &a3, sizeof a3, &ru, sizeof ru, NULL);
    double work = (now_ms() - t0) / 2000.0;
    printf("    ping (no Metal)   %.4f ms  -> %.0f calls/sec\n", ping, 1000.0 / ping);
    printf("    ArrayCount        %.4f ms  -> %.0f calls/sec\n", work, 1000.0 / work);

    /* ================= offscreen triangle ================= */
    printf("\n  [offscreen render]\n");
    call(RM_OP_COPY_ALL_DEVICES, NULL, 0, &rh, sizeof rh, NULL);
    arr = rh.handle;
    struct rm_arg_handle_u64 a0 = { arr, 0 };
    call(RM_OP_ARRAY_OBJECT, &a0, sizeof a0, &rh, sizeof rh, NULL);
    dev = rh.handle;

    struct rm_arg_handle adev = { dev };
    call(RM_OP_NEW_COMMAND_QUEUE, &adev, sizeof adev, &rh, sizeof rh, NULL);
    uint64_t queue = rh.handle;

    /* vertex buffer: one triangle in clip space */
    static const float verts[6] = { 0.0f, 0.8f,  -0.8f, -0.8f,  0.8f, -0.8f };
    uint8_t buf[sizeof(struct rm_new_buffer) + sizeof verts];
    struct rm_new_buffer *nb = (void *)buf;
    nb->device = dev; nb->length = sizeof verts;
    memcpy(buf + sizeof *nb, verts, sizeof verts);
    st = call(RM_OP_NEW_BUFFER, buf, sizeof buf, &rh, sizeof rh, NULL);
    uint64_t vbuf = rh.handle;
    printf("    newBuffer(%zub)   -> %s handle=%llu\n", sizeof verts, statname(st),
           (unsigned long long)vbuf);

    /* render target */
    struct rm_new_texture nt = { dev, 80 /*MTLPixelFormatBGRA8Unorm*/, 64, 64 };
    st = call(RM_OP_NEW_TEXTURE, &nt, sizeof nt, &rh, sizeof rh, NULL);
    uint64_t tex = rh.handle;
    printf("    newTexture(64x64) -> %s\n", statname(st));

    /* shaders, compiled on the host */
    static const char *src =
        "#include <metal_stdlib>\n using namespace metal;\n"
        "vertex float4 vmain(const device float2 *p [[buffer(0)]], uint vid [[vertex_id]])\n"
        "{ return float4(p[vid], 0, 1); }\n"
        "fragment float4 fmain() { return float4(1.0, 0.5, 0.25, 1.0); }\n";
    uint8_t lb[sizeof(struct rm_arg_handle) + 512];
    struct rm_arg_handle *la = (void *)lb;
    la->handle = dev;
    size_t slen = strlen(src);
    memcpy(lb + sizeof *la, src, slen);
    st = call(RM_OP_NEW_LIBRARY, lb, (uint32_t)(sizeof *la + slen), &rh, sizeof rh, NULL);
    uint64_t lib = rh.handle;
    printf("    newLibrary        -> %s\n", statname(st));
    if (st != RM_OK) { printf("    (shader failed to compile; see host log)\n"); close(g_fd); return 1; }

    uint64_t fns[2];
    const char *fnames[2] = { "vmain", "fmain" };
    for (int i = 0; i < 2; i++) {
        uint8_t fb[sizeof(struct rm_arg_handle) + 32];
        struct rm_arg_handle *fa = (void *)fb; fa->handle = lib;
        size_t n = strlen(fnames[i]); memcpy(fb + sizeof *fa, fnames[i], n);
        call(RM_OP_NEW_FUNCTION, fb, (uint32_t)(sizeof *fa + n), &rh, sizeof rh, NULL);
        fns[i] = rh.handle;
    }
    struct rm_new_pipeline np = { dev, fns[0], fns[1], 80 };
    st = call(RM_OP_NEW_RENDER_PIPELINE, &np, sizeof np, &rh, sizeof rh, NULL);
    uint64_t pso = rh.handle;
    printf("    newPipeline       -> %s\n", statname(st));
    if (st != RM_OK) { close(g_fd); return 1; }

    /* ---- build the CONTIGUOUS command stream (no pointers) ---- */
    uint8_t pass[sizeof(struct rm_render_pass) + 256];
    struct rm_render_pass *rp = (void *)pass;
    rp->queue = queue; rp->color_texture = tex;
    rp->clear_r = 0.0; rp->clear_g = 0.0; rp->clear_b = 0.0; rp->clear_a = 1.0;
    uint8_t *c = pass + sizeof *rp; uint32_t cb = 0;
    struct rm_enc_viewport *vp = (void *)(c + cb);
    vp->h.type = RM_ENC_SET_VIEWPORT; vp->h.size = sizeof *vp;
    vp->x = 0; vp->y = 0; vp->w = 64; vp->h_ = 64; vp->znear = 0; vp->zfar = 1;
    cb += sizeof *vp;
    struct rm_enc_pipeline *ep = (void *)(c + cb);
    ep->h.type = RM_ENC_SET_PIPELINE; ep->h.size = sizeof *ep; ep->pipeline = pso;
    cb += sizeof *ep;
    struct rm_enc_vbuf *ev = (void *)(c + cb);
    ev->h.type = RM_ENC_SET_VERTEX_BUFFER; ev->h.size = sizeof *ev;
    ev->buffer = vbuf; ev->offset = 0; ev->index = 0;
    cb += sizeof *ev;
    struct rm_enc_draw *ed = (void *)(c + cb);
    ed->h.type = RM_ENC_DRAW; ed->h.size = sizeof *ed;
    ed->primitive = 3 /*triangle*/; ed->start = 0; ed->count = 3;
    cb += sizeof *ed;
    rp->cmd_bytes = cb;

    double tr0 = now_ms();
    st = call(RM_OP_SUBMIT_RENDER_PASS, pass, (uint32_t)(sizeof *rp + cb), &ru, sizeof ru, NULL);
    double trms = now_ms() - tr0;
    printf("    submitRenderPass  -> %s  (4 encoder cmds in ONE round trip, %.2f ms)\n",
           statname(st), trms);
    if (st != RM_OK) { close(g_fd); return 1; }

    /* ---- readback + checksum ---- */
    static uint8_t px[64 * 64 * 4];
    struct rm_arg_handle at = { tex };
    uint32_t got = 0;
    st = call(RM_OP_TEXTURE_GETBYTES, &at, sizeof at, px, sizeof px, &got);
    uint32_t sum = 2166136261u, lit = 0;
    for (uint32_t i = 0; i < got; i++) { sum ^= px[i]; sum *= 16777619u; }
    for (uint32_t i = 0; i + 3 < got; i += 4) if (px[i] || px[i+1] || px[i+2]) lit++;
    printf("    getBytes          -> %s %u bytes\n", statname(st), got);
    printf("    fnv1a checksum    -> 0x%08x\n", sum);
    printf("    non-black pixels  -> %u / %u (%.1f%%)\n", lit, got/4, 100.0*lit/(got/4.0));
    printf("    %s\n", (lit > 400 && lit < 2600)
        ? "TRIANGLE RENDERED ON THE HOST GPU" : "*** unexpected coverage ***");

    call(RM_OP_STATS, NULL, 0, &ru, sizeof ru, NULL);
    printf("\n  live handles on host: %llu\n", (unsigned long long)ru.value);
    close(g_fd);
    return 0;
}
