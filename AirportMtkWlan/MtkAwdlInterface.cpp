/*
 * MtkAwdlInterface.cpp — default-off on-channel AWDL interface object.
 *
 * Adapted from ForbiddenItlwm AirportItlwmV2.cpp / AWDLBuildGuard.cpp at
 * 69e88fa0fdb0ef7122f2aee81111651c32260a62 (v0.5.4-beta.2), Apple-side
 * only: concrete IO80211AWDLProtocol, Skywalk pool and four queues, and
 * stored setAWDL requests. Not adapted: Intel listener, MCC, Block Ack,
 * master-indication byte parsing, RegistrationInfo offset writes, and
 * RX metadata submit. No LE advertising plugin is included.
 *
 * GPLv2.
 */
#include "AirportItlwmV2.hpp"
#include "MtkAwdlInterface.hpp"
#include "MtkAwdlCap.hpp"
#include <Airport/macOS26/IO80211ManagementBuffer.h>
#include <Airport/macOS26/IOSkywalkPacketView.h>
#include <MtkAwdl/ManagementFrame.hpp>
#include <MtkAwdl/RegistrationImage.hpp>
#include <MtkAwdl/VtableAnchors.hpp>
#include <IOKit/IOLib.h>
#include <libkern/version.h>
#include <string.h>

#define super IO80211AWDLProtocol
OSDefineMetaClassAndStructors(MtkAwdlInterface, IO80211AWDLProtocol)
/* This file also defines AirportItlwm::createVirtualInterface. The alias
 * above is only for OSDefineMetaClassAndStructors. If it stays defined,
 * super:: in the controller method binds to IO80211AWDLProtocol, which
 * has no createVirtualInterface. */
#undef super

static_assert(sizeof(apple80211_awdl_sync_enabled) == sizeof(MtkAwdl::SyncEnabled),
    "sync-enabled ABI");
static_assert(sizeof(apple80211_awdl_sync_state) == sizeof(MtkAwdl::SyncState),
    "sync-state ABI");
static_assert(sizeof(IOSkywalkPacketBufferPool::PoolOptions) == 0x20, "pool options ABI");
static_assert(MTK_AWDLRAW_MAX_FRAME == MtkAwdl::kRawTxMaxFrame, "raw TX stays 800");
static_assert(APPLE80211_VIF_AWDL == 6, "AWDL role");
static_assert(MtkAwdl::kRegistrationPermitted, "registration attempt is allowed");
static_assert(MtkAwdl::registrationCallPermitted(), "generic queue count");
static_assert(MtkAwdl::kSoftwareEthernetQueueCount == 4, "software ethernet queue count");
static_assert(MtkAwdl::genericQueueCountAllowed(1) && MtkAwdl::genericQueueCountAllowed(256), "validator bounds");
static_assert(!MtkAwdl::genericQueueCountAllowed(0) && !MtkAwdl::genericQueueCountAllowed(257), "validator rejects");
static_assert(MtkAwdl::kManagementSubmitPermitted, "management submit transfers the buffer");
static_assert(!MtkAwdl::kRxSubmitPermitted, "RX submit stays closed");
static_assert(!MtkAwdl::kIpv6HandoffPermitted, "IPv6 handoff stays closed");
static_assert(MtkAwdl::kManagementMetadataBytes == 72, "management metadata");
static_assert(MtkAwdl::kManagementSelector == 12, "management selector");
static_assert(MtkAwdl::kRegistrationBytes == 0x130, "RegistrationInfo size");
static_assert(sizeof(apple80211_channel) == sizeof(MtkAwdl::ChannelWords),
    "metadata channel width");
static_assert(!MtkAwdl::kNativeGateDefault, "native gate default off");

extern "C" {
/* Declarations of symbols exported by IO80211Family / libcxx. An unsized
 * array without extern is a definition, which Apple clang rejects. */
extern IO80211VirtualInterface *mtkAwdlCreateSym(AirportItlwm *, ether_addr *, unsigned int)
    __asm__("__ZN12AirportItlwm22createVirtualInterfaceEP10ether_addrj");
extern bool mtkAwdlAttachSym(IO80211Controller *, IO80211VirtualInterface **, ether_addr *, unsigned int, bool)
    __asm__("__ZN17IO80211Controller22attachVirtualInterfaceEPP23IO80211VirtualInterfaceP10ether_addrjb");
extern const uintptr_t mtkAwdlProtoVt[] __asm__("__ZTV19IO80211AWDLProtocol");
extern void mtkAnchorD1() __asm__("__ZN19IO80211AWDLProtocolD1Ev");
extern void mtkAnchorWake() __asm__("__ZN25IOSkywalkNetworkInterface21getSupportedWakeFlagsEPj");
extern void mtkAnchorCca() __asm__("__ZN23IO80211SkywalkInterface15setInterfaceCCAE18apple80211_channeli");
extern void mtkAnchorStats() __asm__("__ZN23IO80211SkywalkInterface9statsDumpEv");
extern void mtkAnchorDps() __asm__("__ZN23IO80211SkywalkInterface12syncDPSStatsEP26apple82011_postMessage_dps");
extern void mtkAnchorReserved15() __asm__("__ZN23IO80211VirtualInterface34_RESERVEDIO80211VirtualInterface15Ev");
extern void mtkAnchorPure() __asm__("___cxa_pure_virtual");
}

static IOSimpleLock *volatile gLock;
static uint32_t gGate;
static uint32_t gBuildOk;
static uint32_t gVtableOk;
static uint32_t gSlotOk;
static uint32_t gQueues;
static uint32_t gRegistered;
static MtkAwdlInterface *volatile gLive;
static char gBlocker[64] = "disabled";
static char gReady[80] = "disabled";

