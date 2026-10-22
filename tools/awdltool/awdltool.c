/*
 * awdltool — M3a userspace for AirportMtkWlan's AWDL raw channel.
 *
 * Build (Xcode CLT, no Homebrew):
 *   clang -O2 -Wall -o awdltool awdltool.c -framework IOKit -framework CoreFoundation
 *
 * Needs root (IOUserClient is administrator-only) AND the kext booted with
 * -mtkawdltx. Without root the open fails cleanly.
 *
 * Subcommands:
 *   info                         print channel state
 *   dump [-t SEC] [-o FILE.pcap] capture AWDL frames to radiotap pcap
 *   sync [-t SEC] [-n NAME]      send PSF/MIF while dumping; report whether
 *                                the iPhone starts addressing us
 *   -s MS (dump/sync)            print a machine-readable "STAT k=v ..." line
 *                                every MS milliseconds (live progress)
 *
 * m3a-2 (userspace only): RX/TXS reads use a 4096-byte in-band buffer. Bigger
 * buffers are passed out-of-line by IOKit, the M3a userclient then sees
 * structureOutput == NULL and returns kIOReturnBadArgument, so m3a-1 never
 * received a single frame. Frames older than the start of the run (left in
 * the 64-slot kernel ring) are discarded as "stale".
 *   probe -d DST [-n N] [-r KBPS] send N fixed-rate unicast data frames and
 *                                print TXS (ACK) if firmware reports them
 *
 * Layout of PSF/MIF TLVs follows owly-awdl (MIT). GPLv2 for this tool.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <mach/mach_time.h>
#include <IOKit/IOKitLib.h>
#ifndef kIOMainPortDefault
#define kIOMainPortDefault kIOMasterPortDefault
#endif
#include "../ClientKit/AwdlRaw.h"
#include "awdl_tlv.h"

static volatile int g_stop;

static void on_sig(int s) { (void)s; g_stop = 1; }

static uint64_t now_us(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}

/* Same clock as the kernel's rxrec.ts_us (mach_absolute_time in us). */
static uint64_t uptime_us(void) {
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000ull;
}

/* IOKit passes struct outputs > sizeof(io_struct_inband_t) out-of-line; the
 * M3a userclient only handles in-band output. Never exceed this. */
#define AWDLTOOL_INBAND 4096

/* ---------- IOKit ---------- */

struct conn {
    io_connect_t connect;
    io_service_t service;
};

static int conn_open(struct conn *c) {
    CFMutableDictionaryRef m;
    io_iterator_t it;
    io_service_t s;
    kern_return_t kr;

    memset(c, 0, sizeof(*c));
    m = IOServiceMatching("AirportItlwm");
    if (!m) { fprintf(stderr, "IOServiceMatching failed\n"); return -1; }
    kr = IOServiceGetMatchingServices((kIOMainPortDefault), m, &it);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceGetMatchingServices: 0x%x\n", kr);
        return -1;
    }
    s = IOIteratorNext(it);
    IOObjectRelease(it);
    if (!s) {
        fprintf(stderr, "AirportItlwm not found (is the kext loaded?)\n");
        return -1;
    }
    kr = IOServiceOpen(s, mach_task_self(), MTK_AWDLRAW_USERCLIENT_TYPE, &c->connect);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceOpen(type=AWDT): 0x%x", kr);
        if (kr == kIOReturnNotPrivileged || kr == kIOReturnNotPermitted)
            fprintf(stderr, " (need root: sudo)\n");
        else if (kr == kIOReturnNotAttached || kr == kIOReturnUnsupported)
            fprintf(stderr, " (boot with -mtkawdltx; rebuild with MTK_AWDL_TX)\n");
        else
            fprintf(stderr, "\n");
        IOObjectRelease(s);
        return -1;
    }
    c->service = s;
    return 0;
}

static void conn_close(struct conn *c) {
    if (c->connect) IOServiceClose(c->connect);
    if (c->service) IOObjectRelease(c->service);
    memset(c, 0, sizeof(*c));
}

