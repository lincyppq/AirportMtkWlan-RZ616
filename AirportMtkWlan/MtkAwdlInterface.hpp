/*
 * MtkAwdlInterface.hpp — concrete IO80211AWDLProtocol for one on-channel
 * experiment. Default off. See docs/AWDL-NATIVE.md.
 *
 * The class layout of the Apple bases is the Tahoe 25G241 declaration from
 * ForbiddenItlwm 69e88fa0fdb0ef7122f2aee81111651c32260a62. Method bodies that
 * only store peer-manager requests are adapted from ItlAWDLInterface in that
 * tree. Intel listener, MCC, Block Ack and Bluetooth code are not included.
 *
 * GPLv2.
 */
#ifndef MtkAwdlInterface_hpp
#define MtkAwdlInterface_hpp

#include <Airport/macOS26/IO80211AWDLProtocol.h>
#include <Airport/macOS26/IOSkywalkQueueFactories.h>
#include <Airport/apple80211_ioctl.h>
#include <MtkAwdl/NativePolicy.hpp>
#include <MtkAwdl/RxLease.hpp>
#include <MtkAwdl/SoftwareRx.hpp>
#include <ClientKit/AwdlRaw.h>
#include <IOKit/IOLocks.h>

class AirportItlwm;
class IOWorkLoop;
class IOSkywalkPacketBufferPool;
class IOEventSource;

class MtkAwdlInterface : public IO80211AWDLProtocol {
    OSDeclareDefaultStructors(MtkAwdlInterface)
public:
    static MtkAwdlInterface *make(AirportItlwm *controller, const ether_addr *address, unsigned int role);
    IOReturn initializeNetwork(IO80211Controller *controller, IOWorkLoop *workLoop, const ether_addr &address);
    void offerOnGate(const uint8_t *frame, size_t length, int32_t rssiDbm, unsigned channel, bool data);
    void dispatchRx(const uint8_t *frame, size_t length, int32_t rssiDbm, unsigned channel, bool data);
    void stop(IOService *provider) override;
    MtkAwdl::LifeAction stopOwning(IOService *provider);
    void runDeferredCleanup();
    void rxLeaseFinished(MtkAwdl::LifeAction action);
    void free() override;

    IOReturn setMacAddress(ether_addr &) override;
    IOReturn getSSID(apple80211_ssid_data *) override;
    IOReturn getCHANNEL(apple80211_channel_data *) override;
    IOReturn getBSSID(apple80211_bssid_data *) override;
    IOReturn getSTATE(apple80211_state_data *) override;
    IOReturn getAWDL_SYNC_PARAMS(apple80211_awdl_sync_params *) override;
    IOReturn getAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled *) override;
    IOReturn getAWDL_EXTENSION_STATE_MACHINE_PARAMETERS(apple80211_awdl_extension_state_machine_parameter *) override;
    IOReturn getAWDL_ELECTION_ID(apple80211_awdl_election_id *) override;
    IOReturn getAWDL_ELECTION_METRIC(apple80211_awdl_election_metric *) override;
    IOReturn getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence *) override;
    IOReturn getPEER_CACHE_MAXIMUM_SIZE(apple80211_peer_cache_maximum_size *) override;
    IOReturn getAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel *) override;
    IOReturn getAWDL_SYNC_STATE(apple80211_awdl_sync_state *) override;
    IOReturn getAWDL_ELECTION_RSSI_THRESHOLDS(apple80211_awdl_election_rssi_thresholds *) override;
    IOReturn getAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode *) override;
    IOReturn getAWDL_ELECTION_PARAMS(apple80211_awdl_election_params *) override;
    IOReturn getAWDL_DEVICE_CAPABILITIES(apple80211_awdl_device_capabilities *) override;
    IOReturn getAWDL_SECONDARY_MASTER_CHANNEL(apple80211_awdl_secondary_master_channel *) override;
    IOReturn getAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode *) override;
    IOReturn getAWDL_MIN_RATE(apple80211_awdl_min_rate *) override;
    IOReturn getAWDL_ENCRYPTION_TYPE(apple80211_encrypt *) override;
    IOReturn getAWDL_TXCAL_PERIOD(apple80211_awdl_txcal_period *) override;
    IOReturn getAWDL_DEV_STATS(apple80211_awdl_dev_stats *) override;
    IOReturn getAWDL_DFSP_CONFIG(apple80211_awdl_dfsp_config *) override;
    IOReturn getAWDL_CCA(apple80211_awdl_cca *) override;
    IOReturn getAWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES(apple80211_awdl_cap *) override;
    IOReturn getLLW_PARAMS(apple80211_llw_params *) override;
    IOReturn setSSID(apple80211_ssid_data *) override;
    IOReturn setCHANNEL(apple80211_channel_data *) override;
    IOReturn setAWDL_SYNC_PARAMS(apple80211_awdl_sync_params *) override;
    IOReturn setAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled *) override;
    IOReturn setAWDL_EXTENSION_STATE_MACHINE_PARAMETERS(apple80211_awdl_extension_state_machine_parameter *) override;
    IOReturn setAWDL_ELECTION_ID(apple80211_awdl_election_id *) override;
    IOReturn setAWDL_BSSID(apple80211_awdl_bssid *) override;
    IOReturn setAWDL_ELECTION_METRIC(apple80211_awdl_election_metric *) override;
    IOReturn setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence *) override;
    IOReturn setAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel *) override;
    IOReturn setAWDL_SYNC_STATE(apple80211_awdl_sync_state *) override;
    IOReturn setAWDL_ELECTION_RSSI_THRESHOLDS(apple80211_awdl_election_rssi_thresholds *) override;
    IOReturn setAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode *) override;
    IOReturn setPEER_CACHE_CONTROL(apple80211_peer_cache_control *) override;
    IOReturn setAWDL_OOB_REQUEST(apple80211_awdl_oob_request *) override;
    IOReturn setAWDL_SYNC_FRAME_TEMPLATE(apple80211_awdl_sync_frame_template *) override;
    IOReturn setAWDL_ELECTION_PARAMS(apple80211_awdl_election_params *) override;
    IOReturn setAWDL_PEER_TRAFFIC_REGISTRATION(apple80211_awdl_peer_traffic_registration *) override;
    IOReturn setAWDL_SECONDARY_MASTER_CHANNEL(apple80211_awdl_secondary_master_channel *) override;
    IOReturn setAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode *) override;
    IOReturn setAWDL_MIN_RATE(apple80211_awdl_min_rate *) override;
    IOReturn setAWDL_ENCRYPTION_KEYS(apple80211_key *) override;
    IOReturn setAWDL_ENCRYPTION_TYPE(apple80211_encrypt *) override;
    IOReturn setAWDL_OOB_AUTO_REQUEST(apple80211_awdl_oob_request *) override;
    IOReturn setAWDL_TXCAL_PERIOD(apple80211_awdl_txcal_period *) override;
    IOReturn setAWDL_AWDL_ADVERTISERS(awdl_peer_advert_add *) override;
    IOReturn setAWDL_DFSP_CONFIG(apple80211_awdl_dfsp_config *) override;
    IOReturn setAWDL_DFSP_UCSA_CONFIG(apple80211_awdl_dfsp_ucsa_config *) override;
    IOReturn setCOMPANION_SKYWALK_IF_CREATE(apple80211_companion_skywalk_if_create *) override;
    IOReturn setAWDL_OOB_SEC_PAYLOAD_REQUEST(apple80211_awdl_af_sec_payload_request *) override;
    IOReturn setLLW_PARAMS(apple80211_llw_params *) override;
    IOReturn setTX_RATE(apple80211_tx_rate *) override;
    IOReturn setP2P_COEX_PROTECTION(apple80211_p2p_coex_protection *) override;
    IOReturn setPSF_DWELL(apple80211_awdl_psf_dwell_params *) override;
    IOReturn setAWDL_CONFIG(apple80211_awdl_config_request *) override;
    IOReturn setAWDL_RTG_PEER_STATS_CONFIG(apple80211_rtg_peer_stats_cfg *) override;

