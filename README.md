# Azki Simulator

[English](README.en.md) · [构建说明](docs/BUILDING.md) · [GPL-2.0](LICENSE)

面向 Apple Silicon Mac 的独立手环模拟器。原生 SwiftUI/AppKit 界面通过独立 QEMU 进程运行设备固件，不嵌入 Azki Studio。

## 功能

- 创建、命名和管理多个独立实例，同时显示设备画面。
- 鼠标触控、实时日志、截图，以及保存到桌面的录屏。
- 启动、停止、冷重启和实例存储持久化；会话默认不限时。
- 安装快应用和表盘，设置虚拟序列号、虚拟蓝牙地址与首次设置选项。
- 调整 PMU ADC 原始输入；界面注明已确认用途，未标定项目不表示真实物理单位。

目录覆盖小米手环 9、9 Pro、10、10 Pro、11 系列配置。具体固件的运行效果取决于模拟器对该版本硬件和存储布局的支持，目录收录不等于所有固件均已验证。固件切换采用冷重启。

外部蓝牙仍属于实验功能，当前不能承诺手机系统或小米运动健康能够完成配对和通信。

## 使用

需要 **Apple Silicon、macOS 15 或更高版本**。安装包内置解释器和模拟核心，使用时无需安装 Python、Homebrew 或开发工具。

1. 在仓库 [Actions](https://github.com/AzumaChiaki/Azki-Simulator/actions/workflows/build-dmg.yml) 中选择一次成功的构建，下载产物并解压。
2. 打开 DMG，把 **Azki Simulator** 拖入 **Applications**。
3. 在应用中导入自己有权使用的固件，然后创建实例、选择固件版本并启动。

开源仓库和自动构建产物**不包含厂商固件、真实设备身份、测试实例或个人日志**。固件由用户本地导入，不会提交回仓库。请保留实例数据；应用在版本切换时保留上一份存储快照。

Actions 产物没有 Developer ID 签名，也不提交 Apple 公证；Apple Silicon 可执行文件仅使用运行所需的 ad-hoc 签名。macOS 可能提示无法验证开发者。不要关闭系统安全机制；仅在确认来源后使用系统提供的单个应用打开方式。

## 构建

```sh
git clone git@github.com:AzumaChiaki/Azki-Simulator.git
cd Azki-Simulator
brew install python@3.12 meson ninja pkgconf glib gettext libffi pcre2 zstd dtc
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
export PATH="$(brew --prefix python@3.12)/libexec/bin:$PATH"
./scripts/build-dmg.sh
```

需要包含 Swift 6.2 或更新版本的完整 Xcode。构建输出位于 `dist/`，包括 DMG、项目源码和引擎依赖源码包。完整步骤与参数见[构建说明](docs/BUILDING.md)。

`VERSION` 管理应用版本，`BUILD_NUMBER` 管理构建号；当前为 **0.1.0（1）**。合并或直接推送到 `main` 会自动构建，也可以在 Actions 页面手动触发。工作流不需要 Apple 证书、密码或公证配置。

## 项目结构

| 路径 | 内容 |
| --- | --- |
| `Sources/AzkiSimulator` | 原生界面、实例管理与运行时通信 |
| `engine/project` | 固件运行封装和安装通信后端 |
| `engine/qemu-overlay` | QEMU 10.1.0 的设备实现与定制文件 |
| `engine/third-party` | 构建所需的第三方辅助源码 |
| `Assets` | 应用图标 |
| `scripts` | 引擎、应用和 DMG 构建脚本 |
| `.github/workflows` | GitHub Actions 自动构建 |
| `docs` | 中英文构建文档 |

## 许可证

Copyright © 2026 AzumaChiaki。

本项目自有代码按 **GNU General Public License version 2（GPL-2.0-only）** 发布，全文见 [LICENSE](LICENSE)。第三方代码保留各自版权和许可证；根许可证不替换其原有授权。相关文件位于 `engine/licenses`、`engine/third-party` 及其源码文件头，构建产物附带相应许可和源码材料。
