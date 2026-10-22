/* Minimal AWDL action-frame builder, layout from owly-awdl (MIT). */
#ifndef AWDL_TLV_H
#define AWDL_TLV_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define AWDL_BSSID ((uint8_t[]){0x00,0x25,0x00,0xff,0x94,0x73})
#define AWDL_OUI   ((uint8_t[]){0x00,0x17,0xf2})
#define AWDL_TYPE  8
#define AWDL_VER   0x10
#define AWDL_PSF   0
#define AWDL_MIF   3
#define AWDL_SLOTS 16
#define AWDL_AW_TU 16
#define AWDL_PRESENCE 4
#define AWDL_PSF_TU 110

struct awdl_buf { uint8_t *p; size_t n, cap; };

static inline void ab_init(struct awdl_buf *b, uint8_t *p, size_t cap) {
    b->p = p; b->n = 0; b->cap = cap;
}
static inline int ab_need(struct awdl_buf *b, size_t k) {
    return b->n + k <= b->cap;
}
static inline void ab_u8(struct awdl_buf *b, uint8_t v) {
    if (ab_need(b,1)) b->p[b->n++] = v;
}
static inline void ab_le16(struct awdl_buf *b, uint16_t v) {
    if (ab_need(b,2)) { b->p[b->n++]=v&0xff; b->p[b->n++]=(v>>8)&0xff; }
}
static inline void ab_le32(struct awdl_buf *b, uint32_t v) {
    if (ab_need(b,4)) {
        b->p[b->n++]=v&0xff; b->p[b->n++]=(v>>8)&0xff;
        b->p[b->n++]=(v>>16)&0xff; b->p[b->n++]=(v>>24)&0xff;
    }
}
static inline void ab_be16(struct awdl_buf *b, uint16_t v) {
    if (ab_need(b,2)) { b->p[b->n++]=(v>>8)&0xff; b->p[b->n++]=v&0xff; }
}
static inline void ab_mac(struct awdl_buf *b, const uint8_t m[6]) {
    if (ab_need(b,6)) { memcpy(b->p+b->n, m, 6); b->n += 6; }
}
static inline void ab_bytes(struct awdl_buf *b, const void *p, size_t n) {
    if (ab_need(b,n)) { memcpy(b->p+b->n, p, n); b->n += n; }
}

/* Start a TLV; returns offset of the length field. */
static inline size_t ab_tlv_begin(struct awdl_buf *b, uint8_t kind) {
    ab_u8(b, kind); size_t at = b->n; ab_le16(b, 0); return at;
}
static inline void ab_tlv_end(struct awdl_buf *b, size_t at) {
    uint16_t len = (uint16_t)(b->n - at - 2);
    b->p[at] = len & 0xff; b->p[at+1] = (len >> 8) & 0xff;
}

struct awdl_own {
    uint8_t  addr[6];
    uint8_t  channel;          /* current radio channel (149) */
    uint8_t  master_channel;   /* home channel */
    uint16_t next_tu;          /* TU to next slot boundary */
    uint16_t aw_counter;
    uint16_t psf_interval_tu;
    uint8_t  master[6];
    uint8_t  sync[6];
    uint32_t height, master_metric, self_metric;
    uint32_t master_counter, self_counter;
    const char *name;          /* host name for MIF ARPA */
    uint8_t  seq_slots[AWDL_SLOTS][2]; /* opclass encoding */
};

static inline void awdl_seq_fixed(struct awdl_own *o, uint8_t ch) {
    uint8_t opc = (ch <= 14) ? 0x51 : 0x80;
    for (int i = 0; i < AWDL_SLOTS; i++) {
        o->seq_slots[i][0] = ch; o->seq_slots[i][1] = opc;
    }
}

static inline void awdl_put_seq_body(struct awdl_buf *b, const struct awdl_own *o) {
    ab_u8(b, AWDL_SLOTS - 1);
    ab_u8(b, 3);              /* OpClass encoding */
    ab_u8(b, 0);              /* duplicate count */
    ab_u8(b, 3);              /* step count */
    ab_le16(b, 0xffff);       /* fill channel */
    for (int i = 0; i < AWDL_SLOTS; i++)
        ab_bytes(b, o->seq_slots[i], 2);
}

