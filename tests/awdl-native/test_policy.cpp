/* Host checks for the on-channel AWDL policy. No kernel and no hardware. */
#include <MtkAwdl/NativePolicy.hpp>
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
    expect(!MtkAwdl::kRegistrationPermitted, "registration closed");
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

    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("awdl native policy: ok\n");
    return 0;
}