static int do_info(struct conn *c, struct mtk_awdlraw_info *out) {
    size_t outsz = sizeof(*out);
    kern_return_t kr = IOConnectCallStructMethod(c->connect, kMtkAwdlRawGetInfo,
        NULL, 0, out, &outsz);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "GetInfo: 0x%x\n", kr);
        return -1;
    }
    return 0;
}

static int do_tx(struct conn *c, const uint8_t *frame, uint32_t len,
    uint32_t rate_kbps, uint32_t flags, struct mtk_awdlraw_txack *ack)
{
    struct mtk_awdlraw_tx req;
    size_t in = offsetof(struct mtk_awdlraw_tx, frame) + len;
    size_t out = sizeof(*ack);
    kern_return_t kr;
    memset(&req, 0, sizeof(req));
    req.rate_kbps = rate_kbps;
    { const char *e = getenv("MTK_AWDL_NO_TXS");
      if (e != NULL && e[0] == '1') flags &= ~MTK_AWDLRAW_TX_WANT_TXS; }
    req.flags = flags;
    req.len = len;
    memcpy(req.frame, frame, len);
    memset(ack, 0, sizeof(*ack));
    kr = IOConnectCallStructMethod(c->connect, kMtkAwdlRawTx, &req, in, ack, &out);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "Tx: 0x%x err=%u\n", kr, ack->err);
        return -1;
    }
    return ack->ok ? 0 : -1;
}

/* Drain RX into caller buffer; returns bytes of the batch struct. */
static int do_rx(struct conn *c, void *buf, size_t cap, size_t *outsz) {
    *outsz = cap;
    kern_return_t kr = IOConnectCallStructMethod(c->connect, kMtkAwdlRawRxRead,
        NULL, 0, buf, outsz);
    return kr == KERN_SUCCESS ? 0 : -1;
}

static int do_txs(struct conn *c, void *buf, size_t cap, size_t *outsz) {
    *outsz = cap;
    kern_return_t kr = IOConnectCallStructMethod(c->connect, kMtkAwdlRawTxsRead,
        NULL, 0, buf, outsz);
    return kr == KERN_SUCCESS ? 0 : -1;
}

/* ---------- pcap (radiotap linktype 127) ---------- */

struct pcap_hdr {
    uint32_t magic;
    uint16_t vmaj, vmin;
    int32_t  tz, sigfigs;
    uint32_t snaplen, network;
};
struct pcap_pkthdr {
    uint32_t ts_sec, ts_usec, incl_len, orig_len;
};
/* Minimal radiotap: present flags + channel + dbm_antsignal. */
#pragma pack(push, 1)
struct rtap {
    uint8_t  it_version, it_pad;
    uint16_t it_len;
    uint32_t it_present;
    uint16_t channel_freq, channel_flags;
    int8_t   dbm_antsignal;
    uint8_t  pad;
};
#pragma pack(pop)

static FILE *pcap_open(const char *path) {
    FILE *f = fopen(path, "wb");
    struct pcap_hdr h;
    if (!f) { perror(path); return NULL; }
    memset(&h, 0, sizeof(h));
    h.magic = 0xa1b2c3d4; h.vmaj = 2; h.vmin = 4;
    h.snaplen = 2048; h.network = 127; /* LINKTYPE_IEEE802_11_RADIO */
    fwrite(&h, sizeof(h), 1, f);
    return f;
}

static void pcap_write(FILE *f, uint64_t ts_us, int rssi, uint16_t chan,
    const uint8_t *frame, uint16_t len)
{
    struct pcap_pkthdr ph;
    struct rtap rt;
    uint16_t freq;
    if (!f) return;
    memset(&rt, 0, sizeof(rt));
    rt.it_len = sizeof(rt);
    rt.it_present = (1u << 3) | (1u << 5); /* channel, dbm_antsignal */
    if (chan >= 1 && chan <= 14) freq = (uint16_t)(2407 + 5 * chan);
    else if (chan == 14) freq = 2484;
    else freq = (uint16_t)(5000 + 5 * chan);
    rt.channel_freq = freq;
    rt.channel_flags = (chan <= 14) ? 0x0080 : 0x0100; /* 2GHz/5GHz */
    rt.dbm_antsignal = (int8_t)rssi;
    ph.ts_sec = (uint32_t)(ts_us / 1000000ull);
    ph.ts_usec = (uint32_t)(ts_us % 1000000ull);
    ph.incl_len = ph.orig_len = sizeof(rt) + len;
    fwrite(&ph, sizeof(ph), 1, f);
    fwrite(&rt, sizeof(rt), 1, f);
    fwrite(frame, 1, len, f);
}

