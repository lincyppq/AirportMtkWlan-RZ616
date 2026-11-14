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
| Pool and queue factories in `IOSkywalkQueueFactories.h` | Kept as declarations. The object does not allocate them. Native registration passes N+2 queues, and N was not recovered, so a fixed count of 4 is not used. |

Not taken, because it is Intel firmware context or out of scope:

- Listener / second MAC context, MCC, off-channel dwell, Block Ack, and the send gate in `itlwm/hal_iwx`.
- Master-indication parsing at raw offsets `+0x28` / `+0x30`.
- A live `registerEthernetInterface` call. The 0x130 image and the measured overrides can be filled (`include/MtkAwdl/RegistrationImage.hpp`), including flags 4 or 6 when the caller names one. The call stays closed: the condition that picks 4 versus 6 is unknown, and the queue count is N+2 with N unknown.
- A live management submit. Selector 12, a 72-byte block, a channel object at `+0x0c`, and a signed 32-bit signal at `+0x2c` are recorded. `bpfTapInput` is not called: the immediate/drop path returns the buffer inside the peer manager, and any other owner's duty is unproven. The caller must not free after a submit.
- IPv6 injection. Native data entry is `inputPacket` with an `IO80211NetworkPacket`, a `packet_info_tag`, an Ethernet header, and a peer lookup. Those contracts are not implemented.
- `BTLEAdvFixup` and every other Bluetooth component. Both Wi-Fi trees are GPLv2; that plugin is not.

`include/Airport/macOS26/IO80211ControllerV2.h` is byte-identical to the same file at the reference commit (SHA-256 `f64adad8b32d7add714c8d05d3167edc0d4270fe9b448d1ab2c50ce80e50507b`). Commit `916ba1` was compiled with Apple clang 17 against MacKernelSDK `3f750085caa17ec3a7880f11c11bf4f48cd6a164` (60 files, serial). The linked vtable matched the 25G241 fixture 568/568, with `setMacAddress` at `0xf40`, `getSSID` at `0xfc8`, and the controller factory at `0xdd8` / `0xde0`. While the gate is off, that factory slot calls `IO80211Controller::createVirtualInterface`. That is compile and static-link evidence, not a runtime trial.

Build check accepts only the 25G241 kernel tag `xnu-12377.161.15.700.19~2/RELEASE_X86_64`. ForbiddenItlwm also lists 25G332 and 25G342 from Kernel Debug Kits this tree has not re-checked, so those builds stay blocked. There is no boot-arg override.

## What a successful create means

`awdlcap create` while both switches are on, and only if the build, AWDL vtable, and controller-slot checks pass, does this:

- Builds `MtkAwdlInterface`, a concrete `IO80211AWDLProtocol`.
- Asks the controller's `attachVirtualInterface` to attach that one object.
- Does not allocate Skywalk pools or queues, and does not call `registerEthernetInterface`.
- Stores later `setAWDL_*` calls in memory. `setCHANNEL`, a companion interface, and out-of-band / master-indication requests return unsupported.
- Sets `MtkAwdlBsdRegistered` and `MtkAwdlTransportReady` to 0. `queues_up` stays 0.

The user-client reply can be success while `bsd_registered` is 0. That means the control object exists. `awdlcap native-status` prints `registration=0`, `rx_submit=0`, `ipv6=0`, and `transport=0`. Finder AirDrop is unchanged.

If a check fails, nothing is attached and the blocker string is one of `unverified-build`, `awdl-vtable-mismatch`, `controller-slot-mismatch`. When the object exists the registration blocker is `registration-flags-and-queue-count-unproven`. Management submit stays `management-async-ownership-unproven`. IPv6 stays `ipv6-needs-skywalk-packet-tag-ethernet-peer`.

Raw TX longer than 800 bytes is refused by `rawTxAdmit`. The value 1428 in the registration image is a size field from the native producer, not permission to put 1428 bytes on the shared station ring. Skywalk TX acceptance stays 0, so that ring is not taken by this object.

Selector 8 is unused. The m3a-6 patch still applies with `git apply --check` and is not applied. Raw TX still shares the station ring with the data path; that race is unchanged.

