/* Host-side decoder: a contiguous wire batch -> real Metal encoder calls.
 *
 * Every field arriving here is attacker-controlled as far as this code is
 * concerned, so nothing is trusted: record sizes are checked against their own
 * struct, sidecar references are range-checked with overflow-safe arithmetic,
 * and handles go through the generation-tagged table rather than being cast.
 *
 * An unimplemented opcode is NAMED -- encoder kind, opcode, record index --
 * because a silent skip renders a subtly wrong frame on a machine one hop
 * away from the developer, which is close to undebuggable.
 */
#ifndef WMT_DECODE_H
#define WMT_DECODE_H
#include "../wmt_wire.h"

enum wmtw_dec_status {
    WMTW_DEC_OK = 0,
    WMTW_DEC_BAD_VERSION,
    WMTW_DEC_BAD_SIZE,
    WMTW_DEC_TRUNCATED,
    WMTW_DEC_TOO_MANY,
    WMTW_DEC_UNIMPLEMENTED_OP,
    WMTW_DEC_SIDECAR_RANGE,
    WMTW_DEC_BAD_HANDLE,
    WMTW_DEC_NOT_EXACT,      /* records did not exactly fill the region */
};

struct wmtw_dec_result {
    enum wmtw_dec_status status;
    uint32_t record_index;
    uint32_t opcode;
    uint32_t encoder_kind;
};

static inline const char *wmtw_dec_strerror(enum wmtw_dec_status s) {
    switch (s) {
    case WMTW_DEC_OK: return "ok";
    case WMTW_DEC_BAD_VERSION: return "record version mismatch";
    case WMTW_DEC_BAD_SIZE: return "record smaller than its own struct";
    case WMTW_DEC_TRUNCATED: return "record runs past the batch";
    case WMTW_DEC_TOO_MANY: return "record cap exceeded";
    case WMTW_DEC_UNIMPLEMENTED_OP: return "opcode not implemented on the host";
    case WMTW_DEC_SIDECAR_RANGE: return "sidecar reference outside the region";
    case WMTW_DEC_BAD_HANDLE: return "handle stale, unknown, or wrong class";
    case WMTW_DEC_NOT_EXACT: return "records do not exactly fill the batch";
    }
    return "?";
}

/* Minimum byte count for each opcode. A record claiming a size smaller than
 * its own struct would otherwise be read past its end. */
static inline uint32_t wmtw_min_size(uint16_t op) {
    switch (op) {
    case WMTW_OP_Nop:                        return sizeof(struct wmtw_nop);
    case WMTW_OP_UseResource:                return sizeof(struct wmtw_useresource);
    case WMTW_OP_SetVertexBuffer:            return sizeof(struct wmtw_setvertexbuffer);
    case WMTW_OP_SetVertexBufferOffset:      return sizeof(struct wmtw_setvertexbufferoffset);
    case WMTW_OP_SetFragmentBuffer:          return sizeof(struct wmtw_setfragmentbuffer);
    case WMTW_OP_SetFragmentTexture:         return sizeof(struct wmtw_setfragmenttexture);
    case WMTW_OP_SetFragmentBytes:           return sizeof(struct wmtw_setfragmentbytes);
    case WMTW_OP_SetRasterizerState:         return sizeof(struct wmtw_setrasterizerstate);
    case WMTW_OP_SetViewports:               return sizeof(struct wmtw_setviewports);
    case WMTW_OP_SetScissorRects:            return sizeof(struct wmtw_setscissorrects);
    case WMTW_OP_SetPSO:                     return sizeof(struct wmtw_setpso);
    case WMTW_OP_SetDSSO:                    return sizeof(struct wmtw_setdsso);
    case WMTW_OP_SetBlendFactorAndStencilRef:return sizeof(struct wmtw_setblendfactorandstencilref);
    case WMTW_OP_Draw:                       return sizeof(struct wmtw_draw);
    case WMTW_OP_DrawIndexed:                return sizeof(struct wmtw_drawindexed);
    default:                                 return 0;   /* unknown */
    }
}

/* Overflow-safe: offset and count are 32-bit from the wire, so the product is
 * computed in 64-bit before comparison. offset+count in 32-bit could wrap and
 * pass a naive check. */
static inline int wmtw_side_ok(uint32_t off, uint32_t count, uint32_t elem, uint32_t region) {
    uint64_t need = (uint64_t)count * elem;
    if (need > WMTW_MAX_SIDECAR_BYTES) return 0;
    return (uint64_t)off + need <= region;
}

#endif
