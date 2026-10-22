<!-- Modified by the RZ616/Tahoe derivative on 2026-10-22; see MODIFICATIONS.md. -->
<!--
  CREDITS.md
  SimpleMtkWlan

  Created by laobamac (王孝慈) on 2026/5/17.
  Copyright © 2026 laobamac (王孝慈). All rights reserved.
-->

# Credits

SimpleMtkWlan is maintained by laobamac (王孝慈).

This repository contains code from several upstream projects and systems. Their
copyright notices and license headers are intentionally preserved in source
files.

## Major Sources

- Original macOS WLAN driver framework and compatibility layer by zxystd
  (钟先耀), released under the GNU General Public License terms present in the
  source files.
- OpenBSD `net80211`, PCI, crypto, and driver support code, including work by
  OpenBSD, NetBSD, FreeBSD, and individual contributors credited in the source
  headers.
- MediaTek MT792x/OpenBSD `if_mwx` related driver code and definitions,
  including credits retained in `SimpleMtkWlan/hal_mwx/MtkMwx.cpp` and
  `SimpleMtkWlan/hal_mwx/if_mwxreg.h`.
- Firmware blobs and firmware packaging support for MediaTek MT7921 and MT7922.

## Current Project Work

- Project rename, packaging, and MT7921/MT7922-focused maintenance by laobamac
  (王孝慈).
- MediaTek MT7921/MT7922 attach, firmware load, DMA, scan, and TX/authentication path
  development continues in this repository.

## License Notes

The top-level project is distributed under GPLv2. Individual files may include
more specific GPL, BSD, OpenBSD, NetBSD, FreeBSD, or permissive license notices;
those notices remain authoritative for the corresponding files.

## AirportMtkWlan (this fork)

- Native macOS Wi-Fi front end (`AirportMtkWlan/`, `include/Airport/`): from the
  `AirportItlwm-Sequoia` target of [laobamac/itlwm](https://github.com/laobamac/itlwm),
  itself based on AirportItlwm by zxystd and the OpenIntelWireless contributors
  ([OpenIntelWireless/itlwm](https://github.com/OpenIntelWireless/itlwm)).
- `mtk-dev` fixes and PR #5: KyatNguyen and mavizao
  ([KyatNguyen/SimpleMtkWlan](https://github.com/KyatNguyen/SimpleMtkWlan/tree/mtk-dev)).
- `tools/ClientKit`: laobamac, [SimpleMTKClient](https://github.com/laobamac/SimpleMTKClient)
  (BSD 3-Clause).
- MediaTek protocol details were checked against the Linux mt76 driver (mt7921).
- Code changes after those, the Command Line Tools build and the documentation were
  written by Claude Code (Anthropic) and tested on hardware by the repository owner.

## RZ616 / Tahoe derivative

- This derivative adds local RZ616 (14c3:0616) hardware fixes and enables the inherited Tahoe native front-end build path.
- Preserve the original upstream authorship and all source-file notices. Compatibility and functional scope are documented in README.md and README.en.md.
- Neither the NullMoth name in inherited bundle identifiers nor upstream code provenance implies official endorsement.
