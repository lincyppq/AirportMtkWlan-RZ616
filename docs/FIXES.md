# What was fixed

Starting point: the KyatNguyen `mtk-dev` branch of SimpleMtkWlan (Ethernet-style
interface, WPA2 working, 14.6 Mbit/s down / 2.6 up on the tested card). Every step was
measured on hardware; commit messages have the details.

## Throughput (MediaTek driver, shared by both front ends)

| Problem | Effect | Fix | Commit |
|---|---|---|---|
| HT/VHT disabled, RX Block Ack refused | AP sent every data frame at OFDM 6 Mbit/s | HT20 (from the `mtk-speed` branch), RX Block Ack via `STA_REC_BA` | `aadd348`, `a6b8de3` |
| Only 20 MHz | 25 Mbit/s down | 40/80 MHz and VHT: channel width/center from the negotiated `ni_chw`, `STA_REC_VHT`, `WTBL_VHT`, BSS RLM | `a2f0b9b` |
| Frames dropped when the TX ring was full | TCP backed off | flow control (oactive) instead of dropping | `d3d98e0` |
| Shifted frame type compared with unshifted `IEEE80211_FC0_TYPE_DATA` | **every data frame sent at a fixed 6 Mbit/s with Block Ack disabled** | compare with the shifted constant | `e292e17` |
| `STA_REC_STATE` sent after association with `vht_opmode` 0 | rate control stuck at 20 MHz, 1 stream | fill `vht_opmode` | `d30f961` |
| All frames on hardware queue VO | no aggregation | queue by TID, as Linux | `d30f961` |
| TX Block Ack only in `STA_REC_BA` | hardware never aggregated | `WTBL_BA` entry first, as Linux `mt76_connac_mcu_sta_ba()` | `2d09355` |

Result: about 290 Mbit/s down, 155 up; loaded ping from ~500 ms to ~80 ms.

## Native Wi-Fi (AirportMtkWlan)

| Problem | Effect | Fix | Commit |
|---|---|---|---|
| Port of the `AirportItlwm-Sequoia` front end | | MediaTek HAL instead of iwm/iwx/iwn, net80211 WCL delta | `b096918`, `9ff3555` |
| HAL never reported `IEEE80211_EVT_DRIVER_READY` | joins deferred forever after the private-address restart, menu stuck "connecting" | report `DRIVER_RESET_BEGIN`/`DRIVER_READY` | `6f227e0` |
| Firmware device added with the factory MAC | associated but DHCP "no server" (unicast to the private address dropped) | program the current address | `5e36a64` |
| `tsleep_nsec()` held the workloop gate | every MCU command timed out when called from the native layer | `IOCommandGate::commandSleep()` when in the gate | `d680e73` |
| Beacon-loss events dropped as "bad seq" | slow reaction when the AP disappears | directed probe as `iwm_rx_bmiss()` | `77103c2` |
