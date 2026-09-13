# 第三方组件说明 / Third-Party Notices

## 本仓库不包含的第三方内容

本仓库**不包含、也不分发**海能达通信股份有限公司（Hytera Communications）或
MCCI Corporation 的任何软件、代码或二进制文件；不包含任何原厂驱动包、CPS 安装文件或
其派生物。仓库中的全部代码均为本项目作者编写。

本文档中出现的 "Hytera"、"MCCI" 等名称与商标归各自权利人所有，仅用于描述兼容性。

## 外部可选依赖（不随本仓库分发，需自行获取）

| 组件 | 许可 | 用途 |
|---|---|---|
| libusb-1.0 | LGPL-2.1（https://libusb.info/） | `tools/hp780_tool.py` 在 Windows 上经 pyusb 访问 USB 时使用；如你在自己的分发中包含该 DLL，请遵守 LGPL-2.1 条款（附许可文本、提供源码获取途径） |
| pyusb | BSD-3-Clause | `tools/hp780_tool.py` 依赖 |
| Windows Driver Kit (WDK 26100) / VS2022 Build Tools | Microsoft 许可条款 | 构建 `kmdf-driver/` 所需 |
