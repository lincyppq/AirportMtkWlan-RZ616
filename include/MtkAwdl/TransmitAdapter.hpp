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
    StillOwned = 7,
    Draining = 8,
    RadioBlocked = 9
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
    bool ownSet = false;
    for (unsigned i = 0; i < 6; ++i)
        ownSet = ownSet || own[i] != 0;
    if (!ownSet)
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
        uint32_t generation = 0;
        uint16_t airLength = 0;
        uint16_t sequence = 0;
        bool held = false;
        bool posted = false;
        uint8_t air[kRawTxMaxFrame] = {};
    };

    struct Request {
        uint64_t token;
        const uint8_t *eth;
        size_t length;
    };

    uint8_t local[6] = {};
    Slot slots[kTransmitSlots] = {};
    unsigned held = 0;
    unsigned completed = 0;
    uint16_t sequence = 1;
    uint32_t nextGeneration = 1;
    bool draining = false;

    void setLocal(const uint8_t mac[6])
    {
        if (mac != nullptr)
            memcpy(local, mac, 6);
    }

    TxFate acceptOne(uint64_t token, const uint8_t *eth, size_t length, uint32_t *generation)
    {
        if (token == 0 || eth == nullptr || generation == nullptr)
            return TxFate::Rejected;
        for (const auto &slot : slots) {
            if (slot.held && slot.token == token)
                return TxFate::Rejected;
        }
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
            slot.generation = nextGeneration++;
            if (nextGeneration == 0)
                nextGeneration = 1;
            slot.airLength = (uint16_t)airLength;
            slot.sequence = sequence;
            slot.posted = false;
            slot.held = true;
            *generation = slot.generation;
            ++held;
            sequence = (uint16_t)((sequence + 1) & 4095u);
            if (sequence == 0)
                sequence = 1;
            return TxFate::Accepted;
        }
        return TxFate::Backpressure;
    }

    /* Returns how many leading requests were accepted. *stopped says why
     * the next one was not. Accepted packets are owned here; the tail is not. */
    unsigned acceptPrefix(const Request *req, unsigned count, uint32_t *generations, TxFate *stopped)
    {
        unsigned took = 0;
        if (stopped != nullptr)
            *stopped = TxFate::Accepted;
        if (req == nullptr || generations == nullptr) {
            if (stopped != nullptr)
                *stopped = TxFate::Rejected;
            return 0;
        }
        for (unsigned i = 0; i < count; ++i) {
            TxFate one = TxFate::Rejected;
            uint32_t generation = 0;
            if (draining)
                one = TxFate::Draining;
            else if (held >= kTransmitSlots)
                one = TxFate::Backpressure;
            else
                one = acceptOne(req[i].token, req[i].eth, req[i].length, &generation);
            if (one != TxFate::Accepted) {
                if (stopped != nullptr)
                    *stopped = one;
                return took;
            }
            generations[i] = generation;
            ++took;
        }
        return took;
    }

    TxFate offer(uint64_t token, const uint8_t *eth, size_t length, uint32_t *generation)
    {
        const Request req = {token, eth, length};
        uint32_t gen = 0;
        TxFate stopped = TxFate::Rejected;
        const unsigned n = acceptPrefix(&req, 1, &gen, &stopped);
        if (generation != nullptr)
            *generation = n == 1 ? gen : 0;
        return n == 1 ? TxFate::Accepted : stopped;
    }

    /* enqueueOk is the array-completion result. False leaves the token owned.
     * True retires it. The packet storage must not be touched after true. */
    TxFate retire(uint64_t token, uint32_t generation, bool enqueueOk)
    {
        for (auto &slot : slots) {
            if (!slot.held || slot.token != token || slot.generation != generation)
                continue;
            if (!enqueueOk)
                return TxFate::StillOwned;
            slot.held = false;
            slot.posted = false;
            slot.token = 0;
            --held;
            ++completed;
            return TxFate::Completed;
        }
        return TxFate::CompleteAgain;
    }

    TxFate markPosted(uint64_t token, uint32_t generation)
    {
        if (!kTransmitRadioSubmitPermitted)
            return TxFate::RadioBlocked;
        for (auto &slot : slots) {
            if (slot.held && slot.token == token && slot.generation == generation) {
                slot.posted = true;
                return TxFate::Accepted;
            }
        }
        return TxFate::Rejected;
    }

    unsigned teardown()
    {
        unsigned n = 0;
        draining = true;
        for (auto &slot : slots) {
            if (!slot.held)
                continue;
            slot.held = false;
            slot.posted = false;
            slot.token = 0;
            slot.generation = 0;
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
