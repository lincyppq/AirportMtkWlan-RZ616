# awdl-airdrop branch — short English summary

[中文完整说明](AWDL-AIRDROP-zh.md)

Experimental driver changes that let a **userspace** AWDL stack ([rz616-airdrop](https://github.com/lincyppq/rz616-airdrop), owly-style) run on RZ616 / MT7922 under macOS Tahoe 26. **Native `awdl0` is not implemented** — Finder/Control Center wireless AirDrop still does not work (see "Track B" below; help wanted).

| Commit | What | Switch |
|---|---|---|
| m2 | AWDL RX diagnostics; relaxed RFCR filter (like Linux mt7921 `FIF_OTHER_BSS`) so AWDL frames reach the driver; extra frames are counted and dropped before net80211; stats in IORegistry `MtkAwdlRx` | `MTK_AWDL_RX_DIAG`; `-mtkawdldiag` / `-mtkawdlrx` |
| m3a | Root-only `MtkAwdlRawUserClient` (GetInfo/RxRead/Tx/TxsRead, ABI in `include/ClientKit/AwdlRaw.h`): raw AWDL RX ring + raw 802.11 TX injection at a fixed OFDM rate (default 6 Mbps); **max frame 800 bytes** (hence MTU 700 in userspace); TXS PID fix; `tools/awdltool` | `MTK_AWDL_TX`; `-mtkawdltx` |
| m3a-3 | TX status handling armed only while a TXS-wanted raw frame is outstanding | `-mtkawdltxsoff` |
| m3a-4 | Scan quiesce before teardown/new scan (min age 250 ms, cancel, wait ≤ 400 ms); dead-firmware detection (2 MCU timeouts or coredump) and full chip reset + firmware reload modelled on Linux `mt7921e_mac_reset` (3 tries, ≤ 8 auto per boot) | `-mtknorecover` |
| m3a-5 | TX-stall watchdog: no TX progress for 8 s (scan time excluded) → full chip reset; `MTK_AWDLRAW_TX_RECOVER` / `sudo awdltool recover`; `awdltool info` shows `m3a5:` counters. Seen working on real hardware (`recover=2 ok=2`) | `-mtknorecover`, `-mtknotxwd` |

Without the new boot-args the driver behaves like `main` (the m3a-4/m3a-5 safety nets are on by default; `-mtknorecover` turns them off). Bundle id, version and Info.plist are unchanged. This branch's `build.json` enables `MTK_AWDL_RX_DIAG` and `MTK_AWDL_TX`.

**Build:** `./build.sh` (kext) and `./build.sh awdltool`; ad-hoc sign with `codesign --force --sign -`.

**Install:** back up the whole EFI to another disk and make sure it boots; `ocvalidate` must pass before and after; replace only `EFI/OC/Kexts/AirportMtkWlan.kext`; add `-mtkawdlrx -mtkawdltx` to boot-args; **cold boot** (power off, then on). Check `bash tools/mtklog.sh` for `tag=rz616-m3a-5`, `wedge guard on`, `tx stall watchdog on`, and `sudo tools/awdltool/awdltool info` for `state=4` and an `m3a5:` line.

**Rollback:** boot the backup EFI, or put the old kext back, drop the boot-args, `ocvalidate`, cold boot. `-mtknorecover` / `-mtknotxwd` disable the recovery features without swapping the kext.

**Known limits:** 800-byte raw frames → MTU 700 → ~628-byte TCP segments, ~2 MB/s receive at ~3300 frames/s (m3a-6, planned: ~1600-byte frames / MTU 1500, ABI change); no channel switching / remain-on-channel, so iPhone and Mac must share the same 5 GHz AP; TX-stall root cause unknown (auto reset recovers Wi-Fi but the transfer in flight usually has to be resent); raw TX uses a fixed rate (default 6 Mbps) with no rate control; tested only on RZ616 `14c3:0616`, Tahoe 26, Intel.

**Track B — native `awdl0`: not done, help wanted.** It needs a real AWDL interface registered with IO80211/Skywalk plus Apple's private AWDL control surface (and probably remain-on-channel). Today native Finder AirDrop to an iPhone only works over a USB cable. Issues/PRs from people who know IO80211Family / AirportItlwm Skywalk internals are very welcome.