static IOReturn toIO(MtkAwdl::Result result)
{
    switch (result) {
    case MtkAwdl::Ok: return kIOReturnSuccess;
    case MtkAwdl::BadArgument: return kIOReturnBadArgument;
    case MtkAwdl::NotReady: return kIOReturnNotReady;
    case MtkAwdl::Blocked:
    case MtkAwdl::Unsupported:
        return kIOReturnUnsupported;
    }
    return kIOReturnUnsupported;
}

static void copyText(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (!src)
        src = "";
    for (; src[i] && i + 1 < cap; ++i)
        dst[i] = src[i];
    dst[i] = 0;
}

static void ensureLock(void)
{
    if (__atomic_load_n(&gLock, __ATOMIC_ACQUIRE) != nullptr)
        return;
    IOSimpleLock *lock = IOSimpleLockAlloc();
    if (lock == nullptr)
        return;
    if (!OSCompareAndSwapPtr(nullptr, lock, reinterpret_cast<void *volatile *>(&gLock)))
        IOSimpleLockFree(lock);
}

static void publish(const char *blocker, const char *ready)
{
    ensureLock();
    IOSimpleLock *lock = __atomic_load_n(&gLock, __ATOMIC_ACQUIRE);
    if (lock == nullptr)
        return;
    IOSimpleLockLock(lock);
    if (blocker != nullptr)
        copyText(gBlocker, sizeof(gBlocker), blocker);
    if (ready != nullptr)
        copyText(gReady, sizeof(gReady), ready);
    IOSimpleLockUnlock(lock);
}

static bool vtableMatches(void)
{
    const uintptr_t *slots = mtkAwdlProtoVt + 2;
    const uintptr_t expected[] = {
        reinterpret_cast<uintptr_t>(&mtkAnchorD1),
        reinterpret_cast<uintptr_t>(&mtkAnchorWake),
        reinterpret_cast<uintptr_t>(&mtkAnchorCca),
        reinterpret_cast<uintptr_t>(&mtkAnchorStats),
        reinterpret_cast<uintptr_t>(&mtkAnchorDps),
        reinterpret_cast<uintptr_t>(&mtkAnchorReserved15),
        reinterpret_cast<uintptr_t>(&mtkAnchorPure),
        reinterpret_cast<uintptr_t>(&mtkAnchorPure),
        0,
    };
    static_assert(sizeof(expected) / sizeof(expected[0]) == MtkAwdl::kAwdlAnchorCount,
        "anchor table");
    for (unsigned i = 0; i < MtkAwdl::kAwdlAnchorCount; ++i) {
        const unsigned index = MtkAwdl::kAwdlAnchors[i].byteOffset / sizeof(uintptr_t);
        if (slots[index] != expected[i])
            return false;
    }
    return true;
}

static bool slotsMatch(AirportItlwm *controller)
{
    const uintptr_t *vt = *reinterpret_cast<const uintptr_t *const *>(controller);
    return vt[MtkAwdl::kControllerCreateSlot / sizeof(uintptr_t)]
            == reinterpret_cast<uintptr_t>(&mtkAwdlCreateSym)
        && vt[MtkAwdl::kControllerAttachSlot / sizeof(uintptr_t)]
            == reinterpret_cast<uintptr_t>(&mtkAwdlAttachSym);
}

static void rememberChecks(AirportItlwm *controller)
{
    const bool build = MtkAwdl::checkBuild(version, nullptr) == MtkAwdl::BuildCheck::Verified;
    const bool vt = build && vtableMatches();
    const bool slot = vt && controller && slotsMatch(controller);
    gBuildOk = build ? 1u : 0u;
    gVtableOk = vt ? 1u : 0u;
    gSlotOk = slot ? 1u : 0u;
    if (controller) {
        controller->setProperty("MtkAwdlBuildCheck", MtkAwdl::buildCheckName(
            MtkAwdl::checkBuild(version, nullptr)));
        controller->setProperty("MtkAwdlVtableCheck", vt ? "match" : "mismatch");
        controller->setProperty("MtkAwdlControllerSlot", slot ? "match" : "mismatch");
        controller->setProperty("MtkAwdlReference", MtkAwdl::kReferenceCommit);
        controller->setProperty("MtkAwdlRegistrationPermitted", 0ull, 32);
        controller->setProperty("MtkAwdlRxSubmitPermitted", 0ull, 32);
        controller->setProperty("MtkAwdlIpv6HandoffPermitted", 0ull, 32);
        controller->setProperty("MtkAwdlTransportReady", 0ull, 32);
        controller->setProperty("MtkAwdlBsdRegistered", 0ull, 32);
        controller->setProperty("MtkAwdlSecondVif", 0ull, 32);
        controller->setProperty("MtkAwdlRawTxMaxFrame", MtkAwdl::kRawTxMaxFrame, 32);
    }
}

bool mtk_awdl_native_gate_on(void)
{
    return __atomic_load_n(&gGate, __ATOMIC_ACQUIRE) != 0;
}

