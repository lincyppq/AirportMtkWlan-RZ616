/*
 * AwdlRaw.h — ABI between AirportMtkWlan (kernel) and the awdltool userspace
 * helper for the M3a observe + inject raw AWDL channel.
 *
 * Everything here is active only when the kext was built with MTK_AWDL_TX and
 * booted with -mtkawdltx. Fixed-width types only, so the same layout is used
 * on both sides. Observe + inject; no routing, no system AirDrop.
 *
 * GPLv2, matching the rest of the driver.
 */
#ifndef MtkAwdlRaw_h
#define MtkAwdlRaw_h

#include <stdint.h>

/* IOServiceOpen type for MtkAwdlRawUserClient. "AWDT". */
#define MTK_AWDLRAW_USERCLIENT_TYPE 0x41574454u
#define MTK_AWDLRAW_ABI             1u

/* AWDL BSSID (AWDL-1). */
#define MTK_AWDLRAW_BSSID0 0x00
#define MTK_AWDLRAW_BSSID1 0x25
#define MTK_AWDLRAW_BSSID2 0x00
#define MTK_AWDLRAW_BSSID3 0xff
#define MTK_AWDLRAW_BSSID4 0x94
#define MTK_AWDLRAW_BSSID5 0x73

/* Largest 802.11 frame captured or injected (no FCS). */
#define MTK_AWDLRAW_MAX_FRAME 800u

/* externalMethod selectors. */
enum {
    kMtkAwdlRawGetInfo = 0, /* -> mtk_awdlraw_info                        */
    kMtkAwdlRawRxRead  = 1, /* -> mtk_awdlraw_rxbatch (drains the ring)   */
    kMtkAwdlRawTx      = 2, /* mtk_awdlraw_tx -> mtk_awdlraw_txack        */
    kMtkAwdlRawTxsRead = 3, /* -> mtk_awdlraw_txsbatch (drains the ring)  */
    kMtkAwdlCapGet     = 4, /* m3i-1: -> mtk_awdlcap_state                 */
    kMtkAwdlCapSet     = 5, /* m3i-1: mtk_awdlcap_set -> mtk_awdlcap_state */
    kMtkAwdlCapVifMake = 6, /* m3i-2: 手动建 awdl0（测试用）-> mtk_awdlcap_state */
    kMtkAwdlCapVifDrop = 7, /* m3i-2: 释放 awdl0（测试用）  -> mtk_awdlcap_state */
    kMtkAwdlRawMethodCount
};

/* Selector 8 is intentionally unused so the unapplied m3a-6 patch can still
 * add GetInfo2 there. This tree does not implement that selector and does
 * not raise MTK_AWDLRAW_MAX_FRAME above 800. */
enum {
    kMtkAwdlNativeGet = 9, /* -> mtk_awdl_native_state */
    kMtkAwdlNativeSet = 10 /* mtk_awdl_native_set -> mtk_awdl_native_state */
};

/* rx record kinds. */
enum {
    kMtkAwdlRawKindOther  = 0,
    kMtkAwdlRawKindPSF    = 1,
    kMtkAwdlRawKindMIF    = 2,
    kMtkAwdlRawKindAction = 3, /* AWDL action, other subtype */
    kMtkAwdlRawKindData   = 4  /* data frame on the AWDL BSSID */
};

/* rx record flags. */
#define MTK_AWDLRAW_RX_UNICAST_TO_ME 0x01u
#define MTK_AWDLRAW_RX_HWDEC         0x02u

/* tx flags. */
#define MTK_AWDLRAW_TX_NO_ACK  0x01u /* set TXD3 NO_ACK (fire and forget)   */
#define MTK_AWDLRAW_TX_WANT_TXS 0x02u /* ask firmware for a TX status report */
#define MTK_AWDLRAW_TX_SET_SN  0x04u /* stamp a hardware sequence number     */
#define MTK_AWDLRAW_TX_RECOVER 0x100u /* m3a-5: no frame; request a full chip
                                      reset + firmware reload (EBUSY if one
                                      is running, ENOTSUP with -mtknorecover) */

struct mtk_awdlraw_info {
    uint32_t abi;
    uint32_t present;       /* 1 when built with MTK_AWDL_TX and -mtkawdltx */
    uint32_t state;         /* ic_state (4 = RUN)                           */
    uint32_t channel;       /* current associated channel                   */
    uint8_t  mac[6];        /* ic_myaddr: the Wi-Fi interface MAC           */
    uint8_t  bssid[6];      /* AWDL BSSID                                   */
    uint32_t rfcr_live;     /* MT_WF_RFCR(0), only valid in RUN             */
    uint64_t rx_captured;
    uint64_t rx_dropped;    /* ring overruns                               */
    uint64_t tx_sent;       /* frames handed to the ring                   */
    uint64_t tx_failed;
    uint64_t txs_reports;
    uint32_t reserved[8];
};

/* One captured frame header; MTK_AWDLRAW align 4, then `len` frame bytes. */
struct mtk_awdlraw_rxrec {
    uint64_t ts_us;         /* host uptime in microseconds                 */
    int32_t  rssi;          /* dBm                                         */
    uint16_t chan;
    uint16_t len;           /* 802.11 bytes that follow this header        */
    uint8_t  kind;
    uint8_t  flags;
    uint8_t  af_subtype;    /* AWDL action subtype, 0xff if not an action  */
    uint8_t  af_version;
    uint8_t  src[6];        /* A2                                          */
    uint16_t pad;
};

struct mtk_awdlraw_rxbatch {
    uint32_t count;         /* records packed in data[]                    */
    uint32_t bytes;         /* used bytes in data[]                        */
    uint32_t remaining;     /* records still queued after this read        */
    uint32_t _r;
    uint8_t  data[];        /* count * (rxrec + padded frame)              */
};

