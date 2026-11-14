/*
 * MtkAwdlCap.hpp — rz616-m3i-2 AWDL capability probe + awdl0 creation.
 *
 * m3i-1: runtime switch (default off, not persisted) that sets
 * apple80211_capability_data.capabilities[4] bit0, the byte wifip2pd tests
 * before it asks for an AWDL virtual interface. Records CARD_CAPABILITIES and
 * VIRTUAL_IF_CREATE.
 *
 * m3i-2 (phase 2A): when the switch is on and wifip2pd asks for a role-6
 * (APPLE80211_VIF_AWDL) interface, build a real IO80211VirtualInterface and
 * return it. IO80211Family's configureIfnet() sets IFEF_AWDL/IFEF_DIRECTLINK
 * and the MTU for role 6 itself, so no ifnet work is done here. With the
 * switch off every call below behaves exactly like m3i-1.
 *
 * Only built with MTK_AWDL_CAP_PROBE. GPLv2.
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
/* AirportItlwm::start, right after mtk_awdlcap_init: remembers the controller
 * so the VIRTUAL_IF_CREATE path does not have to go through a virtual call. */
void     mtk_awdlcap_bind_controller(void *controller);
/* Returns the controller passed to mtk_awdlcap_bind_controller, or NULL. */
void    *mtk_awdlcap_controller(void);
/* Applies the switch to caps[] (len bytes) and records the call. */
void     mtk_awdlcap_on_card_caps(uint8_t *caps, uint32_t len);
/* Records one VIRTUAL_IF_CREATE request and the value returned for it. */
void     mtk_awdlcap_on_vif_create(const struct apple80211_virt_if_create_data *d,
             IOReturn ret);
/* m3i-2: builds (or returns the already built) awdl0 for this request.
 * kIOReturnUnsupported when the switch is off or role != 6 — no allocation.
 * d may be NULL, in which case the last recorded request is reused (manual
 * bring-up from the awdlcap tool). */
IOReturn mtk_awdlcap_vif_create(void *controller,
             const struct apple80211_virt_if_create_data *d);
/* m3i-2: detaches and releases the interface built by the call above. */
void     mtk_awdlcap_vif_drop(void);
/* Records one bring-up step. Safe to call from the native path. */
void     mtk_awdlcap_note(uint32_t step, IOReturn ret, const void *mac, const char *why);
/* Returns false (state unchanged) when the probe is not initialised. */
bool     mtk_awdlcap_set(uint32_t enabled);
/* Returns false when the probe is not initialised. */
bool     mtk_awdlcap_read(struct mtk_awdlcap_state *out);

#ifdef __cplusplus
}
#endif

#endif /* MtkAwdlCap_hpp */