bool mtk_awdl_native_set_gate(uint32_t enabled)
{
    ensureLock();
    if (__atomic_load_n(&gLock, __ATOMIC_ACQUIRE) == nullptr)
        return false;
    const uint32_t value = enabled ? 1u : 0u;
    const uint32_t old = __atomic_exchange_n(&gGate, value, __ATOMIC_ACQ_REL);
    if (!value) {
        if (__atomic_load_n(&gLive, __ATOMIC_ACQUIRE) != nullptr)
            mtk_awdlcap_vif_drop();
        gQueues = 0;
        publish(MtkAwdl::kReadinessDisabled, MtkAwdl::kReadinessDisabled);
    }
    IOLog("AirportMtkWlan: native awdl gate %u -> %u (default off, no BSD awdl0, no transport)\n",
        old, value);
    return true;
}

bool mtk_awdl_native_read(struct mtk_awdl_native_state *out)
{
    if (out == nullptr)
        return false;
    ensureLock();
    IOSimpleLock *lock = __atomic_load_n(&gLock, __ATOMIC_ACQUIRE);
    memset(out, 0, sizeof(*out));
    out->abi = MTK_AWDL_NATIVE_ABI;
    out->enabled = mtk_awdl_native_gate_on() ? 1u : 0u;
    out->build_allowed = gBuildOk;
    out->vtable_ok = gVtableOk;
    out->controller_slot_ok = gSlotOk;
    out->registration_permitted = MtkAwdl::registrationCallPermitted() ? 1u : 0u;
    out->rx_submit_permitted = 0;
    out->ipv6_handoff_permitted = 0;
    out->transport_ready = 0;
    out->bsd_registered = gRegistered;
    out->interface_live = __atomic_load_n(&gLive, __ATOMIC_ACQUIRE) != nullptr;
    out->queues_up = gQueues;
    out->raw_tx_max_frame = MtkAwdl::kRawTxMaxFrame;
    out->second_vif = 0;
    if (lock != nullptr) {
        IOSimpleLockLock(lock);
        copyText(out->blocker, sizeof(out->blocker), gBlocker);
        copyText(out->readiness, sizeof(out->readiness), gReady);
        IOSimpleLockUnlock(lock);
    } else {
        copyText(out->blocker, sizeof(out->blocker), "disabled");
        copyText(out->readiness, sizeof(out->readiness), MtkAwdl::kReadinessDisabled);
    }
    copyText(out->reference, sizeof(out->reference), MtkAwdl::kReferenceCommit);
    return true;
}

IO80211VirtualInterface *AirportItlwm::createVirtualInterface(ether_addr *address, unsigned int role)
{
    if (mtk_awdl_native_gate_on() && role == APPLE80211_VIF_AWDL && address != nullptr)
        return MtkAwdlInterface::make(this, address, role);
    return IO80211Controller::createVirtualInterface(address, role);
}

MtkAwdlInterface *MtkAwdlInterface::make(AirportItlwm *controller, const ether_addr *address, unsigned int role)
{
    if (__atomic_load_n(&gLive, __ATOMIC_ACQUIRE) != nullptr)
        return nullptr;
    auto *interface = new MtkAwdlInterface;
    if (interface == nullptr)
        return nullptr;
    if (!interface->init(controller, const_cast<ether_addr *>(address), role, "awdl")) {
        interface->release();
        return nullptr;
    }
    if (!OSCompareAndSwapPtr(nullptr, interface, reinterpret_cast<void *volatile *>(&gLive))) {
        interface->release();
        return nullptr;
    }
    return interface;
}

void MtkAwdlInterface::releaseNetworkResources()
{
    draining = true;
    if (registered) {
        deregisterEthernetInterface(0);
        registered = false;
        gRegistered = 0;
    }
    for (unsigned i = addedQueues; i > 0; --i) {
        if (packetQueues[i - 1] != nullptr) {
            packetQueues[i - 1]->disable();
            if (queueWorkLoop != nullptr)
                queueWorkLoop->removeEventSource(packetQueues[i - 1]);
        }
    }
    addedQueues = 0;
    gQueues = 0;
    for (auto &queue : packetQueues)
        OSSafeReleaseNULL(queue);
    OSSafeReleaseNULL(txPool);
    OSSafeReleaseNULL(rxPool);
    OSSafeReleaseNULL(queueWorkLoop);
}

void MtkAwdlInterface::stop(IOService *provider)
{
    const bool started = nativeStarted;
    nativeStarted = false;
    releaseNetworkResources();
    if (started)
        IO80211AWDLProtocol::stop(provider);
}

void MtkAwdlInterface::free()
{
    if (nativeStarted)
        stop(getProvider());
    if (configurationLock != nullptr) {
        IOLockFree(configurationLock);
        configurationLock = nullptr;
    }
    if (__atomic_load_n(&gLive, __ATOMIC_ACQUIRE) == this)
        OSCompareAndSwapPtr(this, nullptr, reinterpret_cast<void *volatile *>(&gLive));
    IO80211AWDLProtocol::free();
}

