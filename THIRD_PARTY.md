# Third-party components

The driver derivative is distributed under the preserved top-level GPLv2 license; the exact per-file notices remain authoritative.

- GPLv2: LICENSE.
- BSD/OpenBSD/NetBSD/FreeBSD-derived files: preserve their source headers.
- tools/ClientKit: preserve tools/ClientKit/LICENSE (BSD 3-Clause). The legacy client is not required by this native target.
- MediaTek firmware: the four unchanged byte arrays in include/FwBinary.cpp are device firmware, not relicensed as GPL. They are intended only for MediaTek hardware. Corresponding MediaTek redistribution notices are preserved in LICENSES/LICENCE.mediatek and LICENSES/LICENCE.mediatek-mt7921.

Firmware notice source: Android Open Source Project external/linux-firmware, commit 73a298d5fd416c748dc9334036fda1d0664084ed, mt7921/LICENSE and mt7922/LICENSE. These map the MT7961/MT7921 and MT7922 file pairs to LICENCE.mediatek. This fork does not modify the firmware bytes or claim firmware authorship.

The firmware license permits use and redistribution for MediaTek chipsets and does not permit relicensing the firmware itself as GPL/LGPL. Keep firmware terms separate when redistributing this project or a binary.