private:
    IOReturn unsupported(const char *operation);
    IOReturn blocked(MtkAwdl::Result result, const char *blocker);
    template <typename T>
    IOReturn storeRequested(MtkAwdl::RequestedValue<T> &slot, const T *data, const char *name);
    template <typename T>
    IOReturn readRequested(const MtkAwdl::RequestedValue<T> &slot, T *data);
    void releaseNetworkResources();
    void finishStop(IOService *provider, bool dropCleanupRetain);
    IOReturn applyRegistration(const ether_addr &address);
    static UInt32 rejectTransmit(OSObject *, IOSkywalkTxSubmissionQueue *, IOSkywalkPacket *const *, UInt32, void *);

    IO80211Controller *mgmtController = nullptr;
    IOLock *configurationLock = nullptr;
    MtkAwdl::NativeState nativeState;
    MtkAwdl::FrameTemplate frameTemplate;
    MtkAwdl::DuplicateTable duplicates;
    uint8_t localAddress[6] = {};
    bool nativeStarted = false;
    bool draining = false;
    IOService *deferredProvider = nullptr;
    bool registered = false;
    IOSkywalkPacketBufferPool *txPool = nullptr;
    IOSkywalkPacketBufferPool *rxPool = nullptr;
    IOEventSource *packetQueues[4] = {};
    IOWorkLoop *queueWorkLoop = nullptr;
    unsigned addedQueues = 0;
    MtkAwdl::RequestedValue<apple80211_awdl_sync_params> reqSyncParams;
    MtkAwdl::RequestedValue<apple80211_awdl_extension_state_machine_parameter> reqExtension;
    MtkAwdl::RequestedValue<apple80211_awdl_election_id> reqElectionId;
    MtkAwdl::RequestedValue<apple80211_awdl_election_metric> reqElectionMetric;
    MtkAwdl::RequestedValue<apple80211_awdl_sync_channel_sequence> reqSequence;
    MtkAwdl::RequestedValue<apple80211_awdl_master_channel> reqMaster;
    MtkAwdl::RequestedValue<apple80211_awdl_secondary_master_channel> reqSecondary;
    MtkAwdl::RequestedValue<apple80211_awdl_election_rssi_thresholds> reqRssiThresholds;
    MtkAwdl::RequestedValue<apple80211_awdl_presence_mode> reqPresence;
    MtkAwdl::RequestedValue<apple80211_awdl_bssid> reqBssid;
    MtkAwdl::RequestedValue<apple80211_awdl_af_tx_mode> reqAfTxMode;
};

/* Gate and bring-up. The capability switch is separate and is not changed. */
bool mtk_awdl_native_gate_on(void);
bool mtk_awdl_native_set_gate(uint32_t enabled);
bool mtk_awdl_native_read(struct mtk_awdl_native_state *out);
IOReturn mtk_awdl_native_bringup(void *controller, const uint8_t mac[6], unsigned int role, void **outInterface);
void mtk_awdl_native_teardown(void *controller, void *interface);
void mtk_awdl_native_from_rx(const uint8_t *frame, size_t length, int32_t rssiDbm,
    unsigned channel, bool data);

#endif