IOReturn MtkAwdlInterface::initializeNetwork(IO80211Controller *controller, IOWorkLoop *workLoop,
    const ether_addr &address)
{
    auto stage = [&](const char *name) {
        setProperty("MtkAwdlNetworkStage", name);
        if (controller != nullptr)
            controller->setProperty("MtkAwdlNetworkStage", name);
    };
    if (controller == nullptr || workLoop == nullptr || nativeStarted)
        return kIOReturnBadArgument;
    if (configurationLock == nullptr)
        configurationLock = IOLockAlloc();
    if (configurationLock == nullptr)
        return kIOReturnNoMemory;
    memcpy(localAddress, &address, sizeof(localAddress));
    stage("starting");
    if (!IO80211AWDLProtocol::start(controller)) {
        stage("native-start-failed");
        publish("native-start-failed", "native-start-failed");
        IOLockFree(configurationLock);
        configurationLock = nullptr;
        return kIOReturnError;
    }
    nativeStarted = true;
    nativeState.setAccept(true);
    queueWorkLoop = workLoop;
    queueWorkLoop->retain();
    const IOSkywalkPacketBufferPool::PoolOptions options = {128, 128, 2048, 1, 4096, 0, 0};
    txPool = IOSkywalkPacketBufferPool::withName("mtk-awdl-tx", this, 0, &options);
    rxPool = IOSkywalkPacketBufferPool::withName("mtk-awdl-rx", this, 0, &options);
    if (txPool == nullptr || rxPool == nullptr) {
        stage("pool-alloc-failed");
        publish("pool-alloc-failed", "pool-alloc-failed");
        stop(controller);
        return kIOReturnNoMemory;
    }
    packetQueues[0] = itlMakeTxSubmission(txPool, 64, 0, this, rejectTransmit, nullptr, 0);
    packetQueues[1] = itlMakeTxCompletion(txPool, 64, 0, this, nullptr, nullptr, 0);
    packetQueues[2] = itlMakeRxSubmission(rxPool, 64, 1, 0, this, nullptr, nullptr, 0);
    packetQueues[3] = itlMakeRxCompletion(rxPool, 64, 0, this, nullptr, nullptr, 0);
    for (auto *queue : packetQueues) {
        if (queue == nullptr) {
            stage("queue-alloc-failed");
            publish("queue-alloc-failed", "queue-alloc-failed");
            stop(controller);
            return kIOReturnNoMemory;
        }
    }
    for (auto *queue : packetQueues) {
        const IOReturn added = queueWorkLoop->addEventSource(queue);
        if (added != kIOReturnSuccess) {
            stage("queue-add-failed");
            publish("queue-add-failed", "queue-add-failed");
            stop(controller);
            return added;
        }
        ++addedQueues;
    }
    const IOReturn registeredResult = applyRegistration(address);
    if (registeredResult != kIOReturnSuccess) {
        stage(MtkAwdl::kBlockerRegistration);
        publish(MtkAwdl::kBlockerRegistration, MtkAwdl::kBlockerRegistration);
        controller->setProperty("MtkAwdlRegisterResult", (uint32_t)registeredResult, 32);
        stop(controller);
        return registeredResult;
    }
    /* TX submission stays disabled so Skywalk cannot feed the station ring.
     * RX submission prepares packets; RX completion receives software Ethernet. */
    if (packetQueues[2] != nullptr)
        packetQueues[2]->enable();
    if (packetQueues[3] != nullptr)
        packetQueues[3]->enable();
    gQueues = 1;
    setProperty("MtkAwdlConfigurationMode", "software-ethernet");
    setProperty("MtkAwdlBsdRegistered", 1ull, 32);
    setProperty("MtkAwdlTransportReady", 0ull, 32);
    setProperty("MtkAwdlOnChannelOnly", 1ull, 32);
    setProperty("MtkAwdlQueueCount", MtkAwdl::kSoftwareEthernetQueueCount, 32);
    setProperty("MtkAwdlIpv6Blocker", MtkAwdl::kBlockerIpv6);
    stage("registered-software-ethernet");
    publish("registered-software-ethernet", "registered-software-ethernet");
    controller->setProperty("MtkAwdlBlocker", "registered-software-ethernet");
    return kIOReturnSuccess;
}

UInt32 MtkAwdlInterface::rejectTransmit(OSObject *, IOSkywalkTxSubmissionQueue *,
    IOSkywalkPacket *const *, UInt32, void *)
{
    return MtkAwdl::skywalkTxAcceptCount();
}

IOReturn MtkAwdlInterface::applyRegistration(const ether_addr &address)
{
    IOSkywalkEthernetInterface::RegistrationInfo info = {};
    if (!initRegistrationInfo(&info, 1, sizeof(info)))
        return kIOReturnBadArgument;
    MtkAwdl::RegistrationImage image;
    /* This object does not publish IOPMResetPowerStateOnWake, so flags are 4. */
    const MtkAwdl::Result filled = MtkAwdl::fillRegistrationImage(&image, address.octet,
        MtkAwdl::flagsForWakeResetProperty(false), MtkAwdl::kRegistrationPrefix);
    if (filled != MtkAwdl::Ok)
        return kIOReturnBadArgument;
    memcpy(info.bytes, image.bytes, sizeof(info.bytes));
    IOSkywalkPacketQueue *queues[4];
    for (unsigned i = 0; i < 4; ++i)
        queues[i] = reinterpret_cast<IOSkywalkPacketQueue *>(packetQueues[i]);
    const IOReturn result = registerEthernetInterface(&info, queues,
        MtkAwdl::kSoftwareEthernetQueueCount, txPool, rxPool, 0);
    if (result != kIOReturnSuccess)
        return result;
    registered = true;
    gRegistered = 1;
    return kIOReturnSuccess;
}

