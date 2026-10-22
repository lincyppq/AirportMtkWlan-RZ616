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
