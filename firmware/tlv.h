/*
 * TLV codec for the Ableton Link payload format described in
 * LINK_PROTOCOL_SPEC.md §3.6 / §3.7.
 *
 *   PayloadEntryHeader { u32 key (BE), u32 size (BE) } || value[size]
 *
 * The codec is byte-iterator based and does not allocate. Encoding works
 * by appending entries to a caller-supplied buffer; decoding works by
 * walking the buffer and dispatching on key.
 *
 * Endianness helpers assume the host is little-endian (true for VexRiscv).
 */

#ifndef LINKFPGA_TLV_H
#define LINKFPGA_TLV_H

#include <stddef.h>
#include <stdint.h>

/* 4CC keys (defined in LINK_PROTOCOL_SPEC.md Appendix A). */
#define KEY_TMLN  0x746D6C6Eu  /* Timeline */
#define KEY_SESS  0x73657373u  /* SessionMembership */
#define KEY_STST  0x73747374u  /* StartStopState */
#define KEY_MEP4  0x6D657034u  /* IPv4 measurement endpoint */
#define KEY_MEP6  0x6D657036u  /* IPv6 measurement endpoint */
#define KEY_AEP4  0x61657034u  /* IPv4 audio endpoint */
#define KEY_AEP6  0x61657036u  /* IPv6 audio endpoint */
#define KEY_HT    0x5F5F6874u  /* HostTime  '__ht' */
#define KEY_GT    0x5F5F6774u  /* GHostTime '__gt' */
#define KEY_PGT   0x5F706774u  /* PrevGHostTime '_pgt' */
#define KEY_PI    0x5F5F7069u  /* PeerInfo  '__pi' */
#define KEY_AUCA  0x61756361u  /* ChannelAnnouncements */
#define KEY_AUCB  0x61756362u  /* ChannelByes */
#define KEY_CHID  0x63686964u  /* ChannelId */

/* Big-endian primitive accessors. */
static inline uint16_t be16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | p[1];
}
static inline uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}
static inline uint64_t be64(const uint8_t *p) {
    return ((uint64_t)be32(p) << 32) | (uint64_t)be32(p + 4);
}
static inline void put_be16(uint8_t *p, uint16_t v) {
    p[0] = v >> 8; p[1] = v;
}
static inline void put_be32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
static inline void put_be64(uint8_t *p, uint64_t v) {
    put_be32(p, v >> 32);
    put_be32(p + 4, (uint32_t)v);
}

/* Encode helpers. They append to `*pp`, advancing the pointer. They do
 * not bounds-check; the caller is expected to know the buffer is large
 * enough (LINK_MAX_MSG_*). */
void tlv_emit_header(uint8_t **pp, uint32_t key, uint32_t size);
void tlv_emit_u8     (uint8_t **pp, uint32_t key, uint8_t v);
void tlv_emit_u32be  (uint8_t **pp, uint32_t key, uint32_t v);
void tlv_emit_u64be  (uint8_t **pp, uint32_t key, uint64_t v);
void tlv_emit_bytes  (uint8_t **pp, uint32_t key, const uint8_t *src, uint32_t n);

/* Decoder: walks `[buf, buf+len)` and invokes `cb(key, value, vlen, ctx)`
 * for each entry. Returns 0 on success, -1 on a malformed entry. */
typedef int (*tlv_cb_t)(uint32_t key, const uint8_t *value, uint32_t vlen, void *ctx);
int tlv_walk(const uint8_t *buf, size_t len, tlv_cb_t cb, void *ctx);

#endif /* LINKFPGA_TLV_H */
