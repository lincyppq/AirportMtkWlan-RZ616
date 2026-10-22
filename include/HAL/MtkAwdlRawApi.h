/*
 * MtkAwdlRawApi.h — C boundary the MtkAwdlRawUserClient uses to reach the raw
 * AWDL subsystem living in MtkMwx.cpp. Only active with MTK_AWDL_TX and
 * -mtkawdltx. GPLv2.
 */
#ifndef MtkAwdlRawApi_h
#define MtkAwdlRawApi_h

#include <ClientKit/AwdlRaw.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when built with MTK_AWDL_TX, -mtkawdltx is set, and the HAL is attached. */
int      mwx_awdlraw_api_present(void);
void     mwx_awdlraw_api_info(struct mtk_awdlraw_info *out);
/* Drain the RX ring into out (a mtk_awdlraw_rxbatch); returns bytes written. */
uint32_t mwx_awdlraw_api_rx_read(void *out, uint32_t cap);
/* Inject one raw 802.11 frame. Returns 0 on success. */
int      mwx_awdlraw_api_tx(const struct mtk_awdlraw_tx *req,
             struct mtk_awdlraw_txack *ack);
/* Drain the TXS ring into out (a mtk_awdlraw_txsbatch); returns bytes written. */
uint32_t mwx_awdlraw_api_txs_read(void *out, uint32_t cap);

#ifdef __cplusplus
}
#endif

#endif /* MtkAwdlRawApi_h */