void MtkAwdlInterface::offerOnGate(const uint8_t *frame, size_t length, int32_t rssiDbm,
    unsigned channel, bool data)
{
    if (draining || !registered || frame == nullptr)
        return;
    if (!data) {
        if (length < 25 || MtkAwdl::checkManagementLength(MtkAwdl::kManagementMetadataBytes) != MtkAwdl::Ok)
            return;
        auto *pool = itlGetMgmtFramePool(this);
        if (pool == nullptr)
            return;
        IO80211Buffer *buffer = pool->getBuffer();
        if (buffer == nullptr)
            return;
        uint8_t *bytes = static_cast<uint8_t *>(buffer->getBytesNoCopy());
        const size_t body = length - 24;
        if (bytes == nullptr || body > buffer->getCapacity()) {
            buffer->returnBuffer();
            return;
        }
        memcpy(bytes, frame + 24, body);
        buffer->setLength(body);
        uint8_t meta[MtkAwdl::kManagementMetadataBytes];
        const MtkAwdl::ChannelWords words = MtkAwdl::legacy20ChannelWords(channel);
        if (channel == 0 || words.number != channel ||
            MtkAwdl::writeManagementMetadata(meta, frame + 10, &words, rssiDbm) != MtkAwdl::Ok) {
            buffer->returnBuffer();
            return;
        }
        itlSubmitMgmtFrame(this, buffer, MtkAwdl::kManagementSelector, meta, sizeof(meta));
        return;
    }
    MtkAwdl::DataView view = {};
    if (MtkAwdl::parseAwdlData(frame, length, &view) != MtkAwdl::Ok)
        return;
    const bool multicast = (view.destination[0] & 1) != 0;
    if ((!multicast && memcmp(view.destination, localAddress, 6) != 0) ||
        memcmp(view.source, localAddress, 6) == 0)
        return;
    if (duplicates.seen(view.source, view.sequence))
        return;
    if (rxPool == nullptr || packetQueues[2] == nullptr || packetQueues[3] == nullptr)
        return;
    auto *pool = reinterpret_cast<ItlSkywalkPoolView *>(rxPool);
    ItlSkywalkPacketView *packet = nullptr;
    if (pool->allocatePacket(1, &packet, 1) != kIOReturnSuccess || packet == nullptr)
        return;
    const size_t wanted = 14 + view.ipv6Length;
    bool consumed = false;
    if (packet->prepareWithQueue(packetQueues[2], 0, 0) == kIOReturnSuccess &&
        packet->getPacketBufferCount() == 1 &&
        packet->setDataLength((unsigned)wanted) == kIOReturnSuccess) {
        auto *bytes = reinterpret_cast<uint8_t *>(packet->getDataVirtualAddress());
        size_t written = 0;
        if (bytes != nullptr &&
            MtkAwdl::copyEthernetIPv6(view, bytes, wanted, &written) == MtkAwdl::Ok &&
            written == wanted &&
            itlPacketSetHeadroom(packet, 0) == kIOReturnSuccess &&
            itlPacketSetLinkHeaderLength(packet, 14) == kIOReturnSuccess &&
            itlRxEnqueue(packetQueues[3], &packet, 1, 0) == kIOReturnSuccess) {
            duplicates.remember(view.source, view.sequence);
            consumed = true;
        }
    }
    if (!consumed) {
        packet->setDataLength(0);
        packet->completeWithQueue(packetQueues[2], 0, 0);
        pool->deallocatePacket(packet);
    }
}

struct MtkAwdlRxArgs {
    MtkAwdlInterface *interface;
    const uint8_t *frame;
    size_t length;
    int32_t rssi;
    unsigned channel;
    bool data;
};

static IOReturn mtkAwdlRxAction(OSObject *, void *arg0, void *, void *, void *)
{
    auto *args = static_cast<MtkAwdlRxArgs *>(arg0);
    args->interface->offerOnGate(args->frame, args->length, args->rssi, args->channel, args->data);
    return kIOReturnSuccess;
}

void MtkAwdlInterface::dispatchRx(const uint8_t *frame, size_t length, int32_t rssiDbm,
    unsigned channel, bool data)
{
    if (draining || !registered || queueWorkLoop == nullptr || frame == nullptr)
        return;
    MtkAwdlRxArgs args = {this, frame, length, rssiDbm, channel, data};
    queueWorkLoop->runAction(mtkAwdlRxAction, this, &args, nullptr, nullptr, nullptr);
}

void mtk_awdl_native_from_rx(const uint8_t *frame, size_t length, int32_t rssiDbm,
    unsigned channel, bool data)
{
    auto *live = __atomic_load_n(&gLive, __ATOMIC_ACQUIRE);
    if (!mtk_awdl_native_gate_on() || live == nullptr)
        return;
    live->dispatchRx(frame, length, rssiDbm, channel, data);
}

