# awdl-airdrop 分支：给用户态 AWDL / AirDrop 用的驱动改动

[English summary](AWDL-AIRDROP.md)

> **实验性质。** 这个分支是给 [rz616-airdrop](https://github.com/lincyppq/rz616-airdrop)（用户态 AirDrop，owly 式）用的驱动。
> 它**没有**做出 macOS 原生的 `awdl0` 接口，Finder / 控制中心的原生无线隔空投送**仍然不能用**（见文末「轨道 B」，欢迎帮忙）。

## 1. 这个分支做了什么、为什么

macOS 原生 AirDrop 需要系统里有 `awdl0`（Apple Wireless Direct Link）接口，本驱动没有。
所以我们走另一条路：驱动只提供一个**原始帧收发通道**，AWDL 协议本身（同步、选举、时隙、数据帧）在用户态程序 `awdlpeer` 里跑，
再用 macOS 自带的 feth 虚拟以太网把 IPv6/TCP 交给系统协议栈，AirDrop 的 HTTPS 部分用 owly 的 Rust 实现。
驱动这边按时间顺序一共 5 个提交：

| 提交 | 内容 | 开关 |
|---|---|---|
| **m2** | AWDL 接收诊断 + 放宽接收过滤：清掉 RFCR 里 `DROP_OTHER_TIM / A3_MAC / A3_BSSID / OTHER_BSS`（同 Linux mt7921 的 `FIF_OTHER_BSS`），AWDL 帧（BSSID `00:25:00:ff:94:73`）才能进驱动。多出来的帧在进 net80211 之前计数并丢掉，正常上网看到的帧不变。每秒在 IORegistry `MtkAwdlRx` 发布计数和最近 32 帧帧头。 | 编译宏 `MTK_AWDL_RX_DIAG`；启动参数 `-mtkawdldiag`（只计数）/ `-mtkawdlrx`（放宽过滤） |
| **m3a** | 新的 `MtkAwdlRawUserClient`（只给 root）：GetInfo / RxRead / Tx / TxsRead 四个方法，ABI 见 `include/ClientKit/AwdlRaw.h`。RX：AWDL 动作帧和数据帧进环形缓冲由用户态读走；TX：用户态注入完整 802.11 帧（固定 OFDM 速率，默认 6 Mbps）。**帧长上限 `MTK_AWDLRAW_MAX_FRAME = 800` 字节**（超出拒绝），所以用户态 feth 桥的 MTU 只能设 700。可选每帧 TX 状态报告；m3a-2 修了 TXS 包 ID 位置（TXS word 3 的 31:24 位，mt76 connac2 `MT_TXS3_PID`）。附带 `tools/awdltool`（info / dump / sync / probe）。 | 编译宏 `MTK_AWDL_TX`；启动参数 `-mtkawdltx` |
| **m3a-3** | 只有在有「要 TX 状态」的原始帧没回报时才处理 TXS，平时只多一次判断。 | `-mtkawdltxsoff` 关闭 TXS；`MTK_AWDL_NO_TXS=1`（awdltool 运行时） |
| **m3a-4** | **扫描 quiesce + 固件卡死整芯片复位。** 现象：私有 Wi-Fi 地址导致 HAL 重启、或离开 RUN 时固件扫描刚开始/还在跑 → 固件发 coredump 或不再回应，之后所有 MCU 命令超时，只能重启电脑。修法：拆连接/关 HAL/发新扫描之前先让扫描至少跑 250 ms、发取消、最多等 400 ms；扫描中后台扫描返回 EBUSY。连续 2 次 MCU 超时或收到 coredump → 判定固件已死，后续命令立刻 EIO；复位任务照 Linux `mt7921e_mac_reset` 做：关中断 → stop → DMA 复位 + **WFSYS 复位 + 重新下载固件** → 重新 init，每次最多试 3 次，每次开机最多自动 8 次。 | `-mtknorecover` 全部关掉（退回旧行为） |
| **m3a-5** | **TX 卡死看门狗。** 现象：接收大文件时（每秒收约 3300 帧、发约 80 帧 ACK + AWDL 管理帧），WFDMA 发送环不再前进（DMA_IDX 不动、没有 TX-done/TX-free、`ring_free=0`），但 MCU 还活着，m3a-4 触发不了，Wi-Fi 死到重启。修法：每秒检查一次，有待发包但硬件 DMA_IDX、cons、TX-free 都 8 秒不动（扫描期间不计时）→ 日志 `tx stall #N ... -> full chip reset` → 走 m3a-4 的整芯片复位 + 重载固件 + 自动重连。新增 `MTK_AWDLRAW_TX_RECOVER`，`sudo awdltool recover` 可手动触发；`awdltool info` 多一行 `m3a5: ring_free= ... recover= ok= fail=`。真机上自动复位已经触发并成功过（`recover=2 ok=2`）。 | `-mtknorecover` 关所有自动恢复；`-mtknotxwd` 只关看门狗 |

不加任何新启动参数时，驱动行为和主分支一样（AWDL 相关代码不分配内存、不改过滤；m3a-4/m3a-5 的保护默认开着，用 `-mtknorecover` 可以关）。

本分支的 `AirportMtkWlan/build.json` 默认加了 `MTK_AWDL_RX_DIAG` 和 `MTK_AWDL_TX` 两个宏，`./build.sh` 直接编出带 AWDL 通道的驱动。
`Info.plist`、bundle id（`com.nullmoth.AirportMtkWlan`）和版本号都没变，`config.plist` 的 `Kernel → Add` 条目不用改。

## 2. 编译

需要 macOS + Command Line Tools（`xcode-select --install`）+ Python 3。

```sh
./build.sh                # 驱动 -> build/Release/AirportMtkWlan.kext（会拉取固定版本的 MacKernelSDK）
./build.sh awdltool       # 测试工具 -> tools/awdltool/awdltool
codesign --force --sign - build/Release/AirportMtkWlan.kext
```

## 3. 安装（务必先备份）

1. **完整备份 EFI 到另一块盘/U 盘**，并确认能从备份启动（OpenCore 选单里能选到）。
2. 安装前先用和你的 OpenCore 版本匹配的 `ocvalidate` 校验现有 `config.plist`，确认结果为 0 个问题。
3. 用新编的 `AirportMtkWlan.kext` 替换 `EFI/OC/Kexts/AirportMtkWlan.kext`（只换这一个；`Kernel → Add` 条目不变）。
4. 在 `NVRAM → Add → 7C436110-AB2A-4BBB-A880-FE41995C9F82 → boot-args` 里加上 `-mtkawdlrx -mtkawdltx`（用户态 AWDL 需要）。改了 config.plist 就再跑一次 `ocvalidate`。
5. **完全关机再开机（冷启动）**，不要只点重启——固件状态在热重启时可能残留。
6. 验证：
   - `bash tools/mtklog.sh | grep -E 'tag=rz616|wedge guard|tx stall watchdog'` 应看到 `tag=rz616-m3a-5`、`m3a-4 firmware wedge guard on`、`m3a-5 tx stall watchdog on`。
   - `sudo tools/awdltool/awdltool info` 应有 `state=4`（RUN）和 `m3a5:` 一行。
   - 可选：Wi-Fi 连上后 `sudo tools/awdltool/awdltool recover`，Wi-Fi 应在 5～20 秒内自己重连。

## 4. 撤销 / 回滚

- **最快**：从 OpenCore 选单启动备份的 EFI。
- **换回旧驱动**：把备份里的 `AirportMtkWlan.kext` 拷回 `EFI/OC/Kexts/`（或用 `main` 分支编译的版本），删掉 boot-args 里的 `-mtkawdlrx -mtkawdltx`，`ocvalidate` 后冷启动。
- **只关新功能、不换驱动**：boot-args 加 `-mtknorecover`（关掉 m3a-4/m3a-5 的自动恢复）或 `-mtknotxwd`（只关 TX 看门狗）；去掉 `-mtkawdltx` / `-mtkawdlrx` 就关掉原始通道和放宽过滤。

## 5. 已知限制

- **原始帧上限 800 字节 → feth MTU 700 → 每个 TCP 段约 628 字节。** 收大文件约 1.7～2 MB/s，要每秒约 3300 帧；发送方向（Mac→iPhone）用户态限速后更慢（约 75 KB/s）。**m3a-6（计划中，未做）**：上限提到约 1600、MTU 1500，帧数减半以上；要改 ABI，驱动和 awdlpeer 必须一起换。
- 不能切信道 / 没有 remain-on-channel：网卡停在路由器的信道上，只能在 iPhone AWDL 时间表里同信道的时隙碰面。**iPhone 和 Mac 必须连同一个 5 GHz 路由器**（我们测的是 149 信道）。
- TX 卡死的根因（固件哪一步卡住）还不知道；m3a-5 能自动复位恢复 Wi-Fi，但正在传的文件通常要重发。
- 原始发送是固定速率（默认 6 Mbps）、awdlpeer 默认不要 ACK、没有 WTBL/速率控制。
- 只在 RZ616 / MT7922（`14c3:0616`）+ macOS Tahoe 26 + Intel 上测过。

## 6. 轨道 B：原生 `awdl0` —— **没做，欢迎帮忙**

要让 Finder / 控制中心 / `sharingd` / `rapportd` 原生无线 AirDrop、接力、隔空播放能用，驱动需要向 IO80211 / Skywalk 注册一个真正的 AWDL 接口（`awdl0`），
并实现 Apple 私有的 AWDL 控制接口（同步参数、信道序列、服务发现、对端管理等），很可能还要 remain-on-channel。
这个分支**完全没有**做这部分。目前原生 Finder AirDrop 到 iPhone 只能走 USB 数据线（iOS 17+ 的应用服务 AirDrop 经 rapportd 伴随链接走 USB 网卡）。
如果你熟悉 IO80211Family / AirportItlwm 的 Skywalk 接口，或者有逆向 `awdl0` 的经验，非常欢迎提 issue / PR。

## 7. 相关

- 用户态部分：[rz616-airdrop](https://github.com/lincyppq/rz616-airdrop)（awdlpeer、airdroprecv/airdropsend、lbadv、后台服务、bt-leg）。
- AWDL / AirDrop 协议参考：[owly](https://github.com/t4t5/owly)（MIT）、[OpenDrop](https://github.com/seemoo-lab/opendrop)、[OWL](https://github.com/seemoo-lab/owl)。
