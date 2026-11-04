# awdl-airdrop 分支：用户态 AWDL / AirDrop 所需的驱动改动

[English summary](AWDL-AIRDROP.md)

本分支在 `main` 的基础上增加了一个给用户态 AWDL 协议栈使用的原始帧收发通道、固件卡死和 TX 卡死后的整芯片复位，以及 AWDL 能力位相关的实验代码。
配套的用户态程序在 [rz616-airdrop](https://github.com/lincyppq/rz616-airdrop)。

本驱动**不**提供 macOS 原生的 `awdl0` 接口。Finder / 共享菜单通过系统自带协议栈进行的无线隔空投送不可用（见第 6 节）。

## 1. 版本与改动

下表中的版本名（m2、m3a …）是开发时的内部编号，驱动启动日志里的 `tag=` 字段用它标识版本。

| 版本 | 驱动 tag | 内容 | 开关 |
|---|---|---|---|
| m2 | — | AWDL 接收诊断，放宽接收过滤：清除 RFCR 中的 `DROP_OTHER_TIM / A3_MAC / A3_BSSID / OTHER_BSS`（对应 Linux mt7921 的 `FIF_OTHER_BSS`），使 AWDL 帧（BSSID `00:25:00:ff:94:73`）进入驱动。额外收到的帧在进入 net80211 之前计数并丢弃。每秒在 IORegistry `MtkAwdlRx` 发布计数和最近 32 帧的帧头。 | 编译宏 `MTK_AWDL_RX_DIAG`；启动参数 `-mtkawdldiag`（只计数）/ `-mtkawdlrx`（放宽过滤） |
| m3a / m3a-2 | `rz616-m3a-2` | 新增 `MtkAwdlRawUserClient`（仅 root）：GetInfo / RxRead / Tx / TxsRead，ABI 见 `include/ClientKit/AwdlRaw.h`。RX：AWDL 动作帧和数据帧进入环形缓冲，由用户态读取。TX：用户态注入完整的 802.11 帧（固定 OFDM 速率，默认 6 Mbps）。帧长上限 `MTK_AWDLRAW_MAX_FRAME` = 800 字节。可选的逐帧 TX 状态报告（m3a-2 修正了 TXS 包 ID 的位置：word 3 的 31:24 位，即 mt76 connac2 的 `MT_TXS3_PID`）。附带 `tools/awdltool`。 | 编译宏 `MTK_AWDL_TX`；启动参数 `-mtkawdltx` |
| m3a-3 | — | 只在有等待 TX 状态的原始帧时才处理 TXS。 | `-mtkawdltxsoff` 关闭 TXS；运行 awdltool 时 `MTK_AWDL_NO_TXS=1` |
| m3a-4 | — | 扫描 quiesce：断开连接、关闭 HAL 或发起新扫描前，让正在进行的固件扫描至少运行 250 ms，再发送取消并最多等待 400 ms；扫描进行中时后台扫描返回 EBUSY。连续 2 次 MCU 超时或收到 coredump 时判定固件失效，之后的命令立即返回 EIO，并按 Linux `mt7921e_mac_reset` 的流程整芯片复位（关中断 → stop → DMA 复位 + WFSYS 复位 + 重新下载固件 → 重新 init），每次最多尝试 3 次，每次开机最多自动执行 8 次。 | `-mtknorecover` 关闭 |
| m3a-5 | `rz616-m3a-5` | TX 卡死看门狗：每秒检查一次；有待发数据，但硬件 DMA_IDX、cons 和 TX-free 事件 8 秒都没有变化时（扫描期间不计时），记录 `tx stall #N` 并执行 m3a-4 的整芯片复位。新增 `MTK_AWDLRAW_TX_RECOVER`，可用 `sudo tools/awdltool/awdltool recover` 手动触发。 | `-mtknorecover` 关闭所有自动恢复；`-mtknotxwd` 只关闭看门狗 |
| m3i-1 | `rz616-m3i-1` | AWDL 能力位开关（编译宏 `MTK_AWDL_CAP_PROBE`）。开关只存在于内核内存中，默认关闭，重启后恢复关闭。打开后，CARD_CAPABILITIES 应答中设置 `capabilities[4] \|= 1`。驱动记录每次 CARD_CAPABILITIES 调用（时间、调用进程、开关值、返回的 21 字节）和每次 VIRTUAL_IF_CREATE 请求（完整 32 字节请求和返回值），保存在 32 条的环形缓冲里。VIRTUAL_IF_CREATE 仍返回 `kIOReturnUnsupported`。user client 新增 selector 4（读状态）、5（设开关）。 | `tools/awdlcap`（`on` / `off` / `status`），需要 root 和 `-mtkawdltx` |
| m3i-2 | `rz616-m3i-2` | 开关打开时，对 role 6 的 VIRTUAL_IF_CREATE 尝试分配 `IO80211VirtualInterface`、以 role 6 和名字前缀 `awdl` 初始化、attach 并 registerService，并记录每一步的返回值。内核符号按名字调用。user client 新增 selector 6 / 7（手动建立 / 释放，测试用）。开关关闭时行为与 m3i-1 相同。 | `tools/awdlcap`（`create` / `drop`） |
| m3a-6 | `rz616-m3a-6` | **实验补丁，未应用，未安装，未运行**：`patches/experimental/m3a6-tx-gate-1600.patch`，说明见 [patches/experimental/README.md](../patches/experimental/README.md)。 | — |

不加新启动参数时，AWDL 相关代码不分配内存、不改变接收过滤。m3a-4 / m3a-5 的恢复机制默认开启。
本分支的 `AirportMtkWlan/build.json` 默认启用 `MTK_AWDL_RX_DIAG`、`MTK_AWDL_TX` 和 `MTK_AWDL_CAP_PROBE`。`Info.plist`、bundle id（`com.nullmoth.AirportMtkWlan`）和版本号与 `main` 相同，`config.plist` 中的 `Kernel → Add` 条目不需要修改。

## 2. 编译

需要 macOS、Command Line Tools（`xcode-select --install`）和 Python 3。在仓库根目录执行：

```sh
./build.sh                # 驱动 -> build/Release/AirportMtkWlan.kext（会拉取固定版本的 MacKernelSDK）
./build.sh awdltool       # -> tools/awdltool/awdltool
./build.sh awdlcap        # -> tools/awdlcap/awdlcap
codesign --force --sign - build/Release/AirportMtkWlan.kext
```

## 3. 安装

1. 把完整 EFI 备份到另一块盘或 U 盘，并确认可以从备份启动。
2. 用与 OpenCore 版本匹配的 `ocvalidate` 校验当前 `config.plist`，结果应为 0 个问题。
3. 用新编译的 `AirportMtkWlan.kext` 替换 `EFI/OC/Kexts/AirportMtkWlan.kext`（`Kernel → Add` 条目不变）。
4. 在 `NVRAM → Add → 7C436110-AB2A-4BBB-A880-FE41995C9F82 → boot-args` 中加入 `-mtkawdlrx -mtkawdltx`。修改后再运行一次 `ocvalidate`。
5. 完全关机后再开机（冷启动）。
6. 检查：
   - `kextstat | grep AirportMtkWlan` 显示驱动已加载。
   - `bash tools/mtklog.sh | grep -E 'tag=rz616|wedge guard|tx stall watchdog'` 应包含 `tag=rz616-m3i-2`。
   - `sudo tools/awdltool/awdltool info` 应显示 `present=1 state=4`，并有一行 `m3a5:`。

## 4. 卸载 / 回滚

- 从 OpenCore 菜单启动备份的 EFI；或
- 把原来的 `AirportMtkWlan.kext`（或 `main` 分支编译的版本）放回 `EFI/OC/Kexts/`，从 boot-args 中删除 `-mtkawdlrx -mtkawdltx`，运行 `ocvalidate`，冷启动。
- 不换驱动、只关闭功能：去掉 `-mtkawdltx` / `-mtkawdlrx` 关闭原始通道和放宽过滤；加 `-mtknorecover` 或 `-mtknotxwd` 关闭恢复机制。能力位开关重启后自动关闭。

## 5. 测试环境与结果

测试环境：AMD RZ616（PCI `14c3:0616`），Intel 黑苹果，macOS 26.7.1（25G241），OpenCore；iPhone XS Max，iOS 18.7.10；Mac 与 iPhone 连接同一台路由器的 5 GHz 网络（信道 149）。
以下结果均使用 rz616-airdrop 的用户态程序。

| 测试 | 日期 | 结果 |
|---|---|---|
| iPhone → Mac，iPhone 共享菜单，无线 | 2026-10-23 / 24 | 成功：3.5 MB HEIC、29 MB MOV、527 MB 文件。527 MB 那次传输过程中驱动自动执行了一次整芯片复位，传输完成。观测到的接收速度约 2 MB/s。 |
| m3a-5 TX 卡死看门狗 | 2026-10-23 | 大文件接收期间发生 TX 卡死，看门狗触发整芯片复位，复位成功（`recover ok`），驱动回到 RUN 状态。 |
| Mac → iPhone，用户态发送程序，无线 | 2026-10-24 / 25 | 成功：223 KB PNG（约 40 KB/s）、215 KB PNG（30–40 KB/s）。8 MB 文件在 57% 时失败：传输中 awdlpeer 报告发送持续失败（TXDEAD），Wi-Fi 驱动离开 RUN 状态，随后连接被关闭。 |
| m3i-1 能力位 | 2026-10-25 | 开关关闭时 wifip2pd 报告 “missing the capability”；打开后，在 Finder 隔空投送中点击 iPhone 时，wifip2pd 发出 VIRTUAL_IF_CREATE（role 6，随机本地 MAC，接口名为空），驱动返回 unsupported。关闭开关后恢复原行为。 |
| m3i-2 建立 awdl0 | 2026-10-25 | 失败：开关打开时，第 1 步 alloc 返回 `kIOReturnNoMemory`，没有建立接口。开关关闭时的对照测试无任何动作。 |
| m3a-6 | — | 未测试。 |

## 6. 已知限制

- **原生 AirDrop 不可用。** macOS 原生 AirDrop 无线连接 iPhone 需要 `awdl0`。本驱动没有 `awdl0`；在当前系统的 IO80211Family（1200.13.1）中，`IO80211VirtualInterface` 的 `MetaClass::alloc()` 恒返回 NULL，系统中也没有 `IO80211P2PInterface`，所以 m3i-2 的方法无法建立接口。插着 USB 数据线时，原生 Finder 隔空投送到 iPhone 可以成功（走 USB 网络接口）。
- **帧长上限 800 字节**：用户态 feth 桥的 MTU 最大 758（默认 700），速度低。
- **不能切换信道**：网卡停留在路由器所在信道，只能在 iPhone AWDL 时间表中同信道的时隙收发，所以 Mac 和 iPhone 需要连接同一台路由器的 5 GHz 网络。
- **TX 卡死**：大量发送时 TX 环可能停止前进。m3a-5 能通过整芯片复位恢复 Wi-Fi，但正在进行的传输会中断，Mac → iPhone 的大文件发送因此会失败。2026-10-25 一次 TX 卡死的驱动日志显示 `prod=121 cons=122`（消费者位置超过生产者），与原始帧注入和普通发送路径并发修改同一个 TX 环的情况一致。m3a-6 补丁让原始帧注入经过 command gate，尚未测试。
- 原始帧发送使用固定速率，没有速率控制。
- 只在上述一台机器上测试过。

## 7. 相关项目

- 用户态部分：[rz616-airdrop](https://github.com/lincyppq/rz616-airdrop)。
- 协议参考：[owly](https://github.com/t4t5/owly)（MIT）、[OpenDrop](https://github.com/seemoo-lab/opendrop)、[OWL](https://github.com/seemoo-lab/owl)；DMA 复位流程参考 Linux mt76（mt7921）。
