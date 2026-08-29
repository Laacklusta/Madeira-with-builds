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
};

enum rm_status {
    RM_OK = 0,
    RM_ERR_BAD_MAGIC,
    RM_ERR_BAD_VERSION,
    RM_ERR_BAD_OPCODE,
    RM_ERR_STALE_HANDLE,   /* generation mismatch: released, then reused */
    RM_ERR_BAD_HANDLE,     /* out of range / never allocated            */
    RM_ERR_WRONG_CLASS,    /* handle valid but not the expected class   */
    RM_ERR_SHORT_PAYLOAD,
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
