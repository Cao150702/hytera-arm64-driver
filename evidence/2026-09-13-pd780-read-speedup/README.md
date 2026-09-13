# 2026-09-13 PD780 读频全通 + 调速收官（v97）

## 结果

在 Parallels ARM64 Windows 11 虚拟机上，CPS 对 PD780 的完整读频在 **20 秒以内**完成
（修复前 ~162 秒），随后「读频成功！」→ 数据树完整加载 → 码板持久化
（`C:\cpsim\config\Model.dat` 314,988B + `dmr1200pdtmpt.usr` 115,960B）。

- 驱动：`hyterabulk` v97（DriverVer 1.0.1.15，ARM64）
- 每块（1432B）往返：**133ms → 18ms**（最快 9ms，最慢 45ms）
- 120 块连续传输 ≈ 2.2s；整读 ≤20s（旧 162s，约 8 倍）
- 自然流程（无调试工具、无手动干预）：开 CPS → 读频 → USB1 → 确定 → ~20s 出数据树

## 根因

v90 起 AppDriven 原始帧走 staging（`Stage[16384]` + `BlkTimer` **100ms** 定时释放），
每个块周期 ~133ms 里约 112ms 是这个扣留：

```
wr(写1c7) → ub(电台应答, +4ms) → stg(暂存) → rel(定时器释放, +112ms) → rd → next wr(+17~21ms)
```

v90 加它的动机是防"应答抢在 CPS 建立等待之前到达"的竞态，但后续验证表明：

1. 当时观察到的停顿现象是调试工具干扰 CPS 进程造成的假象；
2. 实际协议时序：CPS 的应答等待在命令发出前就已建立，而应答最快也要数毫秒才到达
   ——等待窗口不会被错过。

参考栈（原厂驱动路径）没有这个扣留，所以官方的快。

## 修复（v97 = v96 + 三点改动）

1. **直投递**：泵完成路径对 AppDriven 帧直接 `Fifo += 整帧`（原 staging 路径降级为
   FIFO 满时的 16K overflow 兜底，5ms 定时器）；每帧日志标记 `dir`（旧为 `stg`/`rel`）。
2. **出生时间戳**：`wl` 事件记录首次日志调用时的 wall-clock（FILETIME 秒低 32 位），
   便于把驱动环日志对齐到运行时刻表。
3. 环形日志沿用 v96 回卷（32KB 窗口）。

补丁位置（`kmdf-driver/hyterabulk.c`）：`HyteraPumpComplete` 的 `if (ctx->AppDriven)` 分支、
`HyteraDbg` 顶部。

## 见证数据

- **live68b.log**（读进行中的环 dump）：120 块连续 `wr:1f` 间隔
  **min/max/avg = 9/45/18ms**，`dir: 121`、`rel: 0`（全程直投递，零 staging）。
- **p68.txt**（完整运行日志）：`read OK posted` 后第一次采样（t+20s）即 `读频成功！100%`；
  点确定后 CPS 立刻写出 315KB `Model.dat`，`ALL-DIALOGS-GONE x3 at t+116s`。
- **click68.txt**：点确定辅助脚本日志（读频成功！+ 100% 两个对话框一次点掉）。
- **p68-tree.png**：结论截图——PD780 数据树（对讲机信息/公共设置/常规设置/XPT集群/
  DMR集群设置/MPT集群设置），状态栏 `PD780 | 400-470 MHz | USB`。

## 复现

1. 构建/部署：`vm-scripts/deploy.cmd` 同款流程——源码送入构建目录，
   INF DriverVer 递增，MSBuild ARM64 Rebuild，`v22deploy.cmd`
   （inf2cat `/os:10_GE_ARM64` + signtool `/sm /sha1 <leaf>` + pnputil remove/add/scan）。
2. 读频测试：本目录的 `p68.ps1`（干净无调试工具；watcher 自动选择端口栈 class 0x14；
   progress 采样 + 多点环 dump + 自动点确定载树）。辅助点击：本目录的 `click68.ps1`。
3. 驱动环 dump：`C:\dbg\dbglog.exe`（打开 `\\.\usbbulk`，IOCTL 0x220000 读 32KB 环；
   0x220001 清环）。

## 工程笔记（写给未来的自己）

- **无 BOM 的 UTF-8 .ps1 被 PowerShell 5.1 按 GBK 解析**：脚本里的中文串字面量变成乱码，
  `-match` 永不命中（自动点确定曾因此静默失效）。中文字符串用
  `[char]0x786E+[char]0x5B9A` 这类 char-code 最稳，或给 .ps1 加 UTF-8 BOM。
- **双 USB 接口实例（&0&1 / &0&2）**：`\\.\usbbulk` 只指向其中一个实例，CPS 流量可能落在
  另一个实例上。曾出现 4 份环 dump SHA 完全相同——说明被读的那个实例的环停在写满 32KB
  边界那一刻。**判断环日志新旧必须先比 SHA，并确认 dump 自身的 `ctl` 事件在不在尾部。**
  按实例直读可走 PnP 接口路径
  `\\?\usb#vid_238b&pid_0a11#<inst>#{8ff89775-cb39-41c3-a170-a52e990fa331}`。
- **跨层命令引号链**（zsh → prlctl → cmd/powershell）会吞双引号/变量——
  复杂 PowerShell 用 `-EncodedCommand`（UTF-16LE base64）最稳。
