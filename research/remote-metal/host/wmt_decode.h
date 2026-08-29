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


/* THE batch validator. Tests and rmetald must both call this one function --
 * a test that walks the stream its own way proves only that the test works.
 *
 * Validates the prologue, then every record: version, minimum size for its own
 * opcode, containment, and the range of every sidecar reference. Requires the
 * records to fill the region EXACTLY, so trailing garbage is an error rather
 * than something a decoder silently stops before.
 *
 * All range arithmetic is 64-bit or subtraction-based: `off + size > bytes`
 * can wrap on 32-bit inputs from the wire and let an oversized record through.
 */
static inline enum wmtw_dec_status
wmtw_validate_batch(const struct wmtw_batch *b, const uint8_t *rec, const uint8_t *side,
                    struct wmtw_dec_result *res)
{
    uint32_t off = 0, n = 0;
    res->record_index = 0; res->opcode = 0;
    res->encoder_kind = b ? b->encoder_kind : 0;

#define DFAIL(st) do { res->status = (st); res->record_index = n; return (st); } while (0)

    if (!b || b->magic != WMTW_BATCH_MAGIC) DFAIL(WMTW_DEC_BAD_VERSION);
    if (b->version != WMTW_VERSION)         DFAIL(WMTW_DEC_BAD_VERSION);
    if (b->record_bytes > WMTW_MAX_BATCH_BYTES)   DFAIL(WMTW_DEC_TRUNCATED);
    if (b->sidecar_bytes > WMTW_MAX_SIDECAR_BYTES) DFAIL(WMTW_DEC_SIDECAR_RANGE);
    if (b->record_count > WMTW_MAX_RECORDS)  DFAIL(WMTW_DEC_TOO_MANY);
    (void)side;

    while (off < b->record_bytes) {
        if (b->record_bytes - off < sizeof(struct wmtw_hdr)) DFAIL(WMTW_DEC_TRUNCATED);
        const struct wmtw_hdr *h = (const struct wmtw_hdr *)(rec + off);
        res->opcode = h->op;
        if (h->version != WMTW_VERSION) DFAIL(WMTW_DEC_BAD_VERSION);
        uint32_t min = wmtw_min_size(h->op);
        if (!min)                       DFAIL(WMTW_DEC_UNIMPLEMENTED_OP);
        if (h->size < min)              DFAIL(WMTW_DEC_BAD_SIZE);
        /* subtraction, never off + size: the latter can wrap */
        if (h->size > b->record_bytes - off) DFAIL(WMTW_DEC_TRUNCATED);
        if (n >= WMTW_MAX_RECORDS)      DFAIL(WMTW_DEC_TOO_MANY);

        switch (h->op) {
        case WMTW_OP_SetFragmentBytes: {
            const struct wmtw_setfragmentbytes *r = (const void *)h;
            if (!wmtw_side_ok(r->bytes_offset, r->bytes_count, 1, b->sidecar_bytes))
                DFAIL(WMTW_DEC_SIDECAR_RANGE);
            break;
        }
        case WMTW_OP_SetViewports: {
            const struct wmtw_setviewports *r = (const void *)h;
            if (r->viewports_count > WMTW_MAX_ARRAY_COUNT) DFAIL(WMTW_DEC_SIDECAR_RANGE);
            if (!wmtw_side_ok(r->viewports_offset, r->viewports_count,
                              sizeof(struct wmtw_viewport), b->sidecar_bytes))
                DFAIL(WMTW_DEC_SIDECAR_RANGE);
            break;
        }
        case WMTW_OP_SetScissorRects: {
            const struct wmtw_setscissorrects *r = (const void *)h;
            if (r->scissors_count > WMTW_MAX_ARRAY_COUNT) DFAIL(WMTW_DEC_SIDECAR_RANGE);
            if (!wmtw_side_ok(r->scissors_offset, r->scissors_count,
                              sizeof(struct wmtw_scissor), b->sidecar_bytes))
                DFAIL(WMTW_DEC_SIDECAR_RANGE);
            break;
        }
        default: break;
        }
        off += h->size; n++;
    }
    if (off != b->record_bytes)   DFAIL(WMTW_DEC_NOT_EXACT);
    if (n != b->record_count)     DFAIL(WMTW_DEC_NOT_EXACT);
#undef DFAIL
    res->status = WMTW_DEC_OK;
    res->record_index = n;
    return WMTW_DEC_OK;
}

#endif
