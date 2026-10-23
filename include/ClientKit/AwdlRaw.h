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
    kMtkAwdlRawMethodCount
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

#endif /* MtkAwdlRaw_h */
