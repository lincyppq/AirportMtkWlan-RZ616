//
//  MtkMwx.hpp
//  SimpleMtkWlan
//
//  Created by laobamac (王孝慈) on 2026/5/17.
//  Copyright © 2026 laobamac (王孝慈). All rights reserved.
//

/*
 * MediaTek MT7922 HAL for SimpleMtkWlan.
 *
 * The driver body is ported from OpenBSD if_mwx.  This header only exposes
 * the SimpleMtkWlan HAL boundary; OpenBSD driver internals stay in MtkMwx.cpp.
 */

#ifndef MtkMwx_hpp
#define MtkMwx_hpp

#include <compat.h>
#include <HAL/MtkHalService.hpp>
#include <HAL/MtkDriverInfo.hpp>
#include <HAL/MtkDriverController.hpp>

struct mwx_softc;
struct pci_attach_args;

/*
 * Runtime tunables, set through the "SMTWTune" property of the
 * SimpleMtkWlan service (see SimpleMtkWlan::setProperties). They take
 * effect on the next association.
 */
struct smtw_tune_t {
	int ht;		/* advertise 802.11n (HT20) */
	int ba;		/* accept RX Block Ack agreements (A-MPDU RX) */
	int vht;	/* advertise 802.11ac (VHT), needs bw=80 */
	int bw;		/* max channel width to advertise: 20, 40 or 80 MHz */
	int txba;	/* request TX Block Ack (A-MPDU TX) */
	int sndq;	/* if_snd length in packets (applies immediately) */
	int txs;	/* TX status report every N frames, 0 = none (immediate) */
};
extern struct smtw_tune_t smtw_tune;

/* ni_rssi = dBm - MWX_MIN_DBM, see mwx_dbm_to_rssi() (IWM_MIN_DBM in itlwm). */
#define MWX_MIN_DBM	(-100)
struct ieee80211com;
void smtw_request_reassoc(struct ieee80211com *);

class MtkMwx : public MtkHalService, MtkDriverInfo, MtkDriverController {
    OSDeclareDefaultStructors(MtkMwx)

public:
    bool attach(IOPCIDevice *device) override;
    void detach(IOPCIDevice *device) override;
    IOReturn enable(IONetworkInterface *interface) override;
    IOReturn disable(IONetworkInterface *interface) override;
    struct ieee80211com *get80211Controller() override;
#ifdef AIRPORT_WCL
    /* As ItlIwm: remembered for scan timing requested by airportd. */
    IOReturn setScanHomeAwayTime(uint32_t milliseconds) override {
        get80211Controller()->ic_wcl_scan_away_time_ms = milliseconds;
        return kIOReturnSuccess;
    }
#endif
    MtkDriverInfo *getDriverInfo() override;
    MtkDriverController *getDriverController() override;
    void free() override;

    const char *getFirmwareVersion() override;
    int16_t getBSSNoise() override;
    bool is5GBandSupport() override;
    int getTxNSS() override;
    const char *getFirmwareName() override;
    UInt32 supportedFeatures() override;
    const char *getFirmwareCountryCode() override;
    uint32_t getTxQueueSize() override;

    void clearScanningFlags() override;
    IOReturn setMulticastList(IOEthernetAddress *addr, int count) override;

    static bool mwx_match(IOPCIDevice *device);

    IOEthernetController *mwxController();
    int mwxSleepNsec(void *ident, int priority, const char *wmesg, uint64_t timo);
    void mwxWakeup(void *ident);

private:
    void releaseAll();

    bool taskQueueOwned;
    struct mwx_softc *com;
    struct pci_attach_args *pci;
};

#endif /* MtkMwx_hpp */
