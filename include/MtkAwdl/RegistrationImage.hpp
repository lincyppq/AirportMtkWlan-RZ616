/*
 * RegistrationImage.hpp — 25G241 RegistrationInfo bytes, not a register call.
 *
 * Numeric overrides are from the 25G241 kernel collection:
 *   IOSkywalkEthernetInterface::initRegistrationInfo writes the version/size
 *   qword at offset 0.
 *   AppleBCMWLANCore::createProxInterface then stores the fields below.
 *   Flags at +0x40 are 6 when IOPMResetPowerStateOnWake is true, and 4 when
 *   that property is absent or not true. A custom interface that does not
 *   publish the property uses 4.
 *   Broadcom initializes N=4 access categories and registers N+2 queues.
 *   The generic validator accepts 1..256 typed queues. This image does not
 *   encode that count. The software bridge uses four reference factories,
 *   not the Broadcom bus factory.
 *
 * GPLv2. No radio and no second interface.
 */
#pragma once
#include <MtkAwdl/NativePolicy.hpp>
#include <stdint.h>
#include <string.h>

namespace MtkAwdl {

inline constexpr size_t kRegistrationBytes = 0x130;
inline constexpr uint32_t kRegistrationVersion = 1;
inline constexpr uint32_t kRegistrationFlagsObserved4 = 4;
inline constexpr uint32_t kRegistrationFlagsObserved6 = 6;
inline constexpr uint32_t kRegistrationSizeField = 1428;
inline constexpr char kRegistrationPrefix[] = "awdl";

struct RegistrationImage {
    uint8_t bytes[kRegistrationBytes];
};

inline void put32(RegistrationImage *image, size_t offset, uint32_t value)
{
    memcpy(image->bytes + offset, &value, sizeof(value));
}

inline uint32_t get32(const RegistrationImage *image, size_t offset)
{
    uint32_t value = 0;
    memcpy(&value, image->bytes + offset, sizeof(value));
    return value;
}

/*
 * Writes the measured constants. flags must be one of the two observed
 * values; this function does not choose between them. prefix must be the
 * stable "awdl" string the native producer points at. Result::Ok means the
 * bytes match the measured constants. A non-zero registerEthernetInterface
 * result is still failure and unwinds the queues.
 */
inline Result fillRegistrationImage(RegistrationImage *image, const uint8_t address[6],
                                    uint32_t flags, const char *prefix)
{
    if (image == nullptr || address == nullptr || prefix == nullptr)
        return BadArgument;
    if (flags != kRegistrationFlagsObserved4 && flags != kRegistrationFlagsObserved6)
        return BadArgument;
    if (strcmp(prefix, kRegistrationPrefix) != 0)
        return BadArgument;
    memset(image, 0, sizeof(*image));
    /* initRegistrationInfo's qword: version in the low half, 0x130 in the high. */
    const uint64_t versionAndSize = (uint64_t)kRegistrationBytes << 32 | kRegistrationVersion;
    memcpy(image->bytes, &versionAndSize, sizeof(versionAndSize));
    put32(image, 0x0c, 3);
    put32(image, 0x14, 2);
    put32(image, 0x24, 0x80);
    put32(image, 0x28, 0x80);
    const char *stable = kRegistrationPrefix;
    memcpy(image->bytes + 0x30, &stable, sizeof(stable));
    put32(image, 0x38, 0);
    const uint16_t headroom = 0x40;
    memcpy(image->bytes + 0x3c, &headroom, sizeof(headroom));
    put32(image, 0x40, flags);
    put32(image, 0x48, kRegistrationSizeField);
    put32(image, 0x4c, kRegistrationSizeField);
    memcpy(image->bytes + 0x108, address, 6);
    return Ok;
}

/* Generic IOSkywalkQueueSet accepts 1..256 non-null typed queues. */
inline bool genericQueueCountAllowed(unsigned count)
{
    return count >= 1 && count <= 256;
}

inline constexpr unsigned kSoftwareEthernetQueueCount = 4;

/* False/absent IOPMResetPowerStateOnWake -> 4. True -> 6. */
inline uint32_t flagsForWakeResetProperty(bool propertyTrue)
{
    return propertyTrue ? kRegistrationFlagsObserved6 : kRegistrationFlagsObserved4;
}

inline bool registrationCallPermitted()
{
    return genericQueueCountAllowed(kSoftwareEthernetQueueCount);
}

} /* namespace MtkAwdl */
