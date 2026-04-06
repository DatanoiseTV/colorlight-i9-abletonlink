#include "tlv.h"

void tlv_emit_header(uint8_t **pp, uint32_t key, uint32_t size) {
    uint8_t *p = *pp;
    put_be32(p, key);
    put_be32(p + 4, size);
    *pp = p + 8;
}

void tlv_emit_u8(uint8_t **pp, uint32_t key, uint8_t v) {
    tlv_emit_header(pp, key, 1);
    *(*pp)++ = v;
}

void tlv_emit_u32be(uint8_t **pp, uint32_t key, uint32_t v) {
    tlv_emit_header(pp, key, 4);
    put_be32(*pp, v);
    *pp += 4;
}

void tlv_emit_u64be(uint8_t **pp, uint32_t key, uint64_t v) {
    tlv_emit_header(pp, key, 8);
    put_be64(*pp, v);
    *pp += 8;
}

void tlv_emit_bytes(uint8_t **pp, uint32_t key,
                    const uint8_t *src, uint32_t n) {
    tlv_emit_header(pp, key, n);
    for (uint32_t i = 0; i < n; i++) (*pp)[i] = src[i];
    *pp += n;
}

int tlv_walk(const uint8_t *buf, size_t len, tlv_cb_t cb, void *ctx) {
    const uint8_t *p   = buf;
    const uint8_t *end = buf + len;
    while (p + 8 <= end) {
        uint32_t key  = be32(p);
        uint32_t size = be32(p + 4);
        const uint8_t *vp = p + 8;
        if (vp + size > end) return -1;
        if (cb(key, vp, size, ctx) != 0) return -1;
        p = vp + size;
    }
    return (p == end) ? 0 : -1;
}
