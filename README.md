<!-- Modified by the RZ616/Tahoe derivative on 2026-10-22; see MODIFICATIONS.md. -->
# AirportMtkWlan-RZ616

[简体中文](README.md) | [English](README.en.md)

适用于 **AMD RZ616 / MediaTek MT7922（PCI ID：`14c3:0616`）** 的 macOS 原生 Wi-Fi 驱动，面向 **macOS Tahoe 26**。

基于 [mnnobre/AirportMtkWlan](https://github.com/mnnobre/AirportMtkWlan) 开发，沿用 MediaTek MT792x 硬件驱动与 AirportItlwm 原生无线框架。通过 macOS 自带的 Wi-Fi 菜单管理网络，无需额外连接客户端。

> 项目处于实验阶段。安装前请备份可启动的 EFI，并准备有线网络及恢复方式。

## 兼容性

| 项目 | 范围 |
| --- | --- |
| 无线网卡 | AMD RZ616 / MediaTek MT7922 |
| PCI ID | `14c3:0616` |
| 系统 | macOS Tahoe 26，Intel 架构（x86_64） |
| OpenCore 内核范围 | `MinKernel: 25.0.0`、`MaxKernel: 25.99.99` |

`14c3:7922`、MT7921 / RZ608 等其他设备 ID 保留上游匹配项，不在本分支的兼容性承诺范围内。当前默认构建不适用于 macOS Sequoia 15。

## 功能与改动

- 集成 macOS 原生 Wi-Fi 菜单、网络扫描、连接与 DHCP。
- 支持 RZ616 设备识别及 MT7922 固件选择。
- 调整固件复位、就绪等待、中断与接收队列处理。
- 限制后台扫描频率，减少重复扫描的影响。
- 使用 Tahoe 对应的原生无线接口及数据结构编译。

### 已知限制

- 网络关联状态查询可能与实际连接状态不一致。
- 蓝牙不由本驱动提供。
- 暂不提供 AirDrop、AWDL、Handoff 支持。
- WPA3、企业认证、睡眠唤醒以及持续高负载场景仍需完善与验证。

## 编译

需要 macOS、Apple Command Line Tools 或相应 Xcode 开发工具，以及 Python 3。构建脚本会获取固定版本的 [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK)。

```sh
./build.sh
codesign --force --sign - build/Release/AirportMtkWlan.kext
codesign --verify --verbose=2 build/Release/AirportMtkWlan.kext
```

生成文件：`build/Release/AirportMtkWlan.kext`。临时签名不等于 Apple 公证或系统扩展批准，系统安全设置仍须满足加载条件。

## 安装

1. 将完整 EFI 备份到其他磁盘，并确认有恢复启动方式。
2. 将 `AirportMtkWlan.kext` 复制到 `EFI/OC/Kexts/`。
3. 在 OpenCore 的 `Kernel → Add` 中添加或更新下列配置：

| 字段 | 值 |
| --- | --- |
| `Arch` | `x86_64` |
| `BundlePath` | `AirportMtkWlan.kext` |
| `ExecutablePath` | `Contents/MacOS/AirportMtkWlan` |
| `PlistPath` | `Contents/Info.plist` |
| `Enabled` | `true` |
| `MinKernel` | `25.0.0` |
| `MaxKernel` | `25.99.99` |

4. 停用其他匹配同一无线网卡的驱动，避免同时加载。
5. 将 `config.plist` 保存为 **XML plist**，使用与 OpenCore 版本匹配的 `ocvalidate` 校验。
6. 重启后，从 macOS Wi-Fi 菜单连接网络。

本项目不提供整机 EFI。不要用文档中的局部配置替换完整的 `config.plist`。

### 卸载与恢复

从 `Kernel → Add` 移除本驱动条目，并删除对应 kext。若系统无法启动，使用备份 EFI 启动，或从其他系统恢复原驱动及配置。

## 问题反馈

请提供系统版本与构建号、网卡 PCI ID、驱动版本或提交号、复现步骤及相关错误。提交前请检查日志，移除 Wi-Fi 名称、密码、MAC/IP 地址、序列号、UUID 和个人路径。无需提交完整 EFI。

## 来源与许可证

项目源自 AirportMtkWlan、SimpleMtkWlan、AirportItlwm 和 OpenBSD 相关代码，详细来源见 [UPSTREAM.md](UPSTREAM.md) 与 [CREDITS.md](CREDITS.md)，修改说明见 [MODIFICATIONS.md](MODIFICATIONS.md)。

驱动代码遵循 [GPLv2](LICENSE)，各文件原有版权和许可证声明保持有效。MediaTek 固件及其他第三方组件保留各自条款，详见 [THIRD_PARTY.md](THIRD_PARTY.md) 和 [LICENSES/](LICENSES/)。本项目不代表上游作者或 NullMoth 官方发行。
