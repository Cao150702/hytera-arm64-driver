# 海能达 HP780 写频线 — ARM64 Windows COM 口驱动(usbser 方案)

2026-09-08 实测通过:Parallels Windows 11 ARM64,设备出现 COM5,
CPS 可选 COM5 读写频;直连工具 read/write-param 全部实测成功。

## 安装步骤(ARM64 Windows)

1. 前置:Secure Boot 关(PD 的 EFI Secure Boot off)+ testsigning on
   ```
   bcdedit /set testsigning on
   ```
   (若被 Windows 更新重置,重设后重启)

2. 用 vm-scripts/make_cert.ps1 生成本地测试证书,将根证书导入"受信任的根证书颁发机构"

3. 安装驱动包:
   ```
   pnputil /add-driver Hytera-DMR-COM.inf
   ```

4. 插上写频线(或设备已连接则删设备实例后重扫):
   ```
   pnputil /scan-devices
   ```
   设备管理器"端口 (COM 和 LPT)"下出现
   "Hytera DMR Radio (COM Port)(COMx)"即为成功。

## 重要:先删掉 MCCI 原装驱动包

CPS 安装程序会在系统里留下 MCCI 原装驱动包(bkrw 系列,oem13-21),
它们的硬件 ID 匹配写频线且签名等级高,设备会自动匹配它们
然后安装失败(x64 内核驱动无法在 ARM64 加载),导致设备一直报错。
必须删除:
```
pnputil /enum-drivers   (找到 bkrw 开头的 oem 号)
pnputil /delete-driver oemXX.inf /uninstall /force
```

## 工具

直连工具支持 COM 口模式:
```
python hp780_tool.py read --com COM5 -o radio.bin
python hp780_tool.py write-param --com COM5 0x76 0x20
python hp780_tool.py info --com COM5
```

CPS 直接选 COM5 即可(串口速率 19200)。