IOReturn mtk_awdl_native_bringup(void *controller, const uint8_t mac[6], unsigned int role, void **outInterface)
{
    if (outInterface != nullptr)
        *outInterface = nullptr;
    auto *ctrl = OSDynamicCast(AirportItlwm, static_cast<OSObject *>(controller));
    if (ctrl == nullptr || mac == nullptr || role != APPLE80211_VIF_AWDL)
        return kIOReturnBadArgument;
    if (!mtk_awdl_native_gate_on())
        return kIOReturnUnsupported;
    if (__atomic_load_n(&gLive, __ATOMIC_ACQUIRE) != nullptr) {
        if (outInterface != nullptr)
            *outInterface = __atomic_load_n(&gLive, __ATOMIC_ACQUIRE);
        return kIOReturnSuccess;
    }
    rememberChecks(ctrl);
    if (!gBuildOk) {
        publish(MtkAwdl::kBlockerBuild, MtkAwdl::kBlockerBuild);
        ctrl->setProperty("MtkAwdlBlocker", MtkAwdl::kBlockerBuild);
        return kIOReturnUnsupported;
    }
    if (!gVtableOk) {
        publish(MtkAwdl::kBlockerVtable, MtkAwdl::kBlockerVtable);
        ctrl->setProperty("MtkAwdlBlocker", MtkAwdl::kBlockerVtable);
        return kIOReturnUnsupported;
    }
    if (!gSlotOk) {
        publish(MtkAwdl::kBlockerSlot, MtkAwdl::kBlockerSlot);
        ctrl->setProperty("MtkAwdlBlocker", MtkAwdl::kBlockerSlot);
        return kIOReturnUnsupported;
    }

    ether_addr address = {};
    memcpy(address.octet, mac, sizeof(address.octet));
    IO80211VirtualInterface *slot = nullptr;
    mtk_awdlcap_note(MTK_AWDLCAP_STEP_ALLOC, kIOReturnSuccess, mac, "concrete-factory");
    const bool attached = ctrl->attachVirtualInterface(&slot, &address, role, false);
    if (!attached || slot == nullptr) {
        if (slot != nullptr) {
            slot->detach(ctrl);
            slot->release();
        }
        OSCompareAndSwapPtr(slot, nullptr, reinterpret_cast<void *volatile *>(&gLive));
        mtk_awdlcap_note(MTK_AWDLCAP_STEP_ATTACH, kIOReturnError, mac, "attach");
        publish("attach-failed", "attach-failed");
        return kIOReturnError;
    }
    mtk_awdlcap_note(MTK_AWDLCAP_STEP_ATTACH, kIOReturnSuccess, mac, "attach");
    auto *interface = static_cast<MtkAwdlInterface *>(slot);
    const IOReturn started = interface->initializeNetwork(ctrl, ctrl->getWorkQueue(), address);
    if (started != kIOReturnSuccess) {
        interface->detach(ctrl);
        interface->release();
        OSCompareAndSwapPtr(interface, nullptr, reinterpret_cast<void *volatile *>(&gLive));
        mtk_awdlcap_note(MTK_AWDLCAP_STEP_INIT, started, mac, "initialize");
        return started;
    }
    /* Registration and peer-manager RX/IPv6 submit stay closed. */
    mtk_awdlcap_note(MTK_AWDLCAP_STEP_REGISTER, kIOReturnUnsupported, mac,
        MtkAwdl::kBlockerRegistration);
    mtk_awdlcap_note(MTK_AWDLCAP_STEP_DONE, kIOReturnSuccess, mac, MtkAwdl::kReadinessControl);
    if (!mtk_awdl_native_gate_on()) {
        mtk_awdl_native_teardown(ctrl, interface);
        return kIOReturnUnsupported;
    }
    if (outInterface != nullptr)
        *outInterface = interface;
    IOLog("AirportMtkWlan: native AWDL control object attached; registration, management submit and IPv6 handoff stay closed\n");
    return kIOReturnSuccess;
}

void mtk_awdl_native_teardown(void *controller, void *interface)
{
    auto *ctrl = OSDynamicCast(AirportItlwm, static_cast<OSObject *>(controller));
    auto *obj = OSDynamicCast(MtkAwdlInterface, static_cast<OSObject *>(interface));
    if (obj == nullptr)
        return;
    OSCompareAndSwapPtr(obj, nullptr, reinterpret_cast<void *volatile *>(&gLive));
    gQueues = 0;
    obj->stop(ctrl);
    if (ctrl != nullptr) {
        obj->detach(ctrl);
        ctrl->detachVirtualInterface(obj, false);
    }
    obj->release();
    publish(mtk_awdl_native_gate_on() ? "dropped" : MtkAwdl::kReadinessDisabled,
        mtk_awdl_native_gate_on() ? "dropped" : MtkAwdl::kReadinessDisabled);
}

IOReturn MtkAwdlInterface::unsupported(const char *operation)
{
    setProperty("MtkAwdlLastUnsupported", operation);
    return kIOReturnUnsupported;
}

IOReturn MtkAwdlInterface::blocked(MtkAwdl::Result result, const char *blocker)
{
    setProperty("MtkAwdlBlocker", blocker);
    publish(blocker, nullptr);
    return toIO(result);
}

template <typename T>
IOReturn MtkAwdlInterface::storeRequested(MtkAwdl::RequestedValue<T> &slot, const T *data, const char *name)
{
    if (configurationLock == nullptr)
        return kIOReturnNotReady;
    IOLockLock(configurationLock);
    const MtkAwdl::Result result = slot.set(data);
    IOLockUnlock(configurationLock);
    if (result == MtkAwdl::Ok)
        setProperty("MtkAwdlLastStoredRequest", name);
    return toIO(result);
}

template <typename T>
IOReturn MtkAwdlInterface::readRequested(const MtkAwdl::RequestedValue<T> &slot, T *data)
{
    if (configurationLock == nullptr)
        return kIOReturnNotReady;
    IOLockLock(configurationLock);
    const MtkAwdl::Result result = slot.get(data);
    IOLockUnlock(configurationLock);
    return toIO(result);
}

IOReturn MtkAwdlInterface::setMacAddress(ether_addr &)
{
    return unsupported("setMacAddress");
}

