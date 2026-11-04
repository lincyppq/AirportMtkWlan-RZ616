# awdl-airdrop branch — English summary

[中文完整说明](AWDL-AIRDROP-zh.md)

This branch adds, on top of `main`, a raw AWDL frame channel for a userspace AWDL stack, full chip reset after firmware or TX stalls, and experimental AWDL capability code. The userspace programs are in [rz616-airdrop](https://github.com/lincyppq/rz616-airdrop). The driver does **not** provide a native `awdl0` interface; wireless AirDrop through the macOS system stack (Finder / share sheet) does not work.

## Versions

Version names (m2, m3a, …) are internal development labels; the driver prints them as `tag=` in its log.

| Version | Tag | Change | Switch |
|---|---|---|---|
| m2 | — | AWDL RX diagnostics; relaxed RFCR filter (like Linux mt7921 `FIF_OTHER_BSS`) so AWDL frames reach the driver; extra frames are counted and dropped before net80211; counters in IORegistry `MtkAwdlRx` | `MTK_AWDL_RX_DIAG`; `-mtkawdldiag` / `-mtkawdlrx` |
| m3a / m3a-2 | `rz616-m3a-2` | Root-only `MtkAwdlRawUserClient` (GetInfo/RxRead/Tx/TxsRead, ABI in `include/ClientKit/AwdlRaw.h`): raw AWDL RX ring and raw 802.11 TX at a fixed OFDM rate (default 6 Mbps); max frame 800 bytes; TX status with the PID fix (`MT_TXS3_PID`); `tools/awdltool` | `MTK_AWDL_TX`; `-mtkawdltx` |
| m3a-3 | — | TX status handled only while a TXS-wanted raw frame is outstanding | `-mtkawdltxsoff` |
| m3a-4 | — | Scan quiesce before teardown/new scan (≥ 250 ms, cancel, wait ≤ 400 ms); dead-firmware detection (2 MCU timeouts or coredump) and full chip reset + firmware reload following Linux `mt7921e_mac_reset` (3 tries, ≤ 8 automatic per boot) | `-mtknorecover` |
| m3a-5 | `rz616-m3a-5` | TX-stall watchdog: no TX progress for 8 s (scan time excluded) → full chip reset; `sudo tools/awdltool/awdltool recover` | `-mtknorecover`, `-mtknotxwd` |
| m3i-1 | `rz616-m3i-1` | AWDL capability switch (`MTK_AWDL_CAP_PROBE`): in-memory, default off; when on, CARD_CAPABILITIES replies get `capabilities[4] \|= 1`. Records CARD_CAPABILITIES calls and VIRTUAL_IF_CREATE requests; VIRTUAL_IF_CREATE still returns unsupported. Selectors 4/5 | `tools/awdlcap on/off/status` |
| m3i-2 | `rz616-m3i-2` | With the switch on, a role-6 VIRTUAL_IF_CREATE tries to allocate, init, attach and register an `IO80211VirtualInterface`; per-step results recorded. Selectors 6/7 (manual create/drop) | `tools/awdlcap create/drop` |
| m3a-6 | `rz616-m3a-6` | **Experimental patch, not applied, not installed, not run**: `patches/experimental/m3a6-tx-gate-1600.patch` (raw TX through the command gate, 1600-byte frames, GetInfo2). See [patches/experimental/README.md](../patches/experimental/README.md) | — |

Without the new boot-args the AWDL code allocates nothing and does not change RX filtering; the m3a-4/m3a-5 recovery is on by default. This branch's `build.json` enables `MTK_AWDL_RX_DIAG`, `MTK_AWDL_TX` and `MTK_AWDL_CAP_PROBE`. Bundle id, version and Info.plist are the same as `main`.

## Build, install, uninstall

- Build (repository root): `./build.sh`, `./build.sh awdltool`, `./build.sh awdlcap`; ad-hoc sign with `codesign --force --sign - build/Release/AirportMtkWlan.kext`.
- Install: back up the whole EFI and make sure it boots; `ocvalidate` before and after; replace only `EFI/OC/Kexts/AirportMtkWlan.kext`; add `-mtkawdlrx -mtkawdltx` to boot-args; cold boot. Check `bash tools/mtklog.sh` for `tag=rz616-m3i-2` and `sudo tools/awdltool/awdltool info` for `present=1 state=4` and an `m3a5:` line.
- Uninstall: boot the backup EFI, or put the previous kext back, remove the boot-args, `ocvalidate`, cold boot. `-mtknorecover` / `-mtknotxwd` disable recovery without changing the kext. The capability switch is off after every reboot.

## Tested

Hardware/OS: AMD RZ616 (`14c3:0616`), Intel Hackintosh, macOS 26.7.1 (25G241), OpenCore; iPhone XS Max, iOS 18.7.10; both on the same router's 5 GHz network (channel 149). Userspace: rz616-airdrop.

| Test | Date | Result |
|---|---|---|
| iPhone → Mac, share sheet, wireless | 2026-10-23/24 | Success: 3.5 MB HEIC, 29 MB MOV, 527 MB file (one automatic chip reset during the 527 MB transfer; transfer completed). Observed receive rate about 2 MB/s. |
| m3a-5 TX-stall watchdog | 2026-10-23 | A TX stall during a large receive triggered a full chip reset; reset succeeded and the driver returned to RUN. |
| Mac → iPhone, userspace sender, wireless | 2026-10-24/25 | Success: 223 KB PNG (about 40 KB/s), 215 KB PNG (30–40 KB/s). An 8 MB file failed at 57%: awdlpeer reported persistent TX failures (TXDEAD), the driver left RUN, the connection closed. |
| m3i-1 capability bit | 2026-10-25 | Switch off: wifip2pd logs "missing the capability". Switch on: clicking the iPhone in Finder AirDrop makes wifip2pd send VIRTUAL_IF_CREATE (role 6, random local MAC, empty name); the driver answers unsupported. Switch off again: original behaviour. |
| m3i-2 awdl0 creation | 2026-10-25 | Failed: step 1 (alloc) returns `kIOReturnNoMemory`; no interface. Control run with the switch off: no action. |
| m3a-6 | — | Not tested. |

## Known limitations

- **No native AirDrop.** Native wireless AirDrop needs `awdl0`. In the running IO80211Family (1200.13.1) `IO80211VirtualInterface::MetaClass::alloc()` always returns NULL and there is no `IO80211P2PInterface`, so the m3i-2 approach cannot create the interface. Native Finder AirDrop to an iPhone works only over a USB cable (USB network interface).
- 800-byte raw frames: the userspace feth bridge MTU is at most 758 (default 700); throughput is low.
- No channel switching: the card stays on the router's channel, so Mac and iPhone must use the same router's 5 GHz network.
- TX stalls under heavy transmit: m3a-5 restores Wi-Fi with a chip reset, but the transfer in progress is lost, so large Mac → iPhone sends can fail. A stall log from 2026-10-25 shows `prod=121 cons=122` (consumer ahead of producer), consistent with raw TX injection and the normal TX path updating the same ring concurrently. The m3a-6 patch serialises raw TX through the command gate; untested.
- Raw TX uses a fixed rate without rate control.
- Tested on one machine only.
