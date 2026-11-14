# On-channel native AWDL object (experimental, default off)

This is a compile-time part of the `MTK_AWDL_CAP_PROBE` kext. Nothing in it runs unless both of these in-memory switches are on, and neither switch is saved across a reboot:

1. The existing capability switch (`awdlcap on`). While it is off, `VIRTUAL_IF_CREATE` does what m3i-1 did: no allocation.
2. The native gate (`awdlcap native-on`, user-client selector 10). Default off. Turning it on does not set the capability bit, does not apply `patches/experimental/m3a6-tx-gate-1600.patch`, and does not change the raw TX frame cap (800 bytes) or the default 6 Mbps raw rate.

Infra Wi-Fi is the same code path while the native gate is off. The gate does not add a second firmware interface, does not change channel, and does not touch the shared TX ring.

## Reference

Principal reference: [ForbiddenItlwm](https://github.com/retropineaple/ForbiddenItlwm) tag `v0.5.4-beta.2`, commit `69e88fa0fdb0ef7122f2aee81111651c32260a62` (2026-11-14). The driver sources at that tag match `v0.5.4-beta.1`; the tag itself only advances the separate LE advertising plugin, which is **not** imported.

Adapted from that tree, with the original GPLv2 terms:

| Taken | What changed for MT7922 |
|---|---|
| `include/Airport/macOS26/IO80211VirtualInterface.h`, `IO80211AWDLProtocol.h`, `IOSkywalkQueueFactories.h` | Copied. These replace the idea of allocating abstract `IO80211VirtualInterface` (its `MetaClass::alloc()` returns null). The old `include/Airport/IO80211VirtualInterface.h` still inherits `IOService` and is not the Tahoe Skywalk layout; the kext does not use it for this object. |
| `tests/awdl-native/native-vtables-25G241.json` | Copied fixture. ForbiddenItlwm describes it as the `IO80211AWDLProtocol` table from the running 26.7.1 (25G241) kernel collection. 568 slots. |
| Anchor list in `include/MtkAwdl/VtableAnchors.hpp` and the runtime compare in `MtkAwdlInterface.cpp` | Same anchors as `ForbiddenItlwm/AWDLBuildGuard.cpp` for Tahoe: destructor, wake flags, CCA, stats, DPS, reserved slot 15, pure virtual at `0xfc8` and `0x11b8`, and a zero word at `0x11c0`. |
| Stored `setAWDL_*` records, sync enabled/state, and the bounded frame-template copy | `include/MtkAwdl/NativePolicy.hpp`. Recording a request does not arm a scheduler. |
| Pool type 0, 128-buffer pools, four Skywalk queues, `attachVirtualInterface` then `initializeNetwork` | Queues are left disabled. `registerEthernetInterface` is not called. |

Not taken, because it is Intel firmware context or out of scope:

- Listener / second MAC context, MCC, off-channel dwell, Block Ack, and the send gate in `itlwm/hal_iwx`.
- Master-indication parsing at raw offsets `+0x28` / `+0x30`.
- RegistrationInfo byte writes (`0x0c`, `0x14`, `0x24`, `0x108`, and the rest in ForbiddenItlwm's `initializeNetwork`).
- RX metadata submit (the reference places RSSI at offset `0x2c` inside its own 72-byte struct). The 25G241 fixture has virtual tables only, so this tree does not submit management frames or IPv6 into Skywalk.
- `BTLEAdvFixup` and every other Bluetooth component. Both Wi-Fi trees are GPLv2; that plugin is not.

`include/Airport/macOS26/IO80211ControllerV2.h` is byte-identical to the same file at the reference commit (SHA-256 `f64adad8b32d7add714c8d05d3167edc0d4270fe9b448d1ab2c50ce80e50507b`). In that header `createVirtualInterface` is immediately followed by `attachVirtualInterface`. ForbiddenItlwm's `tools/abi/audit_vtable.py` checks the compiled controller symbol at byte `0xdd8` on Tahoe and Sequoia (`0xde0` is the next pointer). This environment did not re-run that Mach-O audit.

Build check accepts only the 25G241 kernel tag `xnu-12377.161.15.700.19~2/RELEASE_X86_64`. ForbiddenItlwm also lists 25G332 and 25G342 from Kernel Debug Kits this tree has not re-checked, so those builds stay blocked. There is no boot-arg override.

## What a successful create means

`awdlcap create` while both switches are on, and only if the build, AWDL vtable, and controller-slot checks pass, does this:

- Builds `MtkAwdlInterface`, a concrete `IO80211AWDLProtocol`.
- Asks the controller's `attachVirtualInterface` to attach that one object.
- Allocates a TX pool and an RX pool and four queues on the station work loop, and leaves the queues disabled.
- Stores later `setAWDL_*` calls in memory. `setCHANNEL`, a companion interface, and out-of-band / master-indication requests return unsupported.
- Sets `MtkAwdlBsdRegistered` and `MtkAwdlTransportReady` to 0.

The user-client reply can be success while `bsd_registered` is 0. That means the IOService object and the disabled queues exist. It does **not** mean `ifconfig` shows `awdl0`, and it does **not** mean AirDrop or any file transfer works. `awdlcap native-status` prints the distinction.

If a check fails, nothing is attached and the blocker string is one of `unverified-build`, `awdl-vtable-mismatch`, `controller-slot-mismatch`. Registration stays `registration-layout-unverified` even when the object exists. RX and IPv6 handoff stay unavailable for the same reason: the vtable fixture does not record those struct layouts.

Selector 8 is unused. The m3a-6 patch still applies with `git apply --check` and is not applied. Raw TX still shares the station ring with the data path; that race is unchanged.

## Tests that ran here

On this Linux host, with the system `g++` and Python 3:

- `g++ -std=c++17 -Wall -Wextra -Werror -I include tests/awdl-native/test_policy.cpp`
- `python3 tests/awdl-native/audit_layout.py` (568 slots, anchors, pure-virtual order including `setMacAddress` at `0xf40` and `getSSID` at `0xfc8`, controller header hash, no registration call, selector 8 absent)

## Tests that did not run

No kext was compiled, linked, installed, or loaded. There is no MacKernelSDK checkout and no Apple Command Line Tools in this environment (`clang` is Ubuntu 18.1.3, target `x86_64-pc-linux-gnu`). A syntax check of `MtkAwdlInterface.cpp` stops at `Apple80211.h` because the kext include map and the kernel headers are not here. The Mach-O vtable audit (`audit_vtable.py` against `AirportMtkWlan` / `MtkAwdlInterface`) was not run. No hardware test was run.

## Compile only, on a Mac

From the repository root, with Xcode Command Line Tools installed:

```sh
./build.sh
python3 tests/awdl-native/audit_layout.py
c++ -std=c++17 -Wall -Wextra -Werror -I include tests/awdl-native/test_policy.cpp -o /tmp/awdl-native-policy
/tmp/awdl-native-policy
```

`./build.sh` fetches MacKernelSDK `3f750085caa17ec3a7880f11c11bf4f48cd6a164` and writes `build/Release/AirportMtkWlan.kext`. It does not install the kext, does not edit EFI or boot-args, and does not reboot. `build.sh` then expects, for the link, `libclang_rt.cc_kext.a` under `/Library/Developer/CommandLineTools` and `libkmodc++.a` from a `MacOSX15*.sdk`. Those libraries are not on this host.

After that compile, the next static check is a Mach-O audit of `__ZTV16MtkAwdlInterface` against `tests/awdl-native/native-vtables-25G241.json`, and of `__ZN12AirportItlwm22createVirtualInterfaceEP10ether_addrj` at controller byte `0xdd8`. Do not load the kext on that evidence alone.

## Cleanup and memory

- Pools are 128 packets, not the reference's 2048-packet transport pool. Queues are 64 entries and stay disabled, so this path does not complete Skywalk transmits.
- The four queues are attached to the station work loop. `awdlcap native-off` and `awdlcap drop` call `stop`, detach, `detachVirtualInterface(..., false)`, and `release`. Whether `detachVirtualInterface` also releases the caller's retain has not been observed on hardware; a double-free or a leaked station-work-loop source would show up only on that experimental teardown.
- The controller vtable slot for `createVirtualInterface` is replaced for the life of the kext. While the gate is off the method calls the family implementation. That is safe only if the Tahoe header's slot matches 25G241. The header matches the reference file whose compiled audit checked `0xdd8`; this tree has not repeated the compiled check. A mismatch would put this method in the wrong slot even while the gate is off.
- No second `sc_vif`, no MCC, and no off-channel firmware command is added. Firmware concurrency for a second queue is not established.