/* ---------- commands ---------- */

static void print_info(const struct mtk_awdlraw_info *i) {
    printf("abi=%u present=%u state=%u channel=%u\n",
        i->abi, i->present, i->state, i->channel);
    printf("mac=%02x:%02x:%02x:%02x:%02x:%02x  awdl-bssid=%02x:%02x:%02x:%02x:%02x:%02x\n",
        i->mac[0],i->mac[1],i->mac[2],i->mac[3],i->mac[4],i->mac[5],
        i->bssid[0],i->bssid[1],i->bssid[2],i->bssid[3],i->bssid[4],i->bssid[5]);
    printf("rfcr_live=0x%08x  rx_captured=%llu dropped=%llu  tx_sent=%llu failed=%llu txs=%llu\n",
        i->rfcr_live,
        (unsigned long long)i->rx_captured, (unsigned long long)i->rx_dropped,
        (unsigned long long)i->tx_sent, (unsigned long long)i->tx_failed,
        (unsigned long long)i->txs_reports);
}

struct peer_ctx {
    uint8_t my[6];
    int saw_unicast_to_me;
    int saw_data_to_me;
    int mif_lists_me_as_sync;
    int mif_lists_me_as_master;
    uint8_t last_src[6];
    int have_src;
    uint64_t psf, mif, data, other, uni_to_me;
    uint64_t since_us;      /* uptime at start: older ring entries are stale */
    uint64_t stale, rx_errs;
    int last_rssi;
    uint64_t last_rx_us;    /* uptime of newest counted frame, 0 = none */
};

struct tlv_scan {
    uint8_t my[6];
    int as_sync, as_master;
};

static void tlv_cb(uint8_t kind, const uint8_t *v, size_t n, void *ctx) {
    struct tlv_scan *s = ctx;
    uint8_t m[6];
    if (kind == 4 && awdl_tlv4_master(v, n, m) == 0 && memcmp(m, s->my, 6) == 0)
        s->as_master = 1;
    if (kind == 24 && awdl_tlv24_sync(v, n, m) == 0 && memcmp(m, s->my, 6) == 0)
        s->as_sync = 1;
}

static void handle_rx(struct peer_ctx *pc, FILE *pcap,
    const struct mtk_awdlraw_rxrec *h, const uint8_t *frame)
{
    int to_me = (h->flags & MTK_AWDLRAW_RX_UNICAST_TO_ME) != 0;
    if (pcap) pcap_write(pcap, h->ts_us, h->rssi, h->chan, frame, h->len);
    memcpy(pc->last_src, h->src, 6);
    pc->have_src = 1;
    pc->last_rssi = h->rssi;
    pc->last_rx_us = h->ts_us;
    switch (h->kind) {
    case kMtkAwdlRawKindPSF: pc->psf++; break;
    case kMtkAwdlRawKindMIF: pc->mif++; break;
    case kMtkAwdlRawKindData: pc->data++; break;
    default: pc->other++; break;
    }
    if (to_me) {
        pc->uni_to_me++;
        pc->saw_unicast_to_me = 1;
        if (h->kind == kMtkAwdlRawKindData) pc->saw_data_to_me = 1;
    }
    if (h->kind == kMtkAwdlRawKindMIF && h->len > 40) {
        struct tlv_scan sc;
        memset(&sc, 0, sizeof(sc));
        memcpy(sc.my, pc->my, 6);
        awdl_walk_tlvs(frame + 40, h->len - 40, tlv_cb, &sc);
        if (sc.as_sync) pc->mif_lists_me_as_sync = 1;
        if (sc.as_master) pc->mif_lists_me_as_master = 1;
    }
}

