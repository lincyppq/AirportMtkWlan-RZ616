/* Host checks for the on-channel AWDL policy. No kernel and no hardware. */
#include <MtkAwdl/ManagementFrame.hpp>
#include <MtkAwdl/SoftwareRx.hpp>
#include <MtkAwdl/TransmitAdapter.hpp>
#include <MtkAwdl/NativePolicy.hpp>
#include <MtkAwdl/RegistrationImage.hpp>
#include <MtkAwdl/VtableAnchors.hpp>
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect(bool ok, const char *what)
{
    if (ok)
        return;
    fprintf(stderr, "FAIL %s\n", what);
    ++failures;
}

int main(void)
{
    expect(!MtkAwdl::kNativeGateDefault, "gate defaults off");
    expect(MtkAwdl::kRegistrationPermitted, "registration attempt allowed");
    expect(!MtkAwdl::kRxSubmitPermitted, "rx submit closed");
    expect(!MtkAwdl::kIpv6HandoffPermitted, "ipv6 handoff closed");
    expect(!MtkAwdl::kSecondVifPermitted, "no second vif");
    expect(!MtkAwdl::kOffchannelPermitted, "no offchannel");
    expect(!MtkAwdl::kTransportReady, "transport not ready");
    expect(MtkAwdl::kRawTxMaxFrame == 800, "raw tx cap");
    expect(MtkAwdl::kControllerCreateSlot == 0xdd8, "create slot");
    expect(MtkAwdl::kControllerAttachSlot == 0xde0, "attach slot");
    expect(MtkAwdl::kAwdlVtableSlots == 568, "slot count");
    expect(MtkAwdl::kAwdlAnchorCount == 9, "anchor count");
    expect(MtkAwdl::kAwdlAnchors[0].byteOffset == 0, "first anchor");
    expect(MtkAwdl::kAwdlAnchors[8].symbol == nullptr, "table end");
    expect(MtkAwdl::kAwdlAnchors[8].byteOffset == 0x11c0, "end offset");

    const char *kernel = "Darwin Kernel Version 25.7.1: root:xnu-12377.161.15.700.19~2/RELEASE_X86_64";
    const char *other = "Darwin Kernel Version 25.7.2: root:xnu-12377.161.15.701.48~2/RELEASE_X86_64";
    expect(MtkAwdl::checkBuild(kernel, nullptr) == MtkAwdl::BuildCheck::Verified, "25G241 kernel");
    expect(MtkAwdl::checkBuild(kernel, "25G241") == MtkAwdl::BuildCheck::Verified, "25G241 pair");
    expect(MtkAwdl::checkBuild(kernel, "25G332") == MtkAwdl::BuildCheck::UnknownBuild, "other build blocked");
    expect(MtkAwdl::checkBuild(other, "25G332") == MtkAwdl::BuildCheck::UnknownKernel, "25G332 kernel blocked");
    expect(MtkAwdl::checkBuild("", "25G241") == MtkAwdl::BuildCheck::UnknownKernel, "empty kernel");

    MtkAwdl::NativeState state;
    MtkAwdl::SyncState sync = {1, 1};
    expect(state.setState(&sync) == MtkAwdl::Ok, "disable without accept");
    sync.state = 2;
    expect(state.setState(&sync) == MtkAwdl::Unsupported, "active state refused");
    state.setAccept(true);
    expect(state.setState(&sync) == MtkAwdl::Ok, "active state recorded");
    MtkAwdl::SyncState got = {};
    expect(state.getState(&got) == MtkAwdl::Ok && got.state == 2, "recorded state");
    expect(!MtkAwdl::kTransportReady, "recording is not transport");
    sync.state = 0;
    expect(state.setState(&sync) == MtkAwdl::BadArgument, "state 0");
    sync.state = 4;
    expect(state.setState(&sync) == MtkAwdl::BadArgument, "state 4");

    MtkAwdl::NativeState fresh;
    MtkAwdl::SyncEnabled en = {1, 0, 1};
    expect(fresh.setEnabled(&en) == MtkAwdl::Unsupported, "enable refused");
    fresh.setAccept(true);
    expect(fresh.setEnabled(&en) == MtkAwdl::Ok, "enable recorded");
    en.version = 2;
    expect(fresh.setEnabled(&en) == MtkAwdl::BadArgument, "version 2");

    struct Rec { uint32_t version; uint32_t value; };
    MtkAwdl::RequestedValue<Rec> slot;
    Rec in = {0, 9};
    expect(slot.set(&in) == MtkAwdl::Ok && slot.isValid(), "store version 0");
    Rec out = {};
    expect(slot.get(&out) == MtkAwdl::Ok && out.version == 1 && out.value == 9, "read stored");
    in.version = 3;
    expect(slot.set(&in) == MtkAwdl::BadArgument, "reject later layout");

    MtkAwdl::FrameTemplate frame;
    uint8_t byte = 0xab;
    expect(frame.set(&byte, 1) == MtkAwdl::Ok, "template byte");
    uint8_t big[8] = {};
    expect(frame.set(big, MtkAwdl::FrameTemplate::capacity + 1) == MtkAwdl::BadArgument, "template cap");

    expect(MtkAwdl::setChannelRequest() == MtkAwdl::Unsupported, "no retune");
    expect(MtkAwdl::setCompanionInterface() == MtkAwdl::Unsupported, "no companion");
    expect(MtkAwdl::setOobRequest() == MtkAwdl::Blocked, "oob blocked");
    expect(!strcmp(MtkAwdl::readiness(false, true, true, true, true, true), "disabled"), "off wins");
    expect(!strcmp(MtkAwdl::readiness(true, true, true, true, true, true),
        MtkAwdl::kReadinessControl), "control readiness");

    const uint8_t addr[6] = {2, 0, 0, 0, 0, 1};
    MtkAwdl::RegistrationImage image;
    expect(MtkAwdl::fillRegistrationImage(&image, addr, 4, "awdl") == MtkAwdl::Ok, "flags 4 fill");
    expect(MtkAwdl::get32(&image, 0x00) == 1, "version");
    expect(MtkAwdl::get32(&image, 0x04) == 0x130, "size");
    expect(MtkAwdl::get32(&image, 0x0c) == 3, "field 0x0c");
    expect(MtkAwdl::get32(&image, 0x14) == 2, "field 0x14");
    expect(MtkAwdl::get32(&image, 0x24) == 0x80 && MtkAwdl::get32(&image, 0x28) == 0x80, "0x80 pair");
    expect(MtkAwdl::get32(&image, 0x40) == 4, "flags 4 stored");
    expect(MtkAwdl::get32(&image, 0x48) == 1428 && MtkAwdl::get32(&image, 0x4c) == 1428, "1428 pair");
    expect(memcmp(image.bytes + 0x108, addr, 6) == 0, "address");
    const char *stored = nullptr;
    memcpy(&stored, image.bytes + 0x30, sizeof(stored));
    expect(stored != nullptr && strcmp(stored, "awdl") == 0, "awdl prefix");
    expect(MtkAwdl::fillRegistrationImage(&image, addr, 6, "awdl") == MtkAwdl::Ok, "flags 6 fill");
    expect(MtkAwdl::get32(&image, 0x40) == 6, "flags 6 stored");
    expect(MtkAwdl::fillRegistrationImage(&image, addr, 5, "awdl") == MtkAwdl::BadArgument, "flags 5 rejected");
    expect(MtkAwdl::fillRegistrationImage(&image, addr, 4, "en") == MtkAwdl::BadArgument, "other prefix rejected");
    expect(MtkAwdl::flagsForWakeResetProperty(false) == 4, "wake property absent");
    expect(MtkAwdl::flagsForWakeResetProperty(true) == 6, "wake property true");
    expect(MtkAwdl::registrationCallPermitted(), "generic four-queue topology");
    expect(MtkAwdl::genericQueueCountAllowed(6), "six also in range");
    expect(!MtkAwdl::genericQueueCountAllowed(0), "zero queues rejected");
    expect(MtkAwdl::checkManagementLength(72) == MtkAwdl::Ok, "72 metadata");
    expect(MtkAwdl::checkManagementLength(73) == MtkAwdl::Blocked, "73 traps");
    const MtkAwdl::ChannelWords ch6 = MtkAwdl::legacy20ChannelWords(6);
    const MtkAwdl::ChannelWords ch149 = MtkAwdl::legacy20ChannelWords(149);
    const MtkAwdl::ChannelWords ch52 = MtkAwdl::legacy20ChannelWords(52);
    expect(ch6.version == 1 && ch6.number == 6 && ch6.flags == 0x0a, "channel 6");
    expect(ch149.version == 1 && ch149.number == 149 && ch149.flags == 0x12, "channel 149");
    expect(ch52.version == 1 && ch52.number == 52 && ch52.flags == 0x112, "channel 52");

    uint8_t meta[MtkAwdl::kManagementMetadataBytes];
    const uint8_t peer[6] = {0x02, 1, 2, 3, 4, 5};
    MtkAwdl::ChannelWords channel = {1, 36, 0};
    const int32_t dbm = MtkAwdl::mt76RcpiToSignedDbm(180);
    expect(dbm == -20, "rcpi 180 is -20 dBm");
    expect(MtkAwdl::writeManagementMetadata(meta, peer, &channel, dbm) == MtkAwdl::Ok, "metadata");
    expect(memcmp(meta, peer, 6) == 0, "peer prefix");
    expect(memcmp(meta + 0x0c, &channel, sizeof(channel)) == 0, "channel at 0x0c");
    expect(MtkAwdl::managementRssi(meta) == -20, "signed rssi at 0x2c");
    expect(MtkAwdl::kManagementSelector == 12, "selector 12");
    expect(MtkAwdl::kManagementMetadataBytes > MtkAwdl::kManagementLengthRejectedAtOrBelow, "72 passes length check");

    MtkAwdl::BufferLease early;
    expect(MtkAwdl::acquireLease(&early) == MtkAwdl::Ok, "acquire");
    expect(MtkAwdl::releaseBeforeSubmit(&early) == MtkAwdl::Ok, "return once");
    expect(early.returnCount == 1, "one return");
    expect(MtkAwdl::releaseBeforeSubmit(&early) == MtkAwdl::BadArgument, "no second return");
    expect(early.returnCount == 1, "count stays 1");
    MtkAwdl::BufferLease dropped;
    expect(MtkAwdl::acquireLease(&dropped) == MtkAwdl::Ok, "acquire drop path");
    expect(MtkAwdl::managerReturnedOnDrop(&dropped) == MtkAwdl::Ok, "manager returns buffer");
    expect(MtkAwdl::callerReturnAfterSubmit(&dropped) == MtkAwdl::BadArgument, "caller must not return");
    expect(dropped.returnCount == 1, "no double free");
    expect(MtkAwdl::liveManagementSubmit() == MtkAwdl::Ok, "submit transfers the buffer");
    expect(MtkAwdl::mt76RcpiToSignedDbm(0) == -110, "rcpi 0");
    expect(MtkAwdl::mt76RcpiToSignedDbm(220) == 0, "rcpi 220");
    expect(MtkAwdl::mt76RcpiToSignedDbm(255) == 17, "rcpi 255");

    uint8_t dataFrame[80];
    memset(dataFrame, 0, sizeof(dataFrame));
    dataFrame[0] = 0x08;
    const uint8_t dest[6] = {0x02, 9, 9, 9, 9, 9};
    const uint8_t src[6] = {0x02, 8, 8, 8, 8, 8};
    memcpy(dataFrame + 4, dest, 6);
    memcpy(dataFrame + 10, src, 6);
    memcpy(dataFrame + 16, MtkAwdl::kAwdlBssid, 6);
    dataFrame[22] = 0x10;
    const uint8_t llc[16] = {0xaa, 0xaa, 3, 0, 0x17, 0xf2, 8, 0, 0x03, 0x04, 0, 0, 0, 0, 0x86, 0xdd};
    memcpy(dataFrame + 24, llc, 16);
    dataFrame[40] = 0x60;
    const size_t frameLength = 24 + 16 + 40;
    MtkAwdl::EthernetBridge bridge;
    bridge.registered = true;
    expect(bridge.offer(dataFrame, frameLength, dest) == MtkAwdl::RxFate::Delivered, "first data");
    expect(bridge.inQueue == 1 && bridge.poolFree == MtkAwdl::kSoftwareRxPoolSlots - 1, "consumed");
    expect(bridge.offer(dataFrame, frameLength, dest) == MtkAwdl::RxFate::Duplicate, "duplicate");
    expect(bridge.inQueue == 1, "duplicate keeps the queue");
    bridge.poolFree = 0;
    dataFrame[22] = 0x20;
    expect(bridge.offer(dataFrame, frameLength, dest) == MtkAwdl::RxFate::PoolExhausted, "pool limit");
    bridge.beginShutdown();
    expect(bridge.offer(dataFrame, frameLength, dest) == MtkAwdl::RxFate::Draining, "shutdown");
    expect(bridge.drainDataPackets() == 1, "success is not freed again");

    expect(MtkAwdl::rawTxAdmit(800) == MtkAwdl::Ok, "800 raw bytes");
    expect(MtkAwdl::rawTxAdmit(801) == MtkAwdl::Blocked, "801 blocked");
    expect(MtkAwdl::rawTxAdmit(1428) == MtkAwdl::Blocked, "1428 is not a raw TX grant");
    expect(MtkAwdl::rawTxAdmit(0) == MtkAwdl::BadArgument, "empty frame");
    expect(MtkAwdl::skywalkTxAcceptCount() == 0, "no skywalk accept");
    expect(!MtkAwdl::kTransmitRadioSubmitPermitted && !MtkAwdl::kTransportReady, "radio tx off");

    const uint8_t own[6] = {0x02, 1, 2, 3, 4, 5};
    uint8_t eth[54];
    memset(eth, 0, sizeof(eth));
    eth[5] = 9;
    memcpy(eth + 6, own, 6);
    eth[12] = 0x86;
    eth[13] = 0xdd;
    eth[14] = 0x60;
    MtkAwdl::TransmitAdapter tx;
    uint8_t zero[6] = {};
    tx.setLocal(zero);
    uint32_t gen = 0;
    expect(tx.offer(1, eth, sizeof(eth), &gen) == MtkAwdl::TxFate::Malformed, "zero local mac");
    tx.setLocal(own);
    expect(tx.offer(1, eth, sizeof(eth), &gen) == MtkAwdl::TxFate::Accepted && gen != 0, "tx accept");
    expect(tx.held == 1 && tx.slots[0].airLength > 0 && !tx.slots[0].posted, "held not posted");
    MtkAwdl::DataView view = {};
    expect(MtkAwdl::parseAwdlData(tx.slots[0].air, tx.slots[0].airLength, &view) == MtkAwdl::Ok, "tx parses");
    expect(view.sequence == 1 && view.ipv6Length == 40, "tx sequence");
    expect(tx.retire(1, gen, false) == MtkAwdl::TxFate::StillOwned && tx.held == 1, "completion error keeps it");
    expect(tx.retire(1, gen, true) == MtkAwdl::TxFate::Completed && tx.held == 0, "completion success once");
    expect(tx.retire(1, gen, true) == MtkAwdl::TxFate::CompleteAgain, "no retry after success");
    uint32_t gen2 = 0;
    expect(tx.offer(1, eth, sizeof(eth), &gen2) == MtkAwdl::TxFate::Accepted && gen2 != gen, "token reuse new generation");
    expect(tx.retire(1, gen, true) == MtkAwdl::TxFate::CompleteAgain && tx.held == 1, "stale generation");
    expect(tx.retire(1, gen2, true) == MtkAwdl::TxFate::Completed, "current generation");
    uint8_t bulky[800];
    memset(bulky, 0, sizeof(bulky));
    bulky[5] = 1;
    memcpy(bulky + 6, own, 6);
    bulky[12] = 0x86;
    bulky[13] = 0xdd;
    bulky[14] = 0x60;
    bulky[18] = (uint8_t)((800 - 14 - 40) >> 8);
    bulky[19] = (uint8_t)(800 - 14 - 40);
    expect(tx.offer(4, bulky, sizeof(bulky), &gen) == MtkAwdl::TxFate::Oversize && tx.held == 0, "air over 800");
    MtkAwdl::TransmitAdapter::Request batch[3];
    uint32_t gens[3] = {};
    MtkAwdl::TxFate why = MtkAwdl::TxFate::Rejected;
    batch[0] = {10, eth, sizeof(eth)};
    batch[1] = {11, eth, 10};
    batch[2] = {12, eth, sizeof(eth)};
    expect(tx.acceptPrefix(batch, 3, gens, &why) == 1 && why == MtkAwdl::TxFate::Malformed, "prefix stops");
    expect(tx.held == 1, "tail not owned");
    expect(tx.markPosted(10, gens[0]) == MtkAwdl::TxFate::RadioBlocked && !tx.slots[0].posted, "post is not submit");
    expect(tx.retire(10, gens[0], true) == MtkAwdl::TxFate::Completed, "flush prefix");
    for (unsigned n = 0; n < MtkAwdl::kTransmitSlots; ++n)
        expect(tx.offer(20 + n, eth, sizeof(eth), &gen) == MtkAwdl::TxFate::Accepted, "fill");
    expect(tx.offer(99, eth, sizeof(eth), &gen) == MtkAwdl::TxFate::Backpressure && tx.held == MtkAwdl::kTransmitSlots, "full");
    expect(tx.radioSubmit() == MtkAwdl::TxFate::RadioBlocked, "no ring submit");
    const unsigned drained = tx.teardown();
    expect(drained == MtkAwdl::kTransmitSlots && tx.held == 0, "teardown once");
    expect(tx.teardown() == 0, "teardown again");
    expect(tx.offer(7, eth, sizeof(eth), &gen) == MtkAwdl::TxFate::Draining, "tx after stop");
    expect(tx.acceptOne(8, eth, sizeof(eth), &gen) == MtkAwdl::TxFate::Draining, "direct accept after stop");

    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("awdl native policy: ok\n");
    return 0;
}
