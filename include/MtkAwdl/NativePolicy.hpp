/*
 * NativePolicy.hpp — host-testable on-channel AWDL control policy.
 *
 * Adapted from ForbiddenItlwm (GPLv2) commit
 * 69e88fa0fdb0ef7122f2aee81111651c32260a62 (tag v0.5.4-beta.2):
 *   include/AWDLBuildGuard.hpp
 *   ForbiddenItlwm/AWDLInactiveProtocol.hpp
 *   ForbiddenItlwm/AWDLRequestedValue.hpp
 * Apple-side control state only. Nothing here retunes a radio, opens a
 * second firmware interface, or submits a frame.
 *
 * GPLv2, same terms as the rest of AirportMtkWlan.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace MtkAwdl {

enum Result : int {
    Ok = 0,
    BadArgument = 1,
    Unsupported = 2,
    NotReady = 3,
    Blocked = 4
};

/* In-memory gate. Never persisted, never implied by the capability bit. */
inline constexpr bool kNativeGateDefault = false;

/*
 * Registration flags: IOPMResetPowerStateOnWake true -> 6, otherwise 4.
 * Management submit transfers the IO80211Buffer; the caller does not return it.
 * Software Ethernet RX uses the RX completion queue and does not build a
 * packet_info_tag. The native inputPacket tag path stays unused.
 */
inline constexpr bool kRegistrationPermitted = true;
inline constexpr bool kManagementSubmitPermitted = true;
inline constexpr bool kRxSubmitPermitted = false;
inline constexpr bool kIpv6HandoffPermitted = false;
inline constexpr bool kSecondVifPermitted = false;
inline constexpr bool kOffchannelPermitted = false;
inline constexpr bool kTransportReady = false;
inline constexpr unsigned kRawTxMaxFrame = 800;
inline constexpr unsigned kControllerCreateSlot = 0xdd8;
inline constexpr unsigned kControllerAttachSlot = 0xde0;
inline constexpr unsigned kAwdlVtableSlots = 568;
inline constexpr char kReferenceCommit[] = "69e88fa0fdb0ef7122f2aee81111651c32260a62";
inline constexpr char kVerifiedKernel[] = "xnu-12377.161.15.700.19~2/RELEASE_X86_64";
inline constexpr char kVerifiedOsBuild[] = "25G241";

inline constexpr char kBlockerRegistration[] = "registration-rejected";
inline constexpr char kBlockerRx[] = "management-pool-not-ready";
inline constexpr char kBlockerIpv6[] = "native-inputpacket-tag-unused";
inline constexpr char kBlockerTransport[] = "native-transport-not-implemented";
inline constexpr char kBlockerBuild[] = "unverified-build";
inline constexpr char kBlockerVtable[] = "awdl-vtable-mismatch";
inline constexpr char kBlockerSlot[] = "controller-slot-mismatch";
inline constexpr char kBlockerOffchannel[] = "on-channel-only-no-retune";
inline constexpr char kBlockerSecondVif[] = "second-vif-not-implemented";
inline constexpr char kBlockerOob[] = "master-indication-layout-unverified";
inline constexpr char kReadinessDisabled[] = "disabled";
inline constexpr char kReadinessControl[] = "control-object-registration-blocked";
inline constexpr char kBlockerRawTx[] = "raw-tx-max-800-not-native-mtu";

enum class BuildCheck { Verified, UnknownKernel, UnknownBuild };

inline bool containsText(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !*needle)
        return false;
    const size_t n = strlen(needle);
    for (const char *p = haystack; *p; ++p)
        if (!strncmp(p, needle, n))
            return true;
    return false;
}

/*
 * 25G241 only. 25G332 and 25G342 are in ForbiddenItlwm's list from Kernel
 * Debug Kit comparisons this tree has not repeated, so they stay blocked.
 * An empty osBuild is accepted when the kernel tag matches (osversion can
 * still be empty early). A non-empty osBuild must be exactly 25G241.
 */
inline BuildCheck checkBuild(const char *kernelVersion, const char *osBuild)
{
    if (!containsText(kernelVersion, kVerifiedKernel))
        return BuildCheck::UnknownKernel;
    if (!osBuild || !*osBuild)
        return BuildCheck::Verified;
    if (!strcmp(osBuild, kVerifiedOsBuild))
        return BuildCheck::Verified;
    return BuildCheck::UnknownBuild;
}

