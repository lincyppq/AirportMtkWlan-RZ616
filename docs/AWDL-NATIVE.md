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
| Pool and queue factories in `IOSkywalkQueueFactories.h` | Used when the in-memory gate is on. Four generic queues, not Broadcom's six. TX submission stays disabled. |

Not taken, because it is Intel firmware context or out of scope:

- Listener / second MAC context, MCC, off-channel dwell, Block Ack, and the send gate in `itlwm/hal_iwx`.
- Master-indication parsing at raw offsets `+0x28` / `+0x30`.
- Native `inputPacket` / `packet_info_tag` peer accounting. Software Ethernet delivery does not use that tag. `transport_ready` stays 0.
- `BTLEAdvFixup` and every other Bluetooth component. Both Wi-Fi trees are GPLv2; that plugin is not.

`include/Airport/macOS26/IO80211ControllerV2.h` is byte-identical to the same file at the reference commit (SHA-256 `f64adad8b32d7add714c8d05d3167edc0d4270fe9b448d1ab2c50ce80e50507b`). Commit `916ba1` was compiled with Apple clang 17 against MacKernelSDK `3f750085caa17ec3a7880f11c11bf4f48cd6a164` (60 files, serial). The linked vtable matched the 25G241 fixture 568/568, with `setMacAddress` at `0xf40`, `getSSID` at `0xfc8`, and the controller factory at `0xdd8` / `0xde0`. While the gate is off, that factory slot calls `IO80211Controller::createVirtualInterface`. That is compile and static-link evidence, not a runtime trial.

Build check accepts only the 25G241 kernel tag `xnu-12377.161.15.700.19~2/RELEASE_X86_64`. ForbiddenItlwm also lists 25G332 and 25G342 from Kernel Debug Kits this tree has not re-checked, so those builds stay blocked. There is no boot-arg override.

## What a successful create means

`awdlcap create` while both switches are on, and only if the build, AWDL vtable, and controller-slot checks pass, does this:

- Builds `MtkAwdlInterface`, a concrete `IO80211AWDLProtocol`.
- Asks the controller's `attachVirtualInterface` to attach that one object.
- Builds four Skywalk queues (transmit submission, transmit completion, receive submission, receive completion) and calls `registerEthernetInterface`. Flags are 4 because this object does not publish `IOPMResetPowerStateOnWake`. A non-zero result unwinds the queues. Transmit submission stays disabled.
- Stores later `setAWDL_*` calls in memory. `setCHANNEL`, a companion interface, and out-of-band / master-indication requests return unsupported.
- Sets `MtkAwdlBsdRegistered` to 1 only when `registerEthernetInterface` returns 0. `MtkAwdlTransportReady` stays 0. That is not an AirDrop result.

Action frames, when the gate is on, are copied into an `IO80211Buffer` with a 72-byte metadata block and passed to `bpfTapInput`. The caller does not return the buffer. Data frames are checked for the AWDL BSSID, destination, source echo, LLC, sequence, and duplicates, then offered to the RX completion queue as Ethernet plus IPv6. A successful enqueue is not freed again. The native `inputPacket` tag path is not used.

If a check fails, nothing is attached and the blocker string is one of `unverified-build`, `awdl-vtable-mismatch`, `controller-slot-mismatch`. A rejected registration unwinds the queues and publishes `registration-rejected` plus the IOReturn.

Raw TX longer than 800 bytes is refused by `rawTxAdmit`. The value 1428 in the registration image is a size field from the native producer, not permission to put 1428 bytes on the shared station ring. Skywalk TX acceptance stays 0, so that ring is not taken by this object.

Selector 8 is unused. The m3a-6 patch still applies with `git apply --check` and is not applied. Raw TX still shares the station ring with the data path; that race is unchanged.

## Tests that ran here

On this Linux host, with the system `g++` and Python 3:

