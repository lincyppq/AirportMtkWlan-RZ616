/*
 * TransmitAdapter.hpp — callable AWDL data TX adapter. Not a live radio path.
 *
 * Ethernet-to-AWDL layout adapted from ForbiddenItlwm 69e88fa
 * include/AWDLPeerProbe.hpp and include/AWDLDataPayload.hpp (GPLv2).
 * The 802.11 result is capped at kRawTxMaxFrame (800). That cap is the
 * existing raw inject limit, not a native MTU grant.
 *
 * Accepting a token means this adapter owns it until complete() or
 * teardown() once. radioSubmit() stays blocked: the shared station TX
 * ring has no proven serialized handoff, so nothing here calls it.
 * kTransportReady stays false.
 */
#pragma once
#include <MtkAwdl/SoftwareRx.hpp>
#include <stdint.h>
#include <string.h>

namespace MtkAwdl {

inline constexpr bool kTransmitRadioSubmitPermitted = false;
inline constexpr unsigned kTransmitSlots = 4;

enum class TxFate : int {
    Rejected = 0,
    Malformed = 1,
    Oversize = 2,
    Backpressure = 3,
    Accepted = 4,
    Completed = 5,
    CompleteAgain = 6,
    Draining = 7,
    RadioBlocked = 8
};

inline void write16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

/* Caller owns both buffers. They must not overlap. On any result other
 * than Ok, *written is unchanged. */
inline Result buildAwdlDataFromEthernet(const uint8_t *eth, size_t length, const uint8_t *own,
    uint16_t sequence, uint8_t *output, size_t available, size_t *written)
{
    if (eth == nullptr || own == nullptr || output == nullptr || written == nullptr)
        return BadArgument;
    if (length < 54 || eth[12] != 0x86 || eth[13] != 0xdd)
        return BadArgument;
    if ((own[0] & 1) != 0 || memcmp(eth + 6, own, 6) != 0)
        return BadArgument;
    bool anyDest = false;
    for (unsigned i = 0; i < 6; ++i)
        anyDest = anyDest || eth[i] != 0;
    if (!anyDest || !ipv6PayloadOk(eth + 14, length - 14))
        return BadArgument;
    const size_t air = 24 + 16 + (length - 14);
    if (air > kRawTxMaxFrame)
        return Blocked;
    if (available < air)
        return NotReady;
    memset(output, 0, 24);
    output[0] = 0x08;
    memcpy(output + 4, eth, 6);
    memcpy(output + 10, own, 6);
    memcpy(output + 16, kAwdlBssid, 6);
    write16(output + 22, (uint16_t)((sequence & 4095u) << 4));
    const uint8_t prefix[16] = {0xaa, 0xaa, 3, 0, 0x17, 0xf2, 8, 0, 3, 4, 0, 0, 0, 0, 0x86, 0xdd};
    memcpy(output + 24, prefix, 16);
    write16(output + 34, sequence);
    memcpy(output + 40, eth + 14, length - 14);
    *written = air;
    return Ok;
}

struct TransmitAdapter {
    struct Slot {
        uint64_t token = 0;
        uint16_t airLength = 0;
        uint16_t sequence = 0;
        bool held = false;
        uint8_t air[kRawTxMaxFrame] = {};
    };

    uint8_t local[6] = {};
    Slot slots[kTransmitSlots] = {};
    unsigned held = 0;
    unsigned completed = 0;
    uint16_t sequence = 1;
    bool draining = false;

    void setLocal(const uint8_t mac[6])
    {
        if (mac != nullptr)
            memcpy(local, mac, 6);
    }

    TxFate offer(uint64_t token, const uint8_t *eth, size_t length)
    {
        if (draining)
            return TxFate::Draining;
        if (token == 0 || eth == nullptr)
            return TxFate::Rejected;
        for (const auto &slot : slots) {
            if (slot.held && slot.token == token)
                return TxFate::Rejected;
        }
        if (held >= kTransmitSlots)
            return TxFate::Backpressure;
        Slot scratch;
        size_t airLength = 0;
        const Result built = buildAwdlDataFromEthernet(eth, length, local, sequence,
            scratch.air, sizeof(scratch.air), &airLength);
        if (built == BadArgument)
            return TxFate::Malformed;
        if (built == Blocked)
            return TxFate::Oversize;
        if (built != Ok)
            return TxFate::Rejected;
        for (auto &slot : slots) {
            if (slot.held)
                continue;
            memcpy(slot.air, scratch.air, airLength);
            slot.token = token;
            slot.airLength = (uint16_t)airLength;
            slot.sequence = sequence;
            slot.held = true;
            ++held;
            sequence = (uint16_t)((sequence + 1) & 4095u);
            if (sequence == 0)
                sequence = 1;
            return TxFate::Accepted;
        }
        return TxFate::Backpressure;
    }

    TxFate complete(uint64_t token)
    {
        for (auto &slot : slots) {
            if (!slot.held || slot.token != token)
                continue;
            slot.held = false;
            slot.token = 0;
            --held;
            ++completed;
            return TxFate::Completed;
        }
        return TxFate::CompleteAgain;
    }

    unsigned teardown()
    {
        unsigned n = 0;
        draining = true;
        for (auto &slot : slots) {
            if (!slot.held)
                continue;
            slot.held = false;
            slot.token = 0;
            ++n;
            ++completed;
        }
        held = 0;
        return n;
    }

    TxFate radioSubmit() const
    {
        return kTransmitRadioSubmitPermitted ? TxFate::Accepted : TxFate::RadioBlocked;
    }
};

} /* namespace MtkAwdl */
