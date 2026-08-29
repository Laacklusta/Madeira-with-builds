/* Remote Metal transport -- shared wire protocol.
 *
 * Forwards winemetal's semantic seam to a real Metal device on a macOS host,
 * so a virtualised iOS guest can render on hardware its own paravirtual Metal
 * cannot reach (no mesh shaders, no BC, no GPU family reported at all).
 *
 * ⚠️ winemetal is a SEMANTIC seam, not a wire protocol. Two things it does
 * today are only legal because guest and host share one address space:
 *
 *   1. obj_handle_t is a raw Objective-C pointer CAST to uint64_t --
 *      `params->ret = (obj_handle_t)[array objectAtIndex:i]`. Across a machine
 *      boundary that is meaningless and unsafe, so this protocol never puts a
 *      host pointer on the wire. Handles are generation-tagged table indices.
 *   2. wmtcmd_* command lists are LINKED THROUGH GUEST POINTERS and carry
 *      further pointers for inline bytes, viewports and scissors. Those must
 *      become contiguous records with offsets before command encoding can be
 *      forwarded. This spike deliberately stops short of that.
 *
 * Synchronous request/response by design. Async completion handlers across a
 * machine boundary are how you get non-deterministic hangs, and correctness
 * comes before throughput here.
 */
#ifndef REMOTE_METAL_PROTOCOL_H
#define REMOTE_METAL_PROTOCOL_H

#include <stdint.h>

#define RM_MAGIC   0x4C544D52u   /* 'RMTL' little-endian */
#define RM_VERSION 1u
#define RM_PORT    47821

enum rm_op {
    RM_OP_PING = 1,          /* latency floor, no Metal work        */
    RM_OP_COPY_ALL_DEVICES,  /* -> handle to NSArray<MTLDevice>     */
    RM_OP_ARRAY_COUNT,       /* handle -> u64                       */
    RM_OP_ARRAY_OBJECT,      /* handle, index -> handle             */
    RM_OP_DEVICE_NAME,       /* handle -> utf8 bytes                */
    RM_OP_SUPPORTS_FAMILY,   /* handle, family -> u64 (bool)        */
    RM_OP_SUPPORTS_BC,       /* handle -> u64 (bool)                */
    RM_OP_ALLOCATED_SIZE,    /* handle -> u64                       */
    RM_OP_RETAIN,            /* handle -> status                    */
    RM_OP_RELEASE,           /* handle -> status                    */
    RM_OP_STATS,             /* -> live handle count                */

    /* --- offscreen render milestone ------------------------------------ */
    RM_OP_NEW_COMMAND_QUEUE, /* device -> handle                          */
    RM_OP_NEW_BUFFER,        /* device, len, [inline bytes] -> handle     */
    RM_OP_NEW_TEXTURE,       /* device, fmt, w, h -> handle               */
    RM_OP_NEW_LIBRARY,       /* device, [utf8 source] -> handle           */
    RM_OP_NEW_FUNCTION,      /* library, [utf8 name] -> handle            */
    RM_OP_NEW_RENDER_PIPELINE, /* device, vfn, ffn, fmt -> handle         */
    RM_OP_SUBMIT_RENDER_PASS,  /* one round trip: encode, commit, wait    */
    RM_OP_TEXTURE_GETBYTES,    /* texture -> raw pixels                   */

    /* --- presentation ------------------------------------------------- */
    RM_OP_NEXT_DRAWABLE,       /* -> drawable+texture, or RM_ERR_NO_DRAWABLE */
    RM_OP_LAYER_SIZE,          /* -> current host layer size                 */
};

struct rm_ret_drawable { uint64_t drawable; uint64_t texture; uint64_t width; uint64_t height; };

/* ---- contiguous command stream -----------------------------------------
 *
 * winemetal's wmtcmd_* lists are linked through GUEST POINTERS and carry
 * further pointers for inline bytes, viewports and scissors -- none of which
 * can cross a machine boundary. This is the shape they have to become: a
 * self-describing contiguous byte stream where every record states its own
 * size, and variable-length data is inline rather than referenced.
 *
 * Walk it with `off += rec->size`, never by following a pointer. */
