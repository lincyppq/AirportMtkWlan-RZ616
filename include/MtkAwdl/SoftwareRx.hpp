/*
 * SoftwareRx.hpp — AWDL data frame checks and a software Ethernet bridge.
 *
 * Frame layout adapted from ForbiddenItlwm 69e88fa include/AWDLDataFrame.hpp
 * and include/AWDLDataPayload.hpp (GPLv2). Delivery follows that tree's
 * observeDataFrame: build an Ethernet header plus IPv6, give the packet to
 * the RX completion queue, and do not touch it after success.
 *
 * 25G241 enqueuePackets validates, then completeWithQueue + disposePacket.
 * A successful enqueue consumes the packet. Failure before that returns the
 * caller's allocation. This is not IO80211VirtualInterface::inputPacket and
 * it does not build a packet_info_tag.
 */
#pragma once
#include <MtkAwdl/NativePolicy.hpp>
#include <stdint.h>
#include <string.h>

namespace MtkAwdl {

inline constexpr unsigned kSoftwareRxPoolSlots = 128;
inline constexpr size_t kSoftwareRxBufferBytes = 2048;
inline constexpr uint8_t kAwdlBssid[6] = {0x00, 0x25, 0x00, 0xff, 0x94, 0x73};

enum class RxFate : int {
    Rejected = 0,
    Duplicate = 1,
    PoolExhausted = 2,
    Delivered = 3,
    DeliverFailed = 4,
    Draining = 5
};

struct DataView {
    const uint8_t *destination;
    const uint8_t *source;
    const uint8_t *ipv6;
    size_t ipv6Length;
    uint16_t sequence;
};

inline uint16_t read16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

inline bool ipv6PayloadOk(const uint8_t *p, size_t n)
{
    if (p == nullptr || n < 40 || n > 1500 || (p[0] >> 4) != 6)
        return false;
    const size_t payload = (size_t)((p[4] << 8) | p[5]);
    return payload == n - 40;
}

inline Result parseAwdlData(const uint8_t *frame, size_t length, DataView *out)
{
    if (frame == nullptr || out == nullptr || length < 24 || length > kSoftwareRxBufferBytes)
        return BadArgument;
    if (frame[0] != 0x08 && frame[0] != 0x88)
        return Unsupported;
    if (memcmp(frame + 16, kAwdlBssid, 6) != 0)
        return Unsupported;
    if ((frame[1] & 0xc7) != 0 || (frame[22] & 0x0f) != 0)
        return Unsupported;
    if ((frame[10] & 1) != 0)
        return BadArgument;
    const size_t header = frame[0] == 0x88 ? 26 : 24;
    if (length < header)
        return BadArgument;
    if (header == 26 && (frame[24] & 0x80) != 0)
        return Unsupported;
    const uint8_t *body = frame + header;
    size_t bodyLength = length - header;
    const uint8_t outer[6] = {0xaa, 0xaa, 3, 0, 0, 0};
    if (bodyLength >= 8 && memcmp(body, outer, 6) == 0) {
        const size_t marked = (size_t)((body[6] << 8) | body[7]);
        if (marked != bodyLength - 8)
            return Unsupported;
        body += 8;
        bodyLength -= 8;
    }
    const uint8_t llc[8] = {0xaa, 0xaa, 3, 0, 0x17, 0xf2, 8, 0};
    if (bodyLength < 16 || memcmp(body, llc, 8) != 0)
        return Unsupported;
    if (read16(body + 8) != 0x0403 || read16(body + 12) != 0 ||
        body[14] != 0x86 || body[15] != 0xdd)
        return Unsupported;
    if (!ipv6PayloadOk(body + 16, bodyLength - 16))
        return BadArgument;
    out->destination = frame + 4;
    out->source = frame + 10;
    out->ipv6 = body + 16;
    out->ipv6Length = bodyLength - 16;
    out->sequence = (uint16_t)(read16(frame + 22) >> 4);
    return Ok;
}

inline Result copyEthernetIPv6(const DataView &view, uint8_t *out, size_t capacity, size_t *written)
{
    if (out == nullptr || written == nullptr || view.ipv6 == nullptr)
        return BadArgument;
    if (capacity < 14 + view.ipv6Length)
        return NotReady;
    memcpy(out, view.destination, 6);
    memcpy(out + 6, view.source, 6);
    out[12] = 0x86;
    out[13] = 0xdd;
    memcpy(out + 14, view.ipv6, view.ipv6Length);
    *written = 14 + view.ipv6Length;
    return Ok;
}

struct DuplicateTable {
    struct Slot {
        uint8_t mac[6];
        uint16_t sequence;
        bool used;
    } slots[8] = {};

    bool seen(const uint8_t mac[6], uint16_t sequence) const
    {
        for (const auto &slot : slots) {
            if (slot.used && slot.sequence == sequence && memcmp(slot.mac, mac, 6) == 0)
                return true;
        }
        return false;
    }

    void remember(const uint8_t mac[6], uint16_t sequence)
    {
        for (auto &slot : slots) {
            if (slot.used && memcmp(slot.mac, mac, 6) == 0) {
                slot.sequence = sequence;
                return;
            }
        }
        for (auto &slot : slots) {
            if (!slot.used) {
                memcpy(slot.mac, mac, 6);
                slot.sequence = sequence;
                slot.used = true;
                return;
            }
        }
        memcpy(slots[0].mac, mac, 6);
        slots[0].sequence = sequence;
        slots[0].used = true;
    }
};

struct EthernetBridge {
    bool draining = false;
    bool registered = false;
    unsigned poolFree = kSoftwareRxPoolSlots;
    unsigned delivered = 0;
    unsigned failed = 0;
    unsigned inQueue = 0;
    DuplicateTable duplicates;

    RxFate offer(const uint8_t *frame, size_t length, const uint8_t local[6])
    {
        if (draining)
            return RxFate::Draining;
        if (!registered)
            return RxFate::Rejected;
        DataView view = {};
        if (parseAwdlData(frame, length, &view) != Ok)
            return RxFate::Rejected;
        const bool multicast = (view.destination[0] & 1) != 0;
        if ((!multicast && memcmp(view.destination, local, 6) != 0) ||
            memcmp(view.source, local, 6) == 0)
            return RxFate::Rejected;
        if (duplicates.seen(view.source, view.sequence))
            return RxFate::Duplicate;
        if (poolFree == 0)
            return RxFate::PoolExhausted;
        uint8_t ethernet[14 + 1500];
        size_t written = 0;
        if (copyEthernetIPv6(view, ethernet, sizeof(ethernet), &written) != Ok) {
            ++failed;
            return RxFate::DeliverFailed;
        }
        --poolFree;
        /* Success consumes the packet. The pool slot stays outstanding until
         * the queue disposes it; the caller must not free the packet. */
        ++inQueue;
        ++delivered;
        duplicates.remember(view.source, view.sequence);
        return RxFate::Delivered;
    }

    void enqueueFailedReturnSlot()
    {
        if (poolFree < kSoftwareRxPoolSlots)
            ++poolFree;
        ++failed;
    }

    void beginShutdown()
    {
        draining = true;
    }

    /* Data packets already disposed by a successful enqueue are not freed again. */
    unsigned drainDataPackets() const
    {
        return inQueue;
    }
};

} /* namespace MtkAwdl */
