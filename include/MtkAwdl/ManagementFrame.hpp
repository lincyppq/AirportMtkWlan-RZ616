/*
 * ManagementFrame.hpp — 25G241 management metadata and buffer ownership.
 *
 * Measured on the 25G241 kernel collection:
 *   handleActionFrame_rx builds 72 bytes and calls bpfTapInput with selector 12.
 *   actionFrameProcessInput reads a signed 32-bit signal at metadata+0x2c.
 *   ChanSpecConvToApple80211Channel's out pointer is metadata+0x0c.
 *     The 12-byte apple80211_channel in this tree is the type of that out
 *     pointer. Flag bits inside it were not recovered from the disassembly,
 *     so callers pass the struct they already have; this file does not
 *     invent band flags.
 *   isValidAF rejects a metadata length of 0x47 or less.
 *   Every normal actionFrameInput return consumes the buffer. A queued
 *   record stores the original buffer and copies exactly 72 metadata bytes.
 *   The worker and disable drain call returnBuffer. Length >= 73 traps.
 *
 * RSSI uses the driver's existing MT76 formula ((rcpi - 220) / 2), widened
 * to int32. That is not a new calibration against the Broadcom peer manager.
 *
 * Adapted in part from ForbiddenItlwm 69e88fa include/AWDLManagementFrame.hpp
 * (72-byte block, selector 12, return-before-submit). GPLv2.
 */
#pragma once
#include <MtkAwdl/NativePolicy.hpp>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace MtkAwdl {

inline constexpr size_t kManagementMetadataBytes = 72;
inline constexpr unsigned kManagementSelector = 12;
inline constexpr size_t kManagementChannelOffset = 0x0c;
inline constexpr size_t kManagementRssiOffset = 0x2c;
inline constexpr size_t kManagementLengthRejectedAtOrBelow = 0x47;

struct ChannelWords {
    uint32_t version;
    uint32_t number;
    uint32_t flags;
};
static_assert(sizeof(ChannelWords) == 12, "apple80211_channel width");
static_assert(kManagementChannelOffset + sizeof(ChannelWords) <= kManagementRssiOffset,
    "channel and rssi overlap");

inline int32_t mt76RcpiToSignedDbm(uint32_t rcpi)
{
    const int byte = (int)(rcpi & 0xffu);
    return (int32_t)((byte - 220) / 2);
}

inline Result checkManagementLength(size_t length)
{
    if (length == 0)
        return BadArgument;
    if (length >= 73)
        return Blocked;
    if (length != kManagementMetadataBytes)
        return BadArgument;
    return Ok;
}

inline ChannelWords legacy20ChannelWords(unsigned channel)
{
    ChannelWords out = {1, channel, 0};
    if (channel == 0 || channel > 196)
        return out;
    /* fillLegacy20MHzChanSpec: 0x1000+ch below 15, else 0xd000+ch. */
    const uint16_t spec = (uint16_t)((channel < 15 ? 0x1000 : 0xd000) + channel);
    const unsigned band = spec >> 14;
    const unsigned width = (spec >> 11) & 7;
    uint32_t flags = 0;
    if (band == 0)
        flags = 0x08;
    else if (band == 3) {
        flags = 0x10;
        if (channel >= 52 && channel <= 144)
            flags |= 0x100;
    }
    if (width == 3)
        flags |= 0x04;
    else if (width == 4)
        flags |= 0x400;
    else if (width == 5 || width == 6)
        flags |= 0x800;
    else
        flags |= 0x02;
    if (width == 3 && (spec & 0x700) == 0)
        flags |= 0x200;
    out.flags = flags;
    return out;
}

inline Result writeManagementMetadata(uint8_t out[kManagementMetadataBytes],
                                      const uint8_t peer[6],
                                      const ChannelWords *channel,
                                      int32_t rssiDbm)
{
    if (out == nullptr || peer == nullptr || channel == nullptr)
        return BadArgument;
    memset(out, 0, kManagementMetadataBytes);
    memcpy(out, peer, 6);
    memcpy(out + kManagementChannelOffset, channel, sizeof(*channel));
    memcpy(out + kManagementRssiOffset, &rssiDbm, sizeof(rssiDbm));
    return Ok;
}

inline int32_t managementRssi(const uint8_t meta[kManagementMetadataBytes])
{
    int32_t value = 0;
    memcpy(&value, meta + kManagementRssiOffset, sizeof(value));
    return value;
}

/* Caller-owned lease. returnCount is returnBuffer calls, not frees after submit. */
struct BufferLease {
    int held = 0;
    int returnCount = 0;
    bool submitted = false;
};

inline bool callerMayReturn(const BufferLease &lease)
{
    return lease.held == 1 && !lease.submitted;
}

inline Result acquireLease(BufferLease *lease)
{
    if (lease == nullptr || lease->held != 0 || lease->submitted)
        return BadArgument;
    lease->held = 1;
    return Ok;
}

/* Validation failed before bpfTapInput. Return the buffer exactly once. */
inline Result releaseBeforeSubmit(BufferLease *lease)
{
    if (lease == nullptr || !callerMayReturn(*lease))
        return BadArgument;
    lease->held = 0;
    lease->returnCount += 1;
    return Ok;
}

/*
 * Proven immediate/drop path: the peer manager calls returnBuffer, then
 * bpfTapInput returns without a second return. The caller must not free.
 */
inline Result managerReturnedOnDrop(BufferLease *lease)
{
    if (lease == nullptr || lease->held != 1 || lease->submitted)
        return BadArgument;
    lease->held = 0;
    lease->returnCount += 1;
    lease->submitted = true;
    return Ok;
}

/* Refuses a caller returnBuffer after submit. Does not change returnCount. */
inline Result callerReturnAfterSubmit(BufferLease *lease)
{
    if (lease == nullptr)
        return BadArgument;
    return BadArgument;
}

/* Caller may invoke bpfTapInput. Afterwards the caller must not returnBuffer. */
inline Result liveManagementSubmit()
{
    return kManagementSubmitPermitted ? Ok : Blocked;
}

inline Result rawTxAdmit(size_t length)
{
    if (length == 0)
        return BadArgument;
    if (length > kRawTxMaxFrame)
        return Blocked;
    return Ok;
}

/* Skywalk TX callback result: accept nothing, so the shared MTK ring is idle. */
inline unsigned skywalkTxAcceptCount()
{
    return 0;
}

} /* namespace MtkAwdl */
