#!/usr/bin/env python3
"""Check the Tahoe AWDL declarations against the 25G241 vtable fixture.

The fixture is native-vtables-25G241.json from ForbiddenItlwm commit
69e88fa0fdb0ef7122f2aee81111651c32260a62 (tag v0.5.4-beta.2). That project
reads it from the running 26.7.1 (25G241) kernel collection. This script
does not compile a kext and does not prove a BSD awdl0.
"""
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = Path(__file__).resolve().parent / "native-vtables-25G241.json"
ANCHORS = ROOT / "include/MtkAwdl/VtableAnchors.hpp"
PROTOCOL = ROOT / "include/Airport/macOS26/IO80211AWDLProtocol.h"
VIRTUAL = ROOT / "include/Airport/macOS26/IO80211VirtualInterface.h"
CONTROLLER = ROOT / "include/Airport/macOS26/IO80211ControllerV2.h"
CLASS = ROOT / "AirportMtkWlan/MtkAwdlInterface.hpp"
IMPL = ROOT / "AirportMtkWlan/MtkAwdlInterface.cpp"
COMMIT = "69e88fa0fdb0ef7122f2aee81111651c32260a62"
CONTROLLER_SHA = "f64adad8b32d7add714c8d05d3167edc0d4270fe9b448d1ab2c50ce80e50507b"

failures = []


def fail(message):
    failures.append(message)


def method_name(mangled):
    match = re.search(r"AppleBCMWLANProximityInterface(\d+)([A-Za-z0-9_]+)", mangled)
    if not match:
        return None
    length = int(match.group(1))
    return match.group(2)[:length]


def declared_methods(path):
    names = []
    for line in path.read_text().splitlines():
        match = re.search(r"(?:virtual\s+)?IOReturn\s+(\w+)\s*\(", line)
        if match:
            names.append(match.group(1))
    return names


def main():
    tables = json.loads(FIXTURE.read_text())
    awdl = tables["IO80211AWDLProtocol"]["entries"]
    proximity = tables["AppleBCMWLANProximityInterface"]["entries"]
    while awdl and int(awdl[-1]["packed"], 16) == 0:
        awdl.pop()
    if len(awdl) != 568:
        fail(f"AWDL table has {len(awdl)} slots, expected 568")
    if awdl[-1]["slot"] != 0x11B8:
        fail(f"last AWDL slot is {awdl[-1]['slot']}")
    by_slot = {entry["slot"]: entry["symbols"][0] for entry in awdl}

    anchor_text = ANCHORS.read_text()
    if COMMIT not in anchor_text:
        fail("anchor header does not name the reference commit")
    found = re.findall(r"\{(0x[0-9a-fA-F]+),\s*(?:\"([^\"]+)\"|nullptr)\}", anchor_text)
    if len(found) != 9:
        fail(f"parsed {len(found)} anchors")
    for offset, symbol in found:
        slot = int(offset, 16)
        if symbol is None or symbol == "":
            if slot in by_slot:
                fail(f"slot {offset} should be past the table")
            continue
        if by_slot.get(slot) != symbol:
            fail(f"slot {offset}: fixture {by_slot.get(slot)} header {symbol}")
        if symbol not in IMPL.read_text():
            fail(f"runtime check does not reference {symbol}")

    pure = []
    for index, entry in enumerate(awdl):
        if entry["symbols"][0] == "___cxa_pure_virtual":
            pure.append((entry["slot"], method_name(proximity[index]["symbols"][0])))
    if len(pure) != 64:
        fail(f"pure virtuals {len(pure)}")
    if pure[0] != (0xF40, "setMacAddress"):
        fail(f"first pure {pure[0]}")
    if pure[1][0] != 0xFC8 or pure[1][1] != "getSSID":
        fail(f"AWDL pure start {pure[1]}")

    protocol = declared_methods(PROTOCOL)
    virtual = declared_methods(VIRTUAL)
    if "setMacAddress" not in virtual:
        fail("virtual interface header has no setMacAddress")
    expected = ["setMacAddress"] + protocol
    actual = [name for _, name in pure]
    if expected != actual:
        fail("protocol header methods do not match the 25G241 pure slots")
        for want, got in zip(expected, actual):
            if want != got:
                fail(f"  method {want} != {got}")
                break
        if len(expected) != len(actual):
            fail(f"  count header {len(expected)} fixture {len(actual)}")

    subclass = declared_methods(CLASS)
    for name in expected:
        if name not in subclass:
            fail(f"MtkAwdlInterface does not declare {name}")

    digest = hashlib.sha256(CONTROLLER.read_bytes()).hexdigest()
    if digest != CONTROLLER_SHA:
        fail("macOS26 IO80211ControllerV2.h hash changed")
    controller = CONTROLLER.read_text()
    if "createVirtualInterface" not in controller or "attachVirtualInterface" not in controller:
        fail("controller header is missing the factory methods")
    create_at = controller.find("createVirtualInterface")
    attach_at = controller.find("attachVirtualInterface")
    between = controller[create_at:attach_at]
    if "\n" not in between or between.count("virtual") != 1:
        fail("createVirtualInterface and attachVirtualInterface are not adjacent")

    impl = IMPL.read_text()
    if "extern const uintptr_t mtkAwdlProtoVt[]" not in impl:
        fail("AWDL vtable symbol is not an extern declaration")
    if re.search(r"(?m)^const uintptr_t mtkAwdlProtoVt\[\]", impl):
        fail("unsized AWDL vtable is defined in this file")
    if "IO80211Controller::createVirtualInterface" not in impl:
        fail("family createVirtualInterface is not qualified")
    if "super::createVirtualInterface" in impl:
        fail("super::createVirtualInterface is still bound through the AWDL alias")
    if "#undef super" not in impl:
        fail("IO80211AWDLProtocol super alias is left in scope")
    if "registerEthernetInterface" in impl:
        fail("registration call is present")
    if "mwx_awdlraw_api_tx" in impl:
        fail("native path calls raw TX")
    for banned in ("BTLE", "Bluetooth", "btleadv"):
        if banned in impl:
            fail(f"native path mentions {banned}")

    for header in (ROOT / "include/ClientKit/AwdlRaw.h", ROOT / "tools/ClientKit/AwdlRaw.h"):
        text = header.read_text()
        if "MTK_AWDLRAW_MAX_FRAME 800" not in text:
            fail(f"{header} does not keep the 800-byte cap")
        if "kMtkAwdlRawGetInfo2" in text:
            fail(f"{header} defines the unapplied m3a-6 selector")
        if "kMtkAwdlNativeGet = 9" not in text:
            fail(f"{header} is missing selector 9")
    user = (ROOT / "AirportMtkWlan/MtkAwdlRawUserClient.cpp").read_text()
    if re.search(r"case\s+8\s*:", user) or "kMtkAwdlRawGetInfo2" in user:
        fail("user client implements selector 8")

    if failures:
        print("\n".join(failures))
        return 1
    print("awdl native layout: 568 slots, anchors match 25G241 fixture, "
          "registration and selector 8 closed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
