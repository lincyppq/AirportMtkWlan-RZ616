/*
 * VtableAnchors.hpp — IO80211AWDLProtocol anchors for Tahoe 25G241.
 *
 * Byte offsets match tools/abi/audit_vtable.py in ForbiddenItlwm commit
 * 69e88fa0fdb0ef7122f2aee81111651c32260a62 and the "slot" fields of
 * tests/awdl-native/native-vtables-25G241.json (the running 26.7.1
 * kernel collection, as published by that project). The symbol at offset
 * 0x11c0 is one past the 568-entry table and must be absent (0).
 *
 * GPLv2.
 */
#pragma once
#include <stddef.h>

namespace MtkAwdl {

struct VtableAnchor {
    unsigned byteOffset;
    const char *symbol; /* nullptr means the word must be 0 */
};

inline constexpr VtableAnchor kAwdlAnchors[] = {
    {0x0,    "__ZN19IO80211AWDLProtocolD1Ev"},
    {0x960,  "__ZN25IOSkywalkNetworkInterface21getSupportedWakeFlagsEPj"},
    {0xbe0,  "__ZN23IO80211SkywalkInterface15setInterfaceCCAE18apple80211_channeli"},
    {0xd20,  "__ZN23IO80211SkywalkInterface9statsDumpEv"},
    {0xe60,  "__ZN23IO80211SkywalkInterface12syncDPSStatsEP26apple82011_postMessage_dps"},
    {0xfc0,  "__ZN23IO80211VirtualInterface34_RESERVEDIO80211VirtualInterface15Ev"},
    {0xfc8,  "___cxa_pure_virtual"},
    {0x11b8, "___cxa_pure_virtual"},
    {0x11c0, nullptr},
};

inline constexpr unsigned kAwdlAnchorCount =
    sizeof(kAwdlAnchors) / sizeof(kAwdlAnchors[0]);

} /* namespace MtkAwdl */