static int drain_rx(struct conn *c, struct peer_ctx *pc, FILE *pcap) {
    uint8_t buf[AWDLTOOL_INBAND];
    int total = 0, iter;
    for (iter = 0; iter < 64; iter++) {
        size_t n = 0;
        struct mtk_awdlraw_rxbatch *b;
        uint32_t off, i;
        if (do_rx(c, buf, sizeof(buf), &n) != 0) {
            pc->rx_errs++;
            return total ? total : -1;
        }
        if (n < sizeof(*b)) break;
        b = (struct mtk_awdlraw_rxbatch *)buf;
        off = (uint32_t)sizeof(*b);
        for (i = 0; i < b->count; i++) {
            struct mtk_awdlraw_rxrec *h = (struct mtk_awdlraw_rxrec *)(buf + off);
            const uint8_t *frame;
            uint32_t stride;
            if (off + sizeof(*h) > n) break;
            frame = buf + off + sizeof(*h);
            stride = (uint32_t)sizeof(*h) + ((h->len + 3u) & ~3u);
            if (off + stride > n) break;
            if (h->ts_us < pc->since_us)
                pc->stale++;
            else
                handle_rx(pc, pcap, h, frame);
            off += stride;
        }
        total += (int)b->count;
        if (b->count == 0 || b->remaining == 0) break;
    }
    return total;
}

/* Drain TXS reports; count them and those with an error in w0[18:16]. */
static void drain_txs(struct conn *c, uint64_t *nrep, uint64_t *nerr) {
    uint8_t buf[AWDLTOOL_INBAND];
    int iter;
    for (iter = 0; iter < 8; iter++) {
        size_t n = 0;
        struct mtk_awdlraw_txsbatch *tb;
        uint32_t t;
        if (do_txs(c, buf, sizeof(buf), &n) != 0 || n < sizeof(*tb)) return;
        tb = (struct mtk_awdlraw_txsbatch *)buf;
        for (t = 0; t < tb->count; t++) {
            if (sizeof(*tb) + (t + 1) * sizeof(tb->recs[0]) > n) break;
            (*nrep)++;
            if ((tb->recs[t].w[0] >> 16) & 7) (*nerr)++;
        }
        if (tb->count == 0 || tb->remaining == 0) break;
    }
}

struct txstat { uint64_t psf, mif, fail, txs, txs_err; };

static void print_stat(uint64_t t0_us, const struct peer_ctx *pc,
    const struct txstat *tx)
{
    long long age = -1;
    char src[18] = "-";
    if (pc->last_rx_us) {
        uint64_t u = uptime_us();
        age = u > pc->last_rx_us ? (long long)((u - pc->last_rx_us) / 1000ull) : 0;
    }
    if (pc->have_src)
        snprintf(src, sizeof(src), "%02x:%02x:%02x:%02x:%02x:%02x",
            pc->last_src[0], pc->last_src[1], pc->last_src[2],
            pc->last_src[3], pc->last_src[4], pc->last_src[5]);
    printf("STAT t=%llu rx_psf=%llu rx_mif=%llu rx_data=%llu rx_other=%llu "
        "uni_to_me=%llu as_sync=%d as_master=%d last_rssi=%d last_age_ms=%lld "
        "src=%s tx_psf=%llu tx_mif=%llu tx_fail=%llu txs=%llu txs_err=%llu "
        "stale=%llu rx_err=%llu\n",
        (unsigned long long)((now_us() - t0_us) / 1000000ull),
        (unsigned long long)pc->psf, (unsigned long long)pc->mif,
        (unsigned long long)pc->data, (unsigned long long)pc->other,
        (unsigned long long)pc->uni_to_me,
        pc->mif_lists_me_as_sync, pc->mif_lists_me_as_master,
        pc->last_rx_us ? pc->last_rssi : 0, age, src,
        (unsigned long long)(tx ? tx->psf : 0), (unsigned long long)(tx ? tx->mif : 0),
        (unsigned long long)(tx ? tx->fail : 0), (unsigned long long)(tx ? tx->txs : 0),
        (unsigned long long)(tx ? tx->txs_err : 0),
        (unsigned long long)pc->stale, (unsigned long long)pc->rx_errs);
    fflush(stdout);
}