struct mtk_awdlraw_tx {
    uint32_t rate_kbps;     /* 6000/9000/12000/18000/24000/36000/54000; 0 => 6000 */
    uint32_t flags;         /* MTK_AWDLRAW_TX_*                            */
    uint32_t wcid;          /* 0 => the vif's reserved station entry       */
    uint32_t len;           /* bytes in frame[]                           */
    uint8_t  frame[MTK_AWDLRAW_MAX_FRAME];
};

struct mtk_awdlraw_txack {
    uint32_t ok;            /* 1 if queued to the TX ring                  */
    uint32_t token;         /* packet id used (for TXS correlation)        */
    uint32_t queued;
    uint32_t err;           /* errno-like on failure                      */
};

/* One raw TX-status report: the 8 firmware words plus when we saw it. */
struct mtk_awdlraw_txsrec {
    uint64_t ts_us;
    uint32_t w[8];
};

struct mtk_awdlraw_txsbatch {
    uint32_t count;
    uint32_t remaining;
    uint32_t _r0;
    uint32_t _r1;
    struct mtk_awdlraw_txsrec recs[];
};

/*
 * m3i-1 AWDL capability probe (kext built with MTK_AWDL_CAP_PROBE).
 * The switch sets CARD_CAPABILITIES capabilities[4] bit0 (the byte IO80211Family
 * copies to user space starts at capabilities[0], after the 4-byte version).
 * Default off, not persisted. VIRTUAL_IF_CREATE is only recorded.
 */
#define MTK_AWDLCAP_ABI  2u
#define MTK_AWDLCAP_RECS 32u

enum {
    MTK_AWDLCAP_KIND_CAPS       = 1, /* data = capabilities[] as returned, ret = bytes */
    MTK_AWDLCAP_KIND_VIF_CREATE = 2, /* data = 32-byte apple80211_virt_if_create_data,
                                        ret = IOReturn given back */
    MTK_AWDLCAP_KIND_SET        = 3, /* enabled = new value, ret = old value */
    MTK_AWDLCAP_KIND_VIF_RESULT = 4  /* data = struct mtk_awdlcap_vifres     */
};

/* m3i-2: 建 awdl0 的每一步。ret 是该步的返回值（IOReturn 或 bool）。 */
enum {
    MTK_AWDLCAP_STEP_ALLOC    = 1,
    MTK_AWDLCAP_STEP_INIT     = 2,
    MTK_AWDLCAP_STEP_ATTACH   = 3,
    MTK_AWDLCAP_STEP_REGISTER = 4,
    MTK_AWDLCAP_STEP_DONE     = 5,
    MTK_AWDLCAP_STEP_DROP     = 6
};

struct mtk_awdlcap_vifres {
    uint32_t step;      /* MTK_AWDLCAP_STEP_*  */
    uint32_t ret;       /* IOReturn 或 bool     */
    uint32_t role;
    uint8_t  mac[6];
    uint16_t mtu;       /* 0 = 没有读回          */
    uint32_t pad;
};

struct mtk_awdlcap_rec {
    uint64_t ts_us;         /* mach uptime, microseconds                    */
    uint32_t kind;          /* MTK_AWDLCAP_KIND_*                           */
    int32_t  pid;           /* calling process                              */
    char     comm[16];
    uint32_t enabled;       /* switch value when the call was handled       */
    uint32_t ret;
    uint8_t  data[32];
};

struct mtk_awdlcap_state {
    uint32_t abi;
    uint32_t enabled;
    uint64_t set_calls;
    uint64_t caps_calls;
    uint64_t caps_calls_bit_set;    /* calls that returned capabilities[4] bit0 */
    uint64_t vif_create_calls;
    uint64_t log_suppressed;        /* IOLog lines dropped by the rate limit    */
    uint64_t rec_total;             /* records ever written; ring keeps the last */
    uint64_t now_us;                /* uptime when this snapshot was taken      */
    struct mtk_awdlcap_rec recs[MTK_AWDLCAP_RECS]; /* index = n % MTK_AWDLCAP_RECS */
    /* --- m3i-2（只在尾部追加，前面偏移不变）--- */
    uint64_t vif_ok;        /* 建成功（含幂等返回）的次数            */
    uint64_t vif_fail;      /* 失败次数                              */
    uint32_t vif_live;      /* 1 = 现在有一个由我们建的接口          */
    uint32_t vif_role;
    uint16_t vif_mtu;       /* 0 = 没有读回                          */
    uint8_t  vif_mac[6];
    char     vif_name[16];
};

struct mtk_awdlcap_set {
    uint32_t enabled;       /* 0 or 1 */
    uint32_t _r;
};

/*
 * On-channel native interface experiment (selectors 9 and 10).
 * Default off. enabled is the gate only. The other fields are facts about
 * this build: registration, RX submit, IPv6 handoff and transport stay 0
 * until layout evidence exists. No MAC address is stored here.
 */
#define MTK_AWDL_NATIVE_ABI 1u

struct mtk_awdl_native_state {
    uint32_t abi;
    uint32_t enabled;
    uint32_t build_allowed;
    uint32_t vtable_ok;
    uint32_t controller_slot_ok;
    uint32_t registration_permitted;
    uint32_t rx_submit_permitted;
    uint32_t ipv6_handoff_permitted;
    uint32_t transport_ready;
    uint32_t bsd_registered;
    uint32_t interface_live;
    uint32_t queues_up;
    uint32_t raw_tx_max_frame;
    uint32_t second_vif;
    char     blocker[64];
    char     readiness[80];
    char     reference[48];
};

struct mtk_awdl_native_set {
    uint32_t enabled;       /* 0 or 1 */
    uint32_t _r;
};

#endif /* MtkAwdlRaw_h */