static inline void awdl_put_sync(struct awdl_buf *b, const struct awdl_own *o) {
    size_t at = ab_tlv_begin(b, 4);
    uint8_t ext = AWDL_PRESENCE - 1;
    ab_u8(b, o->channel);
    ab_le16(b, o->next_tu);
    ab_u8(b, o->master_channel);
    ab_u8(b, 0);                          /* guard */
    ab_le16(b, AWDL_AW_TU);
    ab_le16(b, o->psf_interval_tu);
    ab_le16(b, 0x1800);
    ab_le16(b, AWDL_AW_TU);
    ab_le16(b, AWDL_AW_TU);
    {
        uint16_t elapsed = AWDL_AW_TU * AWDL_PRESENCE -
            (o->next_tu < 64 ? o->next_tu : 64);
        uint16_t rem = AWDL_AW_TU > elapsed ? AWDL_AW_TU - elapsed : 0;
        ab_le16(b, rem);
    }
    ab_u8(b, ext); ab_u8(b, ext); ab_u8(b, ext); ab_u8(b, ext);
    ab_mac(b, o->master);
    ab_u8(b, AWDL_PRESENCE);
    ab_u8(b, 0);
    ab_le16(b, o->aw_counter);
    ab_le16(b, o->aw_counter);
    awdl_put_seq_body(b, o);
    ab_u8(b, 0); ab_u8(b, 0);
    ab_tlv_end(b, at);
}

static inline void awdl_put_election_v1(struct awdl_buf *b, const struct awdl_own *o) {
    size_t at = ab_tlv_begin(b, 5);
    ab_u8(b, 0); ab_le16(b, 0);
    ab_u8(b, (uint8_t)(o->height > 255 ? 255 : o->height));
    ab_u8(b, 0);
    ab_mac(b, o->master);
    ab_le32(b, o->master_metric);
    ab_le32(b, o->self_metric);
    ab_u8(b, 0); ab_u8(b, 0);
    ab_tlv_end(b, at);
}

static inline void awdl_put_chseq(struct awdl_buf *b, const struct awdl_own *o) {
    size_t at = ab_tlv_begin(b, 18);
    awdl_put_seq_body(b, o);
    ab_u8(b, 0); ab_u8(b, 0); ab_u8(b, 0);
    ab_tlv_end(b, at);
}

static inline void awdl_put_election_v2(struct awdl_buf *b, const struct awdl_own *o) {
    size_t at = ab_tlv_begin(b, 24);
    ab_mac(b, o->master);
    ab_mac(b, o->sync);
    ab_le32(b, o->master_counter);
    ab_le32(b, o->height);
    ab_le32(b, o->master_metric);
    ab_le32(b, o->self_metric);
    ab_le32(b, 0); ab_le32(b, 0);
    ab_le32(b, o->self_counter);
    ab_tlv_end(b, at);
}

static inline void awdl_put_service(struct awdl_buf *b) {
    size_t at = ab_tlv_begin(b, 6);
    uint8_t z[3] = {0,0,0};
    ab_bytes(b, z, 3); ab_le16(b, 0); ab_le32(b, 0);
    ab_tlv_end(b, at);
}

static inline void awdl_put_ht(struct awdl_buf *b) {
    size_t at = ab_tlv_begin(b, 7);
    ab_le16(b, 0);
    ab_le16(b, 0x11ce);       /* ONE_STREAM caps */
    ab_u8(b, 0x1b);
    ab_u8(b, 0xff);
    ab_le16(b, 0);
    ab_tlv_end(b, at);
}

static inline void awdl_put_arpa(struct awdl_buf *b, const char *name) {
    size_t at = ab_tlv_begin(b, 16);
    size_t n = name ? strlen(name) : 0;
    if (n > 64) n = 64;
    ab_u8(b, 3);
    ab_u8(b, (uint8_t)n);
    if (n) ab_bytes(b, name, n);
    ab_be16(b, 0xc00c);
    ab_tlv_end(b, at);
}

