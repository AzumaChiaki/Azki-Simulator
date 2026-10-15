# 构建 Azki Simulator

[English](BUILDING.en.md) · [返回首页](../README.md)

## 环境

- Apple Silicon Mac，macOS 15 或更新。
- 完整 Xcode，Swift 6.2 或更新；仅安装 Command Line Tools 不足以编译 SwiftUI 宏。
- Homebrew 构建依赖：`python@3.12 meson ninja pkgconf glib gettext libffi pcre2 zstd dtc`。
- 首次构建需要联网下载公开的上游源码和 Python 构建依赖。

应用使用者无需安装这些开发依赖。

## 生成 DMG

```sh
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
export PATH="$(brew --prefix python@3.12)/libexec/bin:$PATH"
./scripts/build-dmg.sh dist
```

脚本从源码构建定制 QEMU、内置 Python 和必要依赖，再构建原生应用并生成 DMG。缺少引擎时构建失败，不会输出只含界面的安装包。输出目录已存在同名 DMG 时，请选新目录或先自行移走旧产物。

| 参数 | 用途 |
| --- | --- |
| `build-dmg.sh` 的第一个参数 | 输出目录，默认 `dist` |
| `AZKI_WORK_DIR` | 临时构建工作区的父目录，默认 `build` |
| `AZKI_BUILD_DIR` | Swift 构建缓存目录 |
| `AZKI_BUILD_JOBS` | Swift 并行任务数，默认 2 |
| `DEVELOPER_DIR` | 要使用的完整 Xcode 开发目录 |

`VERSION` 与 `BUILD_NUMBER` 分别写入 `CFBundleShortVersionString` 和 `CFBundleVersion`。本地构建直接使用文件中的值。GitHub Actions 把它作为起点，按“起点 + 运行序号 − 1”自动递增，只在构建机上写入，不回写仓库；需要跳号时调大 `BUILD_NUMBER` 即可。

## 产物

- `*-unsigned.dmg`：原生应用和 Applications 快捷方式。
- `*-unsigned-source.tar.gz`：该次构建的项目源码及构建说明。
- `*-unsigned-engine-sources.tar.gz`：引擎及其依赖的对应源码与许可证。

请在分发 DMG 时同时提供其对应的源码包。DMG 和应用没有 Developer ID 签名或 Apple 公证，构建只执行无证书的 ad-hoc 签名以满足 Apple Silicon 的代码加载要求。

固件目录只包含模型和导入所需元数据，不包含 AP、OTA、资源镜像、真实设备身份或厂商 SDK。将自己的固件通过应用导入本地私有库。不要把本机实例、身份文件、私钥、Apple 密码或开发日志放入源码目录。

## GitHub Actions

`.github/workflows/build-dmg.yml` 在 `main` 收到 push 时运行，因此普通合并、squash 合并、rebase 合并和直接提交都能触发。也可手动执行 `workflow_dispatch`。

工作流使用 Apple Silicon macOS 15 runner 和 Xcode 26.2，从源码构建后发布 Release（标签 `v<版本>-build<构建号>`）：DMG 作为独立附件直接下载，GPL 对应源码包作为单独的可选附件。Release 先以草稿创建，全部上传完成后才公开。它只需要 `contents: write` 权限，不创建提交、不配置项目成员，也不使用签名或公证凭据。重新运行同一次构建会沿用构建号并替换该 Release 的附件。

GitHub 实际 runner 软件清单以[官方镜像说明](https://github.com/actions/runner-images/blob/main/images/macos/macos-15-arm64-Readme.md)为准。