static void fill_own(struct awdl_own *o, const struct mtk_awdlraw_info *inf,
    const char *name)
{
    memset(o, 0, sizeof(*o));
    memcpy(o->addr, inf->mac, 6);
    o->channel = (uint8_t)(inf->channel ? inf->channel : 149);
    o->master_channel = o->channel;
    o->next_tu = 32;                 /* mid-slot-ish */
    o->aw_counter = 0;
    o->psf_interval_tu = AWDL_PSF_TU;
    memcpy(o->master, o->addr, 6);
    memcpy(o->sync, o->addr, 6);
    o->height = 0;
    o->master_metric = o->self_metric = 60;
    o->master_counter = o->self_counter = 0;
    o->name = name;
    awdl_seq_fixed(o, o->master_channel);
}

static int cmd_info(struct conn *c) {
    struct mtk_awdlraw_info i;
    if (do_info(c, &i) != 0) return 1;
    print_info(&i);
    if (!i.present) {
        fprintf(stderr, "raw channel not present (boot with -mtkawdltx)\n");
        return 2;
    }
    if (i.state != 4)
        fprintf(stderr, "warning: not in RUN (state=%u); TX will fail\n", i.state);
    return 0;
}

static int cmd_dump(struct conn *c, int seconds, const char *out, int stats_ms) {
    struct mtk_awdlraw_info i;
    struct peer_ctx pc;
    FILE *pcap = NULL;
    uint64_t end, t0, next_stat;
    if (do_info(c, &i) != 0) return 1;
    print_info(&i);
    memset(&pc, 0, sizeof(pc));
    memcpy(pc.my, i.mac, 6);
    pc.since_us = uptime_us();
    drain_rx(c, &pc, NULL);  /* discard frames left in the ring from before */
    printf("discarded %llu stale ring entries\n", (unsigned long long)pc.stale);
    if (out) {
        pcap = pcap_open(out);
        if (!pcap) return 1;
        printf("writing %s\n", out);
    }
    t0 = now_us();
    end = t0 + (uint64_t)seconds * 1000000ull;
    next_stat = t0 + (uint64_t)stats_ms * 1000ull;
    while (!g_stop && now_us() < end) {
        drain_rx(c, &pc, pcap);
        if (stats_ms > 0 && now_us() >= next_stat) {
            print_stat(t0, &pc, NULL);
            next_stat += (uint64_t)stats_ms * 1000ull;
        }
        usleep(50 * 1000);
    }
    drain_rx(c, &pc, pcap);
    if (pcap) fclose(pcap);
    if (stats_ms > 0) print_stat(t0, &pc, NULL);
    printf("captured: psf=%llu mif=%llu data=%llu other=%llu uni_to_me=%llu stale=%llu rx_err=%llu\n",
        (unsigned long long)pc.psf, (unsigned long long)pc.mif,
        (unsigned long long)pc.data, (unsigned long long)pc.other,
        (unsigned long long)pc.uni_to_me, (unsigned long long)pc.stale,
        (unsigned long long)pc.rx_errs);
    return pc.rx_errs && (pc.psf + pc.mif + pc.data + pc.other) == 0 ? 6 : 0;
}