enum rm_enc {
    RM_ENC_SET_PIPELINE = 1,   /* handle                               */
    RM_ENC_SET_VERTEX_BUFFER,  /* handle, offset, index                */
    RM_ENC_SET_VERTEX_BYTES,   /* index, len, inline bytes             */
    RM_ENC_SET_VIEWPORT,       /* x, y, w, h, znear, zfar (inline)     */
    RM_ENC_DRAW,               /* primitive, start, count              */
};

struct rm_enc_hdr { uint16_t type; uint16_t pad; uint32_t size; };   /* size covers hdr+body */

struct rm_enc_pipeline { struct rm_enc_hdr h; uint64_t pipeline; };
struct rm_enc_vbuf     { struct rm_enc_hdr h; uint64_t buffer; uint64_t offset; uint64_t index; };
struct rm_enc_draw     { struct rm_enc_hdr h; uint64_t primitive; uint64_t start; uint64_t count; };
struct rm_enc_viewport { struct rm_enc_hdr h; double x, y, w, h_, znear, zfar; };

/* Render pass: descriptor, then `cmd_bytes` of the stream above. One round
 * trip encodes, commits and waits -- which is where the batching win is. */
struct rm_render_pass {
    uint64_t queue;
    uint64_t color_texture;
    /* When non-zero, presentDrawable is encoded on the SAME command buffer
     * before commit -- which is Metal's intended sequencing. Presenting from a
     * separate call after the render buffer has already committed would be
     * wrong. The handle is consumed and invalidated by this submit, so a
     * drawable cannot leak or be presented twice. */
    uint64_t present_drawable;
    double   clear_r, clear_g, clear_b, clear_a;
    uint32_t cmd_bytes;
    uint32_t reserved;
};

struct rm_new_buffer  { uint64_t device; uint64_t length; };  /* + inline bytes */
struct rm_new_texture { uint64_t device; uint64_t pixel_format; uint64_t width; uint64_t height; };
struct rm_new_pipeline{ uint64_t device; uint64_t vfn; uint64_t ffn; uint64_t pixel_format; };

enum rm_status {
    RM_OK = 0,
    RM_ERR_BAD_MAGIC,
    RM_ERR_BAD_VERSION,
    RM_ERR_BAD_OPCODE,
    RM_ERR_STALE_HANDLE,   /* generation mismatch: released, then reused */
    RM_ERR_BAD_HANDLE,     /* out of range / never allocated            */
    RM_ERR_WRONG_CLASS,    /* handle valid but not the expected class   */
    RM_ERR_SHORT_PAYLOAD,
    RM_ERR_NO_DRAWABLE,    /* nextDrawable timed out -- a distinct answer, not a hang */
};

/* Every frame, both directions. payload_len counts bytes AFTER this header. */
struct rm_hdr {
    uint32_t magic;
    uint16_t version;
    uint16_t opcode;
    uint32_t seq;          /* echoed, so a desync is detectable */
    uint32_t status;       /* request: 0. response: enum rm_status */
    uint32_t payload_len;
    uint32_t reserved;
};

/* A handle is (generation << 32) | slot. Generation increments on reuse, so a
 * stale handle is REJECTED rather than silently addressing a different object
 * -- the failure mode that would otherwise appear as inexplicable corruption. */
#define RM_HANDLE(gen, slot) (((uint64_t)(gen) << 32) | (uint32_t)(slot))
#define RM_HANDLE_GEN(h)     ((uint32_t)((h) >> 32))
#define RM_HANDLE_SLOT(h)    ((uint32_t)((h) & 0xffffffffu))
#define RM_NULL_HANDLE       0ull

struct rm_arg_handle       { uint64_t handle; };
struct rm_arg_handle_u64   { uint64_t handle; uint64_t arg; };
struct rm_ret_u64          { uint64_t value; };
struct rm_ret_handle       { uint64_t handle; };

#endif
