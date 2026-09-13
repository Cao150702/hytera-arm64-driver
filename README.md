# Hytera ARM64 驱动项目（海能达对讲机写频线）

## 你可能正遇到这个问题

用 **Apple 芯片的 Mac**,想读写自己的**海能达对讲机**（手台/终端）,于是装了
**Parallels Desktop（PD）或 VMware Fusion** 跑 Windows 11 **ARM64** 虚拟机,再装官方 CPS
（客户端编程软件）——**装 CPS 的时候它自带的写频线驱动就直接安装失败**（官方驱动只有
x86/x64 版,在 ARM64 Windows 上装不了）,CPS 里自然也**识别不到写频线**;
这台 Mac 上也没有别的机器能插这根线。

**本项目就是为这个场景做的**:提供 ARM64 原生 KMDF 驱动,**顶替装不上的官方驱动**,
让**官方 CPS 在 ARM64 Windows 虚拟机（以及任何 Windows on ARM 设备）里正常工作,
读取和写入海能达对讲机**。

| 路径 | 说明 |
|---|---|
| **官方 CPS（主线,推荐）** | 在 ARM64 虚拟机里安装本驱动包（替代装不上的官方驱动）→ 官方 CPS 全功能可用。已实测 **PD780 完整读频**:「读频成功」→ 数据树完整加载 → 码板保存,全程 **≤20 秒**;写频/写参数与读频共用同一驱动通路（回归测试欢迎反馈） |
| macOS 直连（附加,实验性） | `tools/hp780_tool.py` 经 libusb 从 macOS **不装 Windows 也能读回电台镜像**（已实测 PD780,~95KB）。用途:**读频备份**与协议研究/验证。⚠️ 读回的是电台传输态数据,目前**不能直接编辑**;产出官方存档格式（可编辑码板）在路线图上,欢迎贡献 |

**适用设备**:使用海能达 USB 写频线（**VID_238B & PID_0A11**,即 HP780 写频线）的 DMR
对讲机与终端（手台/车台/终端）。已在 **PD780** 完整实测;同一写频线、同一协议族的其它机型
按相同方式使用,欢迎在 Issues 反馈你的机型与结果。

**English**: On an Apple Silicon Mac running Parallels / VMware Fusion, installing the
official Hytera CPS fails at the bundled driver step — that driver ships for x86/x64 only
and cannot install into ARM64 Windows, so the CPS never sees the programming cable. This
project provides an ARM64-native KMDF driver that takes its place, so the **official CPS
works fully on Windows on ARM**: reads *and* programs Hytera DMR radios (handhelds &
terminals) using the Hytera USB programming cable (VID_238B & PID_0A11). Verified end-to-end
with PD780 (full radio read in under 20 s). Also includes an experimental macOS
direct-connect tool for raw-image backup and protocol study (not a codeplug editor).
Independent interoperability project; no Hytera or MCCI code or binaries included.

## 当前状态 (2026-09-13) —— ★PD780 读频全通 + 调速收官★

CPS 对 PD780 的**完整读频 ≤20 秒**跑通:「读频成功！」→ 数据树完整加载(对讲机信息/公共设置/
常规设置/XPT集群/DMR集群设置/MPT集群设置)→ 码板持久化(`Model.dat` ~315KB)。要点:

- 驱动 **v97**(DriverVer 1.0.1.15,ARM64):kind-20 世界
  (`hfc_hrcp.dll` + `hfc_portusb.dll`)原始 wire 帧直通;v97 去掉 v90 引入的 100ms
  staging 扣留、改为**直投递**——每块 133ms→18ms,整读 162s→≤20s(根因分析、实测数据、
  环日志见 `evidence/2026-09-13-pd780-read-speedup/`)。
- 自然使用方案:登录任务 `cps-read-fix`(`vm-scripts/cps-read-fix.ps1`)在 [读频] 对话框
  出现时自动为新版 USB 端口栈(class 0x14)完成选择(幂等无害);之后用户流程 =
  开 CPS → 读频 → 选 USB1 → 确定 → ~20 秒出数据树,无需任何手动干预。

