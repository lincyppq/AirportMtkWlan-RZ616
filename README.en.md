<!-- Modified by the RZ616/Tahoe derivative on 2026-10-22; see MODIFICATIONS.md. -->
# AirportMtkWlan-RZ616

[简体中文](README.md) | [English](README.en.md)

A native macOS Wi-Fi driver for **AMD RZ616 / MediaTek MT7922 (PCI ID `14c3:0616`)**, targeting **macOS Tahoe 26**.

Derived from [mnnobre/AirportMtkWlan](https://github.com/mnnobre/AirportMtkWlan), combining the MediaTek MT792x hardware driver with the AirportItlwm native wireless front end. Networks are managed through the macOS Wi-Fi menu without a separate connection client.

> Experimental software. Back up a bootable EFI and keep Ethernet and a recovery method available before installation.

## Compatibility

| Item | Scope |
| --- | --- |
| Wi-Fi adapter | AMD RZ616 / MediaTek MT7922 |
| PCI ID | `14c3:0616` |
| Operating system | macOS Tahoe 26, x86_64 |
| OpenCore kernel range | `MinKernel: 25.0.0`, `MaxKernel: 25.99.99` |

Other inherited device matches, including `14c3:7922` and MT7921 / RZ608, are outside this branch's compatibility commitment. The default build does not target macOS Sequoia 15.

## Features and changes

- Native macOS Wi-Fi menu, scanning, association and DHCP integration.
- RZ616 device identification and MT7922 firmware selection.
- Firmware reset/readiness, interrupt and RX-queue adjustments.
- Background scan throttling.
- Compilation against the inherited Tahoe wireless interface and layouts.

### Known limitations

- Association-status queries may disagree with actual connectivity.
- Bluetooth is outside this driver's scope.
- AirDrop, AWDL and Handoff are not provided.
- WPA3, enterprise authentication, sleep/wake and sustained high-load scenarios need further development and validation.

## Build

Requires macOS, Apple Command Line Tools or corresponding Xcode developer tools, and Python 3. The script obtains a pinned [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) revision.

```sh
./build.sh
codesign --force --sign - build/Release/AirportMtkWlan.kext
codesign --verify --verbose=2 build/Release/AirportMtkWlan.kext
```

Output: `build/Release/AirportMtkWlan.kext`. Ad-hoc signing is not Apple notarization or kernel-extension approval; system security requirements still apply.

## Installation

1. Back up your complete EFI on another disk and retain a recovery boot method.
2. Copy `AirportMtkWlan.kext` to `EFI/OC/Kexts/`.
3. Add or update its OpenCore `Kernel → Add` entry:

| Field | Value |
| --- | --- |
| `Arch` | `x86_64` |
| `BundlePath` | `AirportMtkWlan.kext` |
| `ExecutablePath` | `Contents/MacOS/AirportMtkWlan` |
| `PlistPath` | `Contents/Info.plist` |
| `Enabled` | `true` |
| `MinKernel` | `25.0.0` |
| `MaxKernel` | `25.99.99` |

4. Disable other drivers matching the same Wi-Fi adapter.
5. Save `config.plist` as **XML plist** and validate it with the matching OpenCore `ocvalidate`.
6. Reboot and join a network through the macOS Wi-Fi menu.

This repository does not provide a complete EFI. Never replace your whole `config.plist` with a documentation snippet.

### Removal and recovery

Remove this driver's `Kernel → Add` entry and its kext. If macOS cannot boot, use your backup EFI or restore the previous driver and configuration from another OS.

## Reporting issues

Include the macOS version/build, PCI ID, driver version or commit, reproduction steps and relevant errors. Remove SSIDs, passwords, MAC/IP addresses, serial numbers, UUIDs and personal paths from logs before sharing. A complete EFI is not required.

## Provenance and licensing

This project derives from AirportMtkWlan, SimpleMtkWlan, AirportItlwm and OpenBSD-related code. See [UPSTREAM.md](UPSTREAM.md), [CREDITS.md](CREDITS.md) and [MODIFICATIONS.md](MODIFICATIONS.md).

Driver code is distributed under [GPLv2](LICENSE); original per-file copyright and license notices remain applicable. MediaTek firmware and other third-party components retain their own terms: see [THIRD_PARTY.md](THIRD_PARTY.md) and [LICENSES/](LICENSES/). This is not an official upstream or NullMoth release.