static inline void awdl_put_datapath(struct awdl_buf *b, const struct awdl_own *o) {
    size_t at = ab_tlv_begin(b, 12);
    ab_le16(b, 0x8f24);
    ab_bytes(b, "X0\0", 3);
    ab_le16(b, o->master_channel == 6 ? 0x0001 :
               o->master_channel == 44 ? 0x0002 : 0x0004);
    ab_mac(b, o->addr);
    ab_le16(b, 0);
    ab_tlv_end(b, at);
}

static inline void awdl_put_version(struct awdl_buf *b) {
    size_t at = ab_tlv_begin(b, 21);
    ab_u8(b, 0x34);           /* AWDL 3.4 */
    ab_u8(b, 1);              /* macOS device class */
    ab_tlv_end(b, at);
}

static inline void awdl_put_all_tlvs(struct awdl_buf *b, const struct awdl_own *o, int mif) {
    awdl_put_sync(b, o);
    awdl_put_election_v1(b, o);
    awdl_put_chseq(b, o);
    awdl_put_election_v2(b, o);
    awdl_put_service(b);
    if (mif) {
        awdl_put_ht(b);
        awdl_put_arpa(b, o->name ? o->name : "rz616");
    }
    awdl_put_datapath(b, o);
    awdl_put_version(b);
}

/* Build a complete 802.11 action frame (no FCS). Returns length. */
static inline size_t awdl_build_action(uint8_t *out, size_t cap,
    const struct awdl_own *o, uint8_t subtype, uint16_t seq, uint32_t time_us)
{
    struct awdl_buf b;
    uint8_t bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
    ab_init(&b, out, cap);
    ab_le16(&b, 0x00d0);                  /* action */
    ab_le16(&b, 0);                       /* duration */
    ab_mac(&b, bcast);
    ab_mac(&b, o->addr);
    ab_mac(&b, AWDL_BSSID);
    ab_le16(&b, (uint16_t)((seq & 0x0fff) << 4));
    ab_u8(&b, 127);
    ab_bytes(&b, AWDL_OUI, 3);
    ab_u8(&b, AWDL_TYPE);
    ab_u8(&b, AWDL_VER);
    ab_u8(&b, subtype);
    ab_u8(&b, 0);
    ab_le32(&b, time_us);
    ab_le32(&b, time_us);
    awdl_put_all_tlvs(&b, o, subtype == AWDL_MIF);
    return b.n;
}

/* Parse helpers for the RX dump / peer check. */
static inline int awdl_is_action(const uint8_t *f, size_t n) {
    return n >= 40 && f[0] == 0xd0 && f[24] == 127 &&
        f[25]==0x00 && f[26]==0x17 && f[27]==0xf2 && f[28]==8;
}
static inline int awdl_is_data(const uint8_t *f, size_t n) {
    return n >= 24 && (f[0] & 0x0c) == 0x08 &&
        f[16]==0x00 && f[17]==0x25 && f[18]==0x00 &&
        f[19]==0xff && f[20]==0x94 && f[21]==0x73;
}
static inline uint8_t awdl_af_subtype(const uint8_t *f) { return f[30]; }

/* Walk TLVs; cb(kind, value, vlen, ctx). Returns 0 on success. */
typedef void (*awdl_tlv_cb)(uint8_t kind, const uint8_t *v, size_t n, void *ctx);
static inline int awdl_walk_tlvs(const uint8_t *body, size_t n, awdl_tlv_cb cb, void *ctx) {
    size_t i = 0;
    while (i + 3 <= n) {
        uint8_t kind = body[i];
        uint16_t len = (uint16_t)(body[i+1] | (body[i+2] << 8));
        if (i + 3 + len > n) return -1;
        if (cb) cb(kind, body + i + 3, len, ctx);
        i += 3 + len;
    }
    return 0;
}

/* Sync-tree / election master in TLV 4 is at value offset 21. */
static inline int awdl_tlv4_master(const uint8_t *v, size_t n, uint8_t out[6]) {
    if (n < 27) return -1;
    memcpy(out, v + 21, 6);
    return 0;
}
/* Election v2 (24): master at 0, sync at 6. */
static inline int awdl_tlv24_sync(const uint8_t *v, size_t n, uint8_t out[6]) {
    if (n < 12) return -1;
    memcpy(out, v + 6, 6);
    return 0;
}

#endif