static int cmd_sync(struct conn *c, int seconds, const char *name, const char *out,
    int stats_ms) {
    struct mtk_awdlraw_info i;
    struct awdl_own own;
    struct peer_ctx pc;
    FILE *pcap = NULL;
    uint8_t frame[800];
    uint16_t seq = 1;
    uint64_t end, next_psf = 0, next_mif = 0, t0, next_stat;
    struct txstat tx;
    memset(&tx, 0, sizeof(tx));
#define tx_psf  tx.psf
#define tx_mif  tx.mif
#define tx_fail tx.fail

    if (do_info(c, &i) != 0) return 1;
    print_info(&i);
    if (!i.present || i.state != 4) {
        fprintf(stderr, "need present=1 and state=4 (associated)\n");
        return 2;
    }
    fill_own(&own, &i, name);
    memset(&pc, 0, sizeof(pc));
    memcpy(pc.my, i.mac, 6);
    pc.since_us = uptime_us();
    drain_rx(c, &pc, NULL);  /* discard frames left in the ring from before */
    drain_txs(c, &tx.txs, &tx.txs_err);
    tx.txs = tx.txs_err = 0;
    printf("discarded %llu stale ring entries\n", (unsigned long long)pc.stale);
    if (out) {
        pcap = pcap_open(out);
        if (!pcap) return 1;
    }
    printf("sending PSF every ~110 TU and MIF mid-slot on ch%u as %s\n",
        own.channel, own.name);
    printf("success = iPhone sends unicast to our MAC, OR a MIF lists us as sync/master\n");

    t0 = now_us();
    end = t0 + (uint64_t)seconds * 1000000ull;
    next_stat = t0 + (uint64_t)stats_ms * 1000ull;
    next_psf = now_us();
    next_mif = now_us() + 32 * 1024; /* ~32 TU */
    while (!g_stop && now_us() < end) {
        uint64_t t = now_us();
        drain_rx(c, &pc, pcap);
        if (t >= next_psf) {
            struct mtk_awdlraw_txack ack;
            size_t n = awdl_build_action(frame, sizeof(frame), &own, AWDL_PSF,
                seq++, (uint32_t)t);
            own.aw_counter++;
            /* Every 16th PSF asks firmware for a TX status report so we can
             * tell "queued" from "actually left the radio". */
            if (do_tx(c, frame, (uint32_t)n, 6000,
                    MTK_AWDLRAW_TX_NO_ACK | MTK_AWDLRAW_TX_SET_SN |
                    ((tx_psf % 16) == 0 ? MTK_AWDLRAW_TX_WANT_TXS : 0), &ack) == 0)
                tx_psf++;
            else
                tx_fail++;
            next_psf = t + (uint64_t)AWDL_PSF_TU * 1024ull; /* TU ≈ 1024 µs */
        }
        if (t >= next_mif) {
            struct mtk_awdlraw_txack ack;
            size_t n = awdl_build_action(frame, sizeof(frame), &own, AWDL_MIF,
                seq++, (uint32_t)t);
            if (do_tx(c, frame, (uint32_t)n, 6000,
                    MTK_AWDLRAW_TX_NO_ACK | MTK_AWDLRAW_TX_SET_SN, &ack) == 0)
                tx_mif++;
            else
                tx_fail++;
            next_mif = t + 64 * 1024ull; /* one slot */
        }
        /* Keep next_tu roughly coherent. */
        own.next_tu = (uint16_t)(((next_mif - t) / 1024ull) % 64);
        if (stats_ms > 0 && t >= next_stat) {
            drain_txs(c, &tx.txs, &tx.txs_err);
            print_stat(t0, &pc, &tx);
            next_stat += (uint64_t)stats_ms * 1000ull;
        }
        usleep(5 * 1000);
    }
    drain_rx(c, &pc, pcap);
    usleep(100 * 1000);
    drain_txs(c, &tx.txs, &tx.txs_err);
    if (pcap) fclose(pcap);
    if (stats_ms > 0) print_stat(t0, &pc, &tx);

    printf("\n=== sync result ===\n");
    printf("tx: psf=%llu mif=%llu fail=%llu\n",
        (unsigned long long)tx_psf, (unsigned long long)tx_mif,
        (unsigned long long)tx_fail);
    printf("txs: reports=%llu with_error=%llu (1 of every 16 PSF asks for a report)\n",
        (unsigned long long)tx.txs, (unsigned long long)tx.txs_err);
    printf("rx: psf=%llu mif=%llu data=%llu uni_to_me=%llu stale_discarded=%llu rx_err=%llu\n",
        (unsigned long long)pc.psf, (unsigned long long)pc.mif,
        (unsigned long long)pc.data, (unsigned long long)pc.uni_to_me,
        (unsigned long long)pc.stale, (unsigned long long)pc.rx_errs);
    if (pc.have_src)
        printf("last rx: src=%02x:%02x:%02x:%02x:%02x:%02x rssi=%d\n",
            pc.last_src[0], pc.last_src[1], pc.last_src[2],
            pc.last_src[3], pc.last_src[4], pc.last_src[5], pc.last_rssi);
    printf("flags: unicast_to_me=%d data_to_me=%d mif_lists_me_as_sync=%d mif_lists_me_as_master=%d\n",
        pc.saw_unicast_to_me, pc.saw_data_to_me,
        pc.mif_lists_me_as_sync, pc.mif_lists_me_as_master);
    if (pc.saw_unicast_to_me || pc.mif_lists_me_as_sync || pc.mif_lists_me_as_master) {
        printf("VERDICT: SUCCESS — iPhone appears to see us on AWDL\n");
        return 0;
    }
    if (tx_psf + tx_mif == 0) {
        printf("VERDICT: FAIL — no frames left the host (TX path broken)\n");
        return 3;
    }
    if (pc.psf + pc.mif + pc.data == 0 && pc.rx_errs > 0) {
        printf("VERDICT: RX-READ-ERROR — %llu ring reads failed\n",
            (unsigned long long)pc.rx_errs);
        return 6;
    }
    if (pc.psf + pc.mif == 0) {
        printf("VERDICT: INCONCLUSIVE — we sent, but heard no iPhone AWDL "
               "(is the share sheet open? try docs/test-protocol)\n");
        return 4;
    }
    printf("VERDICT: NO-PEER-YET — heard iPhone AWDL but it did not address us. "
           "Common: phone has not opened an AW to our MAC (BLE awaken missing), "
           "or our TLVs/timing look wrong. pcap: %s\n", out ? out : "(none)");
    return 5;
#undef tx_psf
#undef tx_mif
#undef tx_fail
}