## 目录结构

- `kmdf-driver/` —— ARM64 原生 KMDF 驱动 hyterabulk(当前主线,v97)
  - 兼容 MCCI bkrwbus 设备模型:设备描述 "USB Bulk Device"、符号链接 `\\.\usbbulk`、
    MCCI bkrwbus 与用户栈接口 GUID;kind-20 路径对用户态提供原始 wire 帧直通
    (7e.. 12B 头 + HRCP 内层),kind-10 的 [L][chk] 包裹路径保留作兼容
  - 已在 ARM64 Windows 11 上编译(machine 0xAA64)、测试签名、本机入库(构建与签名步骤见下文)
- `toolset-glue/` —— WDK 26100 的 MSBuild toolset 粘合层手写文件
  - WDK 26100 安装器不再给 VS2022 安装 `WindowsKernelModeDriver10.0` toolset,这 4 个文件手工补全
  - 安装到 `BuildTools\MSBuild\Microsoft\VC\v170\Platforms\<Arch>\` 对应位置
- `usbser-com/` —— 早期 usbser COM 口方案(存档;CPS 不识别 COM 口,已被 kmdf-driver 取代)
- `protocol/` —— 写频协议文档(基于实测会话整理:帧格式/校验/读频流程/写参数)
  - `HP780写频协议规范.md` 为正式整理稿;`hytera-protocol.json` = 参考会话帧记录
- `tools/` —— 直连工具(macOS libusb / Windows WinUSB / COM 串口三种后端;定位:读频备份/协议验证,非码板编辑器)
- `vm-scripts/` —— 部署与 VM 内运行脚本
  - 驱动部署/签名(`deploy.cmd` 等)、自然使用 watcher(`cps-read-fix.ps1`,配登录任务)、
    端到端读频测试 payload(`p68.ps1`,干净无调试工具 + 环 dump + 自动点确定)
- `evidence/` —— 按日期的验收存档(截图/驱动环日志/运行日志/结论 README)

## 构建

```bat
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" kmdf-driver\hyterabulk.vcxproj /p:Platform=ARM64 /p:Configuration=Release /p:WindowsTargetPlatformVersion=10.0.26100.0
```

前置:VS2022 Build Tools(含 ARM64 MSVC)+ SDK/WDK 26100 + testsigning + toolset-glue 已安装。

## 签名

```bat
inf2cat /driver:. /os:10_GE_ARM64
signtool sign /sm /sha1 <leaf-thumbprint> /v /fd sha256 /ph hyterabulk.sys
signtool sign /sm /sha1 <leaf-thumbprint> /v /fd sha256 /ph hyterabulk.cat
```

注意:`/os:Server10_ARM64` 会报 0xE0000242,客户端 ARM64 26100 必须用 `/os:10_GE_ARM64`;
`signtool /a` 会错选根证书,必须 `/sm /sha1` 显式指定 CodeSigning 叶子证书(机器存储)。
证书链需两级(根 CA + end-entity),生成与部署脚本见 `vm-scripts/`。

## 免责声明 / Disclaimer

- 本项目为个人互操作性(interoperability)项目,目的是让合法持有的设备在官方 CPS 中正常工作。
- 与海能达通信股份有限公司(Hytera Communications)及 MCCI Corporation 无任何隶属、赞助或背书关系。
- "Hytera"、"MCCI" 等名称与商标归各自权利人所有,此处仅用于描述兼容性。
- 本仓库不包含、也不分发海能达或 MCCI 的任何软件、代码或二进制文件。
- 请仅对您合法拥有或获授权操作的设备使用本项目的工具与脚本。

This is an independent interoperability project, not affiliated with, endorsed by, or
sponsored by Hytera Communications or MCCI Corporation. All trademarks belong to their
respective owners and are used only to describe compatibility. No Hytera or MCCI software,
code, or binaries are included or redistributed in this repository. Use only with devices
you legally own or are authorized to operate.

## 许可 / License

本仓库原创内容采用 Apache License 2.0(见 `LICENSE`)。
