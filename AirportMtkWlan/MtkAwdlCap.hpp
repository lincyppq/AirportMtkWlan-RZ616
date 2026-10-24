/*
 * MtkAwdlCap.hpp — rz616-m3i-1 AWDL capability probe.
 *
 * A runtime switch (default off, not persisted) that sets
 * apple80211_capability_data.capabilities[4] bit0, the byte wifip2pd tests
 * before it asks for an AWDL virtual interface. It also records every
 * CARD_CAPABILITIES and VIRTUAL_IF_CREATE request. VIRTUAL_IF_CREATE is still
 * answered with kIOReturnUnsupported; no interface is created. Only built
 * with MTK_AWDL_CAP_PROBE. GPLv2.
 */
#ifndef MtkAwdlCap_hpp
#define MtkAwdlCap_hpp

#include <IOKit/IOTypes.h>
#include <ClientKit/AwdlRaw.h>

struct apple80211_virt_if_create_data;

#ifdef __cplusplus
extern "C" {
#endif

/* AirportItlwm::start. Until it succeeds every call below is a no-op (the
 * capability bit is never set). The lock it installs lives as long as the
 * kext; driver instances never free it. */
void     mtk_awdlcap_init(void);
/* Applies the switch to caps[] (len bytes) and records the call. */
void     mtk_awdlcap_on_card_caps(uint8_t *caps, uint32_t len);
/* Records one VIRTUAL_IF_CREATE request and the value returned for it. */
void     mtk_awdlcap_on_vif_create(const struct apple80211_virt_if_create_data *d,
             IOReturn ret);
/* Returns false (state unchanged) when the probe is not initialised. */
bool     mtk_awdlcap_set(uint32_t enabled);
/* Returns false when the probe is not initialised. */
bool     mtk_awdlcap_read(struct mtk_awdlcap_state *out);

#ifdef __cplusplus
}
#endif

#endif /* MtkAwdlCap_hpp */