#define RET_UNSUP(fn, arg) \
IOReturn MtkAwdlInterface::fn(arg) { return unsupported(#fn); }

RET_UNSUP(getSSID, apple80211_ssid_data *)
RET_UNSUP(getCHANNEL, apple80211_channel_data *)
RET_UNSUP(getBSSID, apple80211_bssid_data *)
RET_UNSUP(getSTATE, apple80211_state_data *)
RET_UNSUP(getPEER_CACHE_MAXIMUM_SIZE, apple80211_peer_cache_maximum_size *)
RET_UNSUP(getAWDL_ELECTION_PARAMS, apple80211_awdl_election_params *)
RET_UNSUP(getAWDL_DEVICE_CAPABILITIES, apple80211_awdl_device_capabilities *)
RET_UNSUP(getAWDL_MIN_RATE, apple80211_awdl_min_rate *)
RET_UNSUP(getAWDL_ENCRYPTION_TYPE, apple80211_encrypt *)
RET_UNSUP(getAWDL_TXCAL_PERIOD, apple80211_awdl_txcal_period *)
RET_UNSUP(getAWDL_DEV_STATS, apple80211_awdl_dev_stats *)
RET_UNSUP(getAWDL_DFSP_CONFIG, apple80211_awdl_dfsp_config *)
RET_UNSUP(getAWDL_CCA, apple80211_awdl_cca *)
RET_UNSUP(getAWDL_AF_SECONDARY_PAYLOAD_CAPABILITIES, apple80211_awdl_cap *)
RET_UNSUP(getLLW_PARAMS, apple80211_llw_params *)
RET_UNSUP(setSSID, apple80211_ssid_data *)
RET_UNSUP(setPEER_CACHE_CONTROL, apple80211_peer_cache_control *)
RET_UNSUP(setAWDL_ELECTION_PARAMS, apple80211_awdl_election_params *)
RET_UNSUP(setAWDL_PEER_TRAFFIC_REGISTRATION, apple80211_awdl_peer_traffic_registration *)
RET_UNSUP(setAWDL_MIN_RATE, apple80211_awdl_min_rate *)
RET_UNSUP(setAWDL_ENCRYPTION_KEYS, apple80211_key *)
RET_UNSUP(setAWDL_ENCRYPTION_TYPE, apple80211_encrypt *)
RET_UNSUP(setAWDL_TXCAL_PERIOD, apple80211_awdl_txcal_period *)
RET_UNSUP(setAWDL_AWDL_ADVERTISERS, awdl_peer_advert_add *)
RET_UNSUP(setAWDL_DFSP_CONFIG, apple80211_awdl_dfsp_config *)
RET_UNSUP(setAWDL_DFSP_UCSA_CONFIG, apple80211_awdl_dfsp_ucsa_config *)
RET_UNSUP(setAWDL_OOB_SEC_PAYLOAD_REQUEST, apple80211_awdl_af_sec_payload_request *)
RET_UNSUP(setLLW_PARAMS, apple80211_llw_params *)
RET_UNSUP(setTX_RATE, apple80211_tx_rate *)
RET_UNSUP(setP2P_COEX_PROTECTION, apple80211_p2p_coex_protection *)
RET_UNSUP(setPSF_DWELL, apple80211_awdl_psf_dwell_params *)
RET_UNSUP(setAWDL_CONFIG, apple80211_awdl_config_request *)
RET_UNSUP(setAWDL_RTG_PEER_STATS_CONFIG, apple80211_rtg_peer_stats_cfg *)
#undef RET_UNSUP

IOReturn MtkAwdlInterface::setCHANNEL(apple80211_channel_data *)
{
    return blocked(MtkAwdl::setChannelRequest(), MtkAwdl::kBlockerOffchannel);
}

IOReturn MtkAwdlInterface::setCOMPANION_SKYWALK_IF_CREATE(apple80211_companion_skywalk_if_create *)
{
    return blocked(MtkAwdl::setCompanionInterface(), MtkAwdl::kBlockerSecondVif);
}

IOReturn MtkAwdlInterface::setAWDL_OOB_REQUEST(apple80211_awdl_oob_request *)
{
    return blocked(MtkAwdl::setOobRequest(), MtkAwdl::kBlockerOob);
}

IOReturn MtkAwdlInterface::setAWDL_OOB_AUTO_REQUEST(apple80211_awdl_oob_request *)
{
    return blocked(MtkAwdl::setOobRequest(), MtkAwdl::kBlockerOob);
}

IOReturn MtkAwdlInterface::getAWDL_SYNC_PARAMS(apple80211_awdl_sync_params *data)
{
    return readRequested(reqSyncParams, data);
}
IOReturn MtkAwdlInterface::setAWDL_SYNC_PARAMS(apple80211_awdl_sync_params *data)
{
    return storeRequested(reqSyncParams, data, "AWDL_SYNC_PARAMS");
}
IOReturn MtkAwdlInterface::getAWDL_EXTENSION_STATE_MACHINE_PARAMETERS(apple80211_awdl_extension_state_machine_parameter *data)
{
    return readRequested(reqExtension, data);
}
IOReturn MtkAwdlInterface::setAWDL_EXTENSION_STATE_MACHINE_PARAMETERS(apple80211_awdl_extension_state_machine_parameter *data)
{
    return storeRequested(reqExtension, data, "AWDL_EXTENSION_STATE_MACHINE_PARAMETERS");
}
IOReturn MtkAwdlInterface::getAWDL_ELECTION_ID(apple80211_awdl_election_id *data)
{
    return readRequested(reqElectionId, data);
}
IOReturn MtkAwdlInterface::setAWDL_ELECTION_ID(apple80211_awdl_election_id *data)
{
    return storeRequested(reqElectionId, data, "AWDL_ELECTION_ID");
}
IOReturn MtkAwdlInterface::getAWDL_ELECTION_METRIC(apple80211_awdl_election_metric *data)
{
    return readRequested(reqElectionMetric, data);
}
IOReturn MtkAwdlInterface::setAWDL_ELECTION_METRIC(apple80211_awdl_election_metric *data)
{
    return storeRequested(reqElectionMetric, data, "AWDL_ELECTION_METRIC");
}
IOReturn MtkAwdlInterface::getAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence *data)
{
    return readRequested(reqSequence, data);
}
IOReturn MtkAwdlInterface::setAWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE(apple80211_awdl_sync_channel_sequence *data)
{
    /* Stored only. The sequence is not applied to firmware and does not retune. */
    return storeRequested(reqSequence, data, "AWDL_SYNCHRONIZATION_CHANNEL_SEQUENCE");
}
IOReturn MtkAwdlInterface::getAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel *data)
{
    return readRequested(reqMaster, data);
}
IOReturn MtkAwdlInterface::setAWDL_MASTER_CHANNEL(apple80211_awdl_master_channel *data)
{
    return storeRequested(reqMaster, data, "AWDL_MASTER_CHANNEL");
}
IOReturn MtkAwdlInterface::getAWDL_SECONDARY_MASTER_CHANNEL(apple80211_awdl_secondary_master_channel *data)
{
    return readRequested(reqSecondary, data);
}
IOReturn MtkAwdlInterface::setAWDL_SECONDARY_MASTER_CHANNEL(apple80211_awdl_secondary_master_channel *data)
{
    return storeRequested(reqSecondary, data, "AWDL_SECONDARY_MASTER_CHANNEL");
}
IOReturn MtkAwdlInterface::getAWDL_ELECTION_RSSI_THRESHOLDS(apple80211_awdl_election_rssi_thresholds *data)
{
    return readRequested(reqRssiThresholds, data);
}
IOReturn MtkAwdlInterface::setAWDL_ELECTION_RSSI_THRESHOLDS(apple80211_awdl_election_rssi_thresholds *data)
{
    return storeRequested(reqRssiThresholds, data, "AWDL_ELECTION_RSSI_THRESHOLDS");
}
IOReturn MtkAwdlInterface::getAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode *data)
{
    return readRequested(reqPresence, data);
}
IOReturn MtkAwdlInterface::setAWDL_PRESENCE_MODE(apple80211_awdl_presence_mode *data)
{
    return storeRequested(reqPresence, data, "AWDL_PRESENCE_MODE");
}
IOReturn MtkAwdlInterface::setAWDL_BSSID(apple80211_awdl_bssid *data)
{
    return storeRequested(reqBssid, data, "AWDL_BSSID");
}
IOReturn MtkAwdlInterface::getAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode *data)
{
    return readRequested(reqAfTxMode, data);
}
IOReturn MtkAwdlInterface::setAWDL_AF_TX_MODE(apple80211_awdl_af_tx_mode *data)
{
    /* Does not change the raw TX rate. That path stays 6 Mbps unless the
     * caller of the raw channel asks for another OFDM rate, still capped
     * at 800 bytes, and still on the current channel. */
    return storeRequested(reqAfTxMode, data, "AWDL_AF_TX_MODE");
}