inline const char *buildCheckName(BuildCheck c)
{
    switch (c) {
    case BuildCheck::Verified: return "verified";
    case BuildCheck::UnknownKernel: return "unknown-kernel";
    case BuildCheck::UnknownBuild: return "unknown-build";
    }
    return "unknown";
}

struct SyncEnabled {
    uint32_t version;
    uint32_t unk1;
    uint32_t enabled;
};

struct SyncState {
    uint32_t version;
    uint32_t state;
};

/* Records what the peer manager asked. accept==true does not start a schedule. */
class NativeState {
public:
    void setAccept(bool value) { accept = value; }
    bool accepting() const { return accept; }

    Result setEnabled(const SyncEnabled *data)
    {
        if (!data || data->version > 1 || data->enabled > 1)
            return BadArgument;
        if (data->enabled && !accept)
            return Unsupported;
        enabled = data->enabled != 0;
        return Ok;
    }

    Result getEnabled(SyncEnabled *data) const
    {
        if (!data)
            return BadArgument;
        *data = {};
        data->version = 1;
        data->enabled = enabled ? 1u : 0u;
        return Ok;
    }

    Result setState(const SyncState *data)
    {
        if (!data || data->version > 1 || data->state < 1 || data->state > 3)
            return BadArgument;
        /* State 1 is DISABLE and can be acknowledged with no radio.
         * Other states are recorded only when accept is on. Recording does
         * not arm a scheduler, change channel, or start transmission. */
        if (data->state != 1 && !accept)
            return Unsupported;
        state = data->state;
        return Ok;
    }

    Result getState(SyncState *data) const
    {
        if (!data)
            return BadArgument;
        *data = {};
        data->version = 1;
        data->state = state;
        return Ok;
    }

private:
    bool accept = false;
    bool enabled = false;
    uint32_t state = 1;
};

template <typename T>
class RequestedValue {
public:
    Result set(const T *request)
    {
        if (!request || request->version > 1)
            return BadArgument;
        value = *request;
        value.version = 1;
        valid = true;
        return Ok;
    }
    Result get(T *reply) const
    {
        if (!reply)
            return BadArgument;
        if (!valid)
            return NotReady;
        *reply = value;
        return Ok;
    }
    bool isValid() const { return valid; }
private:
    T value{};
    bool valid = false;
};

/* Bounded copy of a kernel-owned template. Not a transmit buffer. */
class FrameTemplate {
public:
    static constexpr size_t capacity = 0x724;
    Result set(const void *payload, size_t length)
    {
        if (length > capacity || (length && !payload))
            return BadArgument;
        const auto *source = static_cast<const uint8_t *>(payload);
        for (size_t i = 0; i < length; ++i)
            bytes[i] = source[i];
        for (size_t i = length; i < used; ++i)
            bytes[i] = 0;
        used = length;
        return Ok;
    }
    size_t size() const { return used; }
    Result copy(void *destination, size_t available) const
    {
        if (available < used || (used && !destination))
            return NotReady;
        auto *target = static_cast<uint8_t *>(destination);
        for (size_t i = 0; i < used; ++i)
            target[i] = bytes[i];
        return Ok;
    }
private:
    uint8_t bytes[capacity]{};
    size_t used = 0;
};

/* Channel changes, a second interface, and OOB/master-indication parsing
 * are refused here so a caller cannot reach them by forgetting a check. */
inline Result setChannelRequest() { return Unsupported; }
inline Result setCompanionInterface() { return Unsupported; }
inline Result setOobRequest() { return Blocked; }

inline const char *readiness(bool gate, bool buildOk, bool vtableOk,
                              bool slotOk, bool object, bool queues)
{
    if (!gate)
        return kReadinessDisabled;
    if (!buildOk)
        return kBlockerBuild;
    if (!vtableOk)
        return kBlockerVtable;
    if (!slotOk)
        return kBlockerSlot;
    if (object && queues)
        return kReadinessControl;
    if (object)
        return "control-object-no-queues";
    return "blocked";
}

} /* namespace MtkAwdl */