## Tests that ran here

On this Linux host, with the system `g++` and Python 3:

- `g++ -std=c++17 -Wall -Wextra -Werror -I include tests/awdl-native/test_policy.cpp`
- `python3 tests/awdl-native/audit_layout.py` (568 slots, anchors, pure-virtual order including `setMacAddress` at `0xf40` and `getSSID` at `0xfc8`, controller header hash, no registration call, selector 8 absent)

## Tests that did not run

Commit `916ba1` was linked on a Mac and its vtable audit matched, as recorded above. This later source change has not been compiled there. This host still has no MacKernelSDK and no Apple Command Line Tools. No kext was installed or loaded, and the native gate was not turned on. No hardware test was run.

## Compile only, on a Mac

Apple clang 17 and MacKernelSDK `3f750085caa17ec3a7880f11c11bf4f48cd6a164` rejected commit `cac190c` in `MtkAwdlInterface.cpp`: the AWDL vtable symbol was defined as an unsized array, and `#define super IO80211AWDLProtocol` made `AirportItlwm::createVirtualInterface` call `IO80211AWDLProtocol::createVirtualInterface`. Both are fixed in the source. This host has not compiled the corrected kext.

From the repository root, with Xcode Command Line Tools installed:

```sh
JOBS=1 ./build.sh
python3 tests/awdl-native/audit_layout.py
c++ -std=c++17 -Wall -Wextra -Werror -I include tests/awdl-native/test_policy.cpp -o /tmp/awdl-native-policy
/tmp/awdl-native-policy
```

`JOBS=1` passes `--jobs 1` to `tools/build-xcode-kext.py`. Omitting it keeps the previous default, `os.cpu_count()`. `./build.sh` fetches MacKernelSDK `3f750085caa17ec3a7880f11c11bf4f48cd6a164` and writes `build/Release/AirportMtkWlan.kext`. It does not install the kext, does not edit EFI or boot-args, and does not reboot. The link expects `libclang_rt.cc_kext.a` under `/Library/Developer/CommandLineTools` and `libkmodc++.a` from a `MacOSX15*.sdk`. Those libraries are not on this host.

After that compile, the next static check is a Mach-O audit of `__ZTV16MtkAwdlInterface` against `tests/awdl-native/native-vtables-25G241.json`, and of `__ZN12AirportItlwm22createVirtualInterfaceEP10ether_addrj` at controller byte `0xdd8`. Do not load the kext on that evidence alone.

## Cleanup and memory

- No Skywalk pool or queue is allocated, so the station work loop does not gain an event source from this object.
- `awdlcap native-off` and `awdlcap drop` call `stop`, detach, `detachVirtualInterface(..., false)`, and `release`. Whether `detachVirtualInterface` also releases the caller's retain has not been observed on hardware.
- The controller slot at `0xdd8` calls the family method while the gate is off. The `916ba1` link audit checked that slot. This later edit does not move the method, and it has not itself been linked.
- No second `sc_vif`, no MCC, and no off-channel firmware command is added.

## Still needed from a 25G241 disassembly

These are the next reads. They are not permission to load the kext.

1. In `AppleBCMWLANCore::createProxInterface`, which instruction writes the `%al` tested at the `testb %al,%al` / `cmovel` that stores 4 or 6 at RegistrationInfo+0x40?
2. In `AppleBCMWLANProximityInterface::start`, what stores the byte at context+0x3b1, and what are the vtables of the pointers at +0x408 (the N queue entries), +0x3f0, +0x3f8, +0x430, and +0x438?
3. In `IO80211AWDLPeerManager::actionFrameInput`, on the path that does not call `IO80211Buffer::returnBuffer` before returning, is the buffer queued, copied, or still owned by the caller?
4. For one 2.4 GHz chanspec and one 5 GHz chanspec, which three dwords does `ChanSpecConvToApple80211Channel` write through its `apple80211_channel*` out pointer?
5. Which offsets of `packet_info_tag` does `IO80211VirtualInterface::inputPacket` or `IO80211PeerManager::skywalkInputPacket` read, and is the supported data injection `inputPacket` or an RX completion enqueue?