IOReturn MtkAwdlInterface::getAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled *data)
{
    if (data == nullptr)
        return kIOReturnBadArgument;
    MtkAwdl::SyncEnabled local = {};
    const IOReturn result = toIO(nativeState.getEnabled(&local));
    if (result == kIOReturnSuccess)
        memcpy(data, &local, sizeof(local));
    return result;
}

IOReturn MtkAwdlInterface::setAWDL_SYNC_ENABLED(apple80211_awdl_sync_enabled *data)
{
    if (data == nullptr)
        return kIOReturnBadArgument;
    MtkAwdl::SyncEnabled local = {};
    memcpy(&local, data, sizeof(local));
    const MtkAwdl::Result result = nativeState.setEnabled(&local);
    if (result == MtkAwdl::Unsupported)
        setProperty("MtkAwdlBlocker", MtkAwdl::kBlockerTransport);
    if (result == MtkAwdl::Ok)
        setProperty("MtkAwdlSyncEnabledRequested", local.enabled, 32);
    return toIO(result);
}

IOReturn MtkAwdlInterface::getAWDL_SYNC_STATE(apple80211_awdl_sync_state *data)
{
    if (data == nullptr)
        return kIOReturnBadArgument;
    MtkAwdl::SyncState local = {};
    const IOReturn result = toIO(nativeState.getState(&local));
    if (result == kIOReturnSuccess)
        memcpy(data, &local, sizeof(local));
    return result;
}

IOReturn MtkAwdlInterface::setAWDL_SYNC_STATE(apple80211_awdl_sync_state *data)
{
    if (data == nullptr)
        return kIOReturnBadArgument;
    MtkAwdl::SyncState local = {};
    memcpy(&local, data, sizeof(local));
    const MtkAwdl::Result result = nativeState.setState(&local);
    if (result == MtkAwdl::Ok)
        setProperty("MtkAwdlSyncStateRequested", local.state, 32);
    return toIO(result);
}

IOReturn MtkAwdlInterface::setAWDL_SYNC_FRAME_TEMPLATE(apple80211_awdl_sync_frame_template *data)
{
    if (data == nullptr || data->version > 1)
        return kIOReturnBadArgument;
    if (configurationLock == nullptr)
        return kIOReturnNotReady;
    IOLockLock(configurationLock);
    const MtkAwdl::Result result = frameTemplate.set(data->payload, data->payload_len);
    IOLockUnlock(configurationLock);
    if (result == MtkAwdl::Ok)
        setProperty("MtkAwdlLastStoredRequest", "AWDL_SYNC_FRAME_TEMPLATE");
    return toIO(result);
}

__attribute__((destructor)) static void mtk_awdl_native_unload(void)
{
    IOSimpleLock *lock = gLock;
    gLock = nullptr;
    if (lock != nullptr)
        IOSimpleLockFree(lock);
}