static int parse_mac(const char *s, uint8_t m[6]) {
    unsigned a[6];
    if (sscanf(s, "%02x:%02x:%02x:%02x:%02x:%02x",
            &a[0],&a[1],&a[2],&a[3],&a[4],&a[5]) != 6)
        return -1;
    for (int i = 0; i < 6; i++) m[i] = (uint8_t)a[i];
    return 0;
}

static int cmd_probe(struct conn *c, const uint8_t dst[6], int n, uint32_t rate) {
    struct mtk_awdlraw_info i;
    uint8_t frame[64];
    uint16_t seq = 1;
    int ok = 0, fail = 0, acked = 0, ackerr = 0;
    uint8_t txsbuf[AWDLTOOL_INBAND];

    if (do_info(c, &i) != 0) return 1;
    print_info(&i);
    if (!i.present || i.state != 4) {
        fprintf(stderr, "need present=1 and state=4\n");
        return 2;
    }
    printf("sending %d unicast data frames to %02x:%02x:%02x:%02x:%02x:%02x at %u kbps on ch%u\n",
        n, dst[0],dst[1],dst[2],dst[3],dst[4],dst[5], rate, i.channel);
    printf("(M3b risk probe: on Linux this card sent 0/132 on 5 GHz)\n");

    for (int k = 0; k < n && !g_stop; k++) {
        struct awdl_buf b;
        struct mtk_awdlraw_txack ack;
        /* Minimal AWDL data header + 8 zero bytes of fake IPv6. */
        ab_init(&b, frame, sizeof(frame));
        ab_le16(&b, 0x0008);              /* data */
        ab_le16(&b, 0);
        ab_mac(&b, dst);
        ab_mac(&b, i.mac);
        ab_mac(&b, AWDL_BSSID);
        ab_le16(&b, (uint16_t)((seq++ & 0x0fff) << 4));
        {
            uint8_t llc[8] = {0xaa,0xaa,0x03,0x00,0x17,0xf2,0x08,0x00};
            ab_bytes(&b, llc, 8);
            ab_le16(&b, 0x0403);
            ab_le16(&b, (uint16_t)k);
            ab_le16(&b, 0);
            ab_be16(&b, 0x86dd);
            uint8_t pad[8] = {0};
            ab_bytes(&b, pad, 8);
        }
        if (do_tx(c, frame, (uint32_t)b.n, rate,
                MTK_AWDLRAW_TX_WANT_TXS | MTK_AWDLRAW_TX_SET_SN, &ack) == 0)
            ok++;
        else
            fail++;
        usleep(2 * 1000);
    }
    usleep(200 * 1000);
    {
        size_t nout = 0;
        if (do_txs(c, txsbuf, sizeof(txsbuf), &nout) == 0 &&
            nout >= sizeof(struct mtk_awdlraw_txsbatch)) {
            struct mtk_awdlraw_txsbatch *tb = (void *)txsbuf;
            printf("TXS reports: %u\n", tb->count);
            for (uint32_t t = 0; t < tb->count; t++) {
                uint32_t w0 = tb->recs[t].w[0];
                int err = (w0 >> 16) & 7;
                int fixed = (w0 >> 31) & 1;
                uint32_t ratebits = w0 & 0x3fff;
                printf("  txs[%u] w0=0x%08x ackerr=%d fixed=%d rate=0x%x\n",
                    t, w0, err, fixed, ratebits);
                if (err) ackerr++; else acked++;
            }
        } else {
            printf("TXS reports: none (firmware may not have returned any)\n");
        }
    }
    printf("queued ok=%d fail=%d ; txs acked=%d ackerr=%d\n", ok, fail, acked, ackerr);
    if (ok == 0) {
        printf("VERDICT: TX-FAIL — frames never queued\n");
        return 3;
    }
    if (acked > 0 && ackerr == 0) {
        printf("VERDICT: TX-OK — firmware reported ACK (peer must exist)\n");
        return 0;
    }
    if (ackerr > 0) {
        printf("VERDICT: ON-AIR-NO-ACK — frames likely left the radio, peer did not ACK "
               "(expected if dst is not in an AW with us)\n");
        return 4;
    }
    printf("VERDICT: QUEUED-NO-TXS — queued but no status; need a sniffer or "
           "another device to confirm airtime\n");
    return 5;
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage:\n"
        "  sudo %s info\n"
        "  sudo %s dump [-t SEC] [-o FILE.pcap] [-s STAT_MS]\n"
        "  sudo %s sync [-t SEC] [-n NAME] [-o FILE.pcap] [-s STAT_MS]\n"
        "  sudo %s probe -d AA:BB:CC:DD:EE:FF [-n N] [-r KBPS]\n",
        argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv) {
    struct conn c;
    const char *cmd;
    int rc;

    if (argc < 2) { usage(argv[0]); return 2; }
    cmd = argv[1];
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    if (geteuid() != 0)
        fprintf(stderr, "warning: not root; IOServiceOpen will likely fail\n");

    if (conn_open(&c) != 0) return 1;

    if (strcmp(cmd, "info") == 0) {
        rc = cmd_info(&c);
    } else if (strcmp(cmd, "dump") == 0) {
        int t = 60, st = 0; const char *o = NULL;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "-t") && i+1 < argc) t = atoi(argv[++i]);
            else if (!strcmp(argv[i], "-o") && i+1 < argc) o = argv[++i];
            else if (!strcmp(argv[i], "-s") && i+1 < argc) st = atoi(argv[++i]);
            else { usage(argv[0]); rc = 2; goto out; }
        }
        rc = cmd_dump(&c, t, o, st);
    } else if (strcmp(cmd, "sync") == 0) {
        int t = 60, st = 0; const char *n = "rz616-m3a", *o = NULL;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "-t") && i+1 < argc) t = atoi(argv[++i]);
            else if (!strcmp(argv[i], "-n") && i+1 < argc) n = argv[++i];
            else if (!strcmp(argv[i], "-o") && i+1 < argc) o = argv[++i];
            else if (!strcmp(argv[i], "-s") && i+1 < argc) st = atoi(argv[++i]);
            else { usage(argv[0]); rc = 2; goto out; }
        }
        rc = cmd_sync(&c, t, n, o, st);
    } else if (strcmp(cmd, "probe") == 0) {
        uint8_t dst[6]; int have = 0, n = 32; uint32_t r = 6000;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "-d") && i+1 < argc) {
                if (parse_mac(argv[++i], dst) != 0) {
                    fprintf(stderr, "bad MAC\n"); rc = 2; goto out;
                }
                have = 1;
            } else if (!strcmp(argv[i], "-n") && i+1 < argc) n = atoi(argv[++i]);
            else if (!strcmp(argv[i], "-r") && i+1 < argc) r = (uint32_t)atoi(argv[++i]);
            else { usage(argv[0]); rc = 2; goto out; }
        }
        if (!have) { fprintf(stderr, "probe needs -d MAC\n"); rc = 2; goto out; }
        rc = cmd_probe(&c, dst, n, r);
    } else {
        usage(argv[0]); rc = 2;
    }
out:
    conn_close(&c);
    return rc;
}