- `g++ -std=c++17 -Wall -Wextra -Werror -I include tests/awdl-native/test_policy.cpp` (includes the constexpr registration asserts)
- `g++ -std=c++17 -Wall -Wextra -Werror -pthread -I include tests/awdl-native/test_rx_lease.cpp`
- `python3 tests/awdl-native/audit_layout.py` (568 slots, anchors, feature default off, selector 8 unused, `transport_ready` stays 0)

## Tests that did not run

`d82689c` linked on Apple clang. Its vtable audit does not cover this later source. This host still has no MacKernelSDK. No kext was installed or loaded, and the native gate was not turned on. No hardware test was run. A gates-off source assert is not a runtime proof, and a host ThreadSanitizer run is not a kernel retain-count proof.

## Compile only, on a Mac

Apple clang 17 and MacKernelSDK `3f750085caa17ec3a7880f11c11bf4f48cd6a164` rejected `f3be352` in `MtkAwdlInterface.cpp`: three `static_assert`s called `registrationCallPermitted` and `genericQueueCountAllowed` before those functions were `constexpr`. Rebuild this head from scratch with `JOBS=1 ./build.sh`. Do not reuse the `916ba1` link or its vtable audit. The five new imports (`getMgmtFramePool`, `bpfTapInput`, `enqueuePackets`, `setHeadroom`, `setLinkHeaderLength`) were defined symbols in the 25G241 index; that is not a link of this kext.

From the repository root, with Xcode Command Line Tools installed:

```sh
JOBS=1 ./build.sh
python3 tests/awdl-native/audit_layout.py
c++ -std=c++17 -Wall -Wextra -Werror -I include tests/awdl-native/test_policy.cpp -o /tmp/awdl-native-policy
/tmp/awdl-native-policy
c++ -std=c++17 -Wall -Wextra -Werror -pthread -I include tests/awdl-native/test_rx_lease.cpp -o /tmp/awdl-rx-lease
/tmp/awdl-rx-lease
```

`JOBS=1` passes `--jobs 1` to `tools/build-xcode-kext.py`. Omitting it keeps the previous default, `os.cpu_count()`. `./build.sh` fetches MacKernelSDK `3f750085caa17ec3a7880f11c11bf4f48cd6a164` and writes `build/Release/AirportMtkWlan.kext`. It does not install the kext, does not edit EFI or boot-args, and does not reboot. The link expects `libclang_rt.cc_kext.a` under `/Library/Developer/CommandLineTools` and `libkmodc++.a` from a `MacOSX15*.sdk`. Those libraries are not on this host.

After that compile, the next static check is a Mach-O audit of `__ZTV16MtkAwdlInterface` against `tests/awdl-native/native-vtables-25G241.json`, and of `__ZN12AirportItlwm22createVirtualInterfaceEP10ether_addrj` at controller byte `0xdd8`. Do not load the kext on that evidence alone.

## Cleanup and memory

- Each interface has its own `MtkAwdl::Life`. A gate-held stop retains the object and the provider before it unlocks. The last leave cleans up inline, on or off that gate, with no thread call. `free` does not retain. The management pool stays with the controller. A failed RX completion resets, completes, and deallocates; a successful enqueue does not.
- `awdlcap native-off` and `awdlcap drop` then detach and release the interface. Whether `detachVirtualInterface` also releases the caller's retain has not been observed on hardware.
- The controller slot at `0xdd8` calls the family method while the gate is off. The `916ba1` link audit checked that slot. This later edit does not move the method, and it has not itself been linked.
- No second `sc_vif`, no MCC, and no off-channel firmware command is added.

## Remaining limits

- The Broadcom override of `registerSkywalkInterface` is not used. This object uses the generic four-queue factories. A non-zero `registerEthernetInterface` result unwinds them. The logical-link nexus flag bit is not named; the return value is the check.
- `packet_info_tag` is not built. Software Ethernet delivery does not need it. Native peer accounting through `inputPacket` is not implemented.
- The Broadcom companion pool was not named by a getter. This path uses separate TX and RX pools from `IOSkywalkPacketBufferPool::withName`.
- No second firmware interface, no MCC, and no off-channel command. Raw TX remains 800 bytes and is not used by this object.
