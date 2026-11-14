# Modification notices

Modified derivative maintained for lincyppq. Prepared for distribution on 2026-10-22. Original authors retain their copyrights. This is an experimental derivative, not an official upstream or NullMoth release.

RZ616 hardware adaptation and Tahoe build integration were developed during October 2026. Publication notices and bilingual documentation were updated on 2026-10-22.

- SimpleMtkWlan/hal_mwx/MtkMwx.cpp: RZ616 matching, MT7922 reset/readiness and interrupt/RX handling; optional diagnostics are not enabled by the default native build.
- AirportMtkWlan/WCL/Sequoia-Info.plist: RZ616 PCI ID match.
- AirportMtkWlan/AirportItlwmSkywalkInterface.hpp: native front-end integration declaration.
- AirportMtkWlan/WCL/WCL.cpp: background scan throttling.
- AirportMtkWlan/build.json: Tahoe deployment target and native target macro.
- README.md, README.en.md, CREDITS.md, UPSTREAM.md and docs/INSTALL.md: bilingual user documentation, provenance, compatibility and installation scope.
- .gitignore and .github/workflows/build-kext.yml: publication exclusions and manual native-driver build workflow. 
- LICENSES/ and THIRD_PARTY.md: additional firmware license notices.

No driver binary or complete EFI is distributed by this source-only publication.

## awdl-airdrop branch (2026-10-22 … 2026-10-25)

Experimental changes for a userspace AWDL / AirDrop stack; see docs/AWDL-AIRDROP-zh.md and docs/AWDL-AIRDROP.md.

- SimpleMtkWlan/hal_mwx/MtkMwx.cpp, MtkMwx.hpp: AWDL RX diagnostics and relaxed RX filter (m2), raw AWDL RX/TX channel and TX status (m3a, m3a-3), scan quiesce and full chip reset on dead firmware (m3a-4), TX-stall watchdog (m3a-5). All AWDL paths are behind the MTK_AWDL_RX_DIAG / MTK_AWDL_TX build macros and the -mtkawdldiag / -mtkawdlrx / -mtkawdltx boot-args; -mtknorecover / -mtknotxwd disable the recovery features.
- AirportMtkWlan/AirportItlwmV2.cpp, AirportItlwmV2.hpp, MtkAwdlRawUserClient.cpp/.hpp (new): root-only user client for the raw channel.
- include/ClientKit/AwdlRaw.h, include/HAL/MtkAwdlRawApi.h, tools/ClientKit/AwdlRaw.h (new): userspace/kernel ABI.
- tools/awdltool (new) and build.sh `awdltool` target: test tool.
- AirportMtkWlan/MtkAwdlCap.cpp/.hpp (new), AirportItlwmSkywalkInterface.cpp/.hpp, AirportItlwmV2.cpp, MtkAwdlRawUserClient.cpp, include/ClientKit/AwdlRaw.h, tools/ClientKit/AwdlRaw.h: AWDL capability switch, CARD_CAPABILITIES / VIRTUAL_IF_CREATE recorder and awdl0 creation attempt (m3i-1, m3i-2), behind the MTK_AWDL_CAP_PROBE build macro; switch default off.
- tools/awdlcap (new) and build.sh `awdlcap` target: control tool for the switch.
- patches/experimental/ (new): untested m3a-6 patch, not applied to the source tree.
- AirportMtkWlan/build.json: adds MtkAwdlRawUserClient.cpp, MtkAwdlCap.cpp and the MTK_AWDL_RX_DIAG / MTK_AWDL_TX / MTK_AWDL_CAP_PROBE defines.
- docs/AWDL-AIRDROP-zh.md, docs/AWDL-AIRDROP.md, README.md, README.en.md: branch documentation.

## On-channel native object (default off)

Apple-side interface code adapted from ForbiddenItlwm tag `v0.5.4-beta.2`, commit `69e88fa0fdb0ef7122f2aee81111651c32260a62`. No Bluetooth code. The m3a-6 patch is not applied. See docs/AWDL-NATIVE.md.

- include/Airport/macOS26/IO80211VirtualInterface.h, IO80211AWDLProtocol.h, IOSkywalkQueueFactories.h: Tahoe 25G241 declarations copied from that commit.
- include/MtkAwdl/NativePolicy.hpp, VtableAnchors.hpp: host-testable gate, build check, and vtable anchors.
- AirportMtkWlan/MtkAwdlInterface.cpp/.hpp: concrete IO80211AWDLProtocol. When the in-memory gate is on it registers four generic Skywalk queues and offers management frames and software Ethernet. TX submission stays disabled. Native inputPacket stays unused.
- AirportMtkWlan/MtkAwdlCap.cpp, MtkAwdlRawUserClient.cpp, AirportItlwmV2.hpp, include/ClientKit/AwdlRaw.h, tools/ClientKit/AwdlRaw.h, tools/awdlcap/awdlcap.c: second in-memory gate (selectors 9 and 10). Selector 8 stays unused.
- tests/awdl-native/: 25G241 vtable fixture and host audits.
- include/MtkAwdl/RegistrationImage.hpp, ManagementFrame.hpp, SoftwareRx.hpp, RxLease.hpp: measured registration bytes, flags 4 unless IOPMResetPowerStateOnWake is true, 72-byte management metadata, software Ethernet checks, and the shared teardown state machine. A gate-held stop keeps one cleanup retain and finishes from the last leave, off that gate. `transport_ready` stays 0.
- AirportMtkWlan/build.json: compiles MtkAwdlInterface.cpp.

