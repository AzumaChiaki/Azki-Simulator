# 在 Linux 上构建 Azki Simulator

[English](BUILDING-LINUX.en.md) · [返回首页](../README.md) · [macOS 构建说明](BUILDING.md)

## 状态

Linux 支持当前为**开发/CI 级**交付：从源码构建引擎、headless 运行固件、以本地控制面板操作。不提供 `.deb`/Flatpak 安装包，不做跨发行版承诺；官方以 Arch Linux（Wayland）为参考环境，其他发行版按同套依赖与包管理器对应安装即可。与真实手机的蓝牙射频配对（BlueZ）尚未实现，虚拟对端协议层（快应用与表盘安装等）已支持 Linux。

## 环境

- Linux，x86_64 或 aarch64。构建主机即运行主机。
- 构建依赖（Arch 包名）：`base-devel meson ninja pkgconf python glib2 pixman zlib libffi pcre2 zstd dtc libcap openssl`。
- 运行依赖：`bubblewrap`（进程隔离，需要非特权 user namespace）、`ffmpeg`（可选，录屏）。
- 桌面应用构建依赖：`dotnet-sdk`（.NET SDK，构建 `linux/` 下的 Avalonia 应用）。
- 首次构建需要联网下载 QEMU、CPython 与 Python wheel 的上游源码。

以 Debian/Ubuntu 为例的对应包：`build-essential meson ninja-build pkg-config python3 libglib2.0-dev libpixman-1-dev zlib1g-dev libffi-dev libpcre2-dev libzstd-dev device-tree-compiler libcap-dev libssl-dev bubblewrap`。

## 构建

```sh
git clone git@github.com:AzumaChiaki/Azki-Simulator.git
cd Azki-Simulator
./scripts/build.sh debug
```

`build.sh` 在 Linux 上分派到 `scripts/build-linux.sh`：从源码构建定制 QEMU（arm-softmmu、TCG、无显示后端）、内置 CPython 3.12 与 Pillow/littlefs，编译 YAFFS/UnQLite 工具，校验全部 ELF 的动态链接，最后连同主机 Runtime 一起打包。桌面应用属于交付物的一部分：仓库存在 `linux/` 而主机没有 dotnet SDK 时构建直接失败，只想要引擎包请显式设置 `AZKI_SKIP_APP=1`。Release 使用 `./scripts/build-linux.sh release`。

独立引擎构建：`python3 -B scripts/build-engine.py`，输出 `dist/Engine` 与 `dist/Engine-sources.tar.gz`。缓存目录 `build/` 可复用；`dist/` 只放最终产物；一次性 staging 在仓库外临时目录完成后清理，规则与 macOS 相同。

### 下载与索引

上游 tarball 与 PyPI 源码包先取官方地址，失败后自动改走公共镜像，并按 PyPI 公布的 SHA-256 校验，镜像无法替换内容。构建 wheel 时，PEP 517 后端还会自行安装 Cython、meson-python 一类的构建依赖，其索引固定为 `https://pypi.org/simple`（不继承主机 `pip.conf`，否则一个失效的镜像只会表现为 `from versions: none`）。网络受限时可显式指定：

```sh
AZKI_PIP_INDEX_URL=https://mirrors.cloud.tencent.com/pypi/simple ./scripts/build-linux.sh
```

替换索引意味着改由该镜像供给构建期工具链；引擎运行时的依赖闭包仍逐文件校验 SHA-256，不受影响。

桌面应用（`linux/`）的文件属性校验按 glibc x86_64 的 `struct stat` 布局实现；其他主机会显式报错，不会按偏移猜字段。

桌面应用把逃逸出命令的异常写进 `~/.local/share/AzkiSimulator/com.azki.AzkiSimulator/logs/gui.log`（`AZKI_DATA_ROOT` 会改变该前缀）；窗口只显示一行状态，排查要看这个文件。

## 与 macOS 构建的差异

| 项目 | macOS | Linux |
| --- | --- | --- |
| 进程隔离 | `/usr/bin/sandbox-exec`（Seatbelt SBPL） | `/usr/bin/bwrap`（bubblewrap 挂载命名空间） |
| 二进制完整性 | Mach-O 依赖重写 + ad-hoc codesign | 链接系统库；`ldd`/RUNPATH 闭合校验，`manifest.json` 逐文件 SHA-256 |
| 签名声明 | `signing.adHoc = true` | `signing.scheme = manifest-sha256` |
| GPL 对应源码 | Homebrew 配方与上游 tarball | 上游 tarball + `DEPENDENCIES.json`（系统库以构建主机发行版来源获取） |
| 设备画面 | 原生窗口 + `frame.raw` | 仅 `frame.raw`（QEMU 不编译 GTK/SDL；窗口化直接报错） |
| 虚拟蓝牙射频 | CoreBluetooth（实验功能） | 未实现；`--bluetooth reject/virtual`，BlueZ 后续支持 |

bwrap 与 Seatbelt 的授权粒度不同：绑定的只读目录（内置解释器、prepared 项目）内文件内容全部可读，而非仅清单固定的文件；可写范围仍然严格限定在该会话私有目录。bwrap 启动失败即硬失败，不会退回无沙箱执行。若内核禁用了非特权 user namespace（部分容器或加固内核），模拟器无法启动会话，需恢复 `kernel.unprivileged_userns_clone=1` 或在允许 userns 的环境运行。

## 测试

无需引擎产物即可回归：

```sh
python3 -m unittest discover -s tests -p 'test_*.py'
```

包含 C 设备代码的编译测试需要 `gcc`（或 clang）与 `pkg-config glib-2.0`；缺少编译器时自动跳过。`tests/test_system_crypto.py` 用 FIPS-197 已知答案向量与 ECDH 对称/往返性质钉死各主机系统加密桥的字节契约。

桌面应用单独回归：`dotnet test linux/AzkiSimulator.Linux.sln`，覆盖画面头契约、bwrap argv 结构、会话描述符、PMU ADC 取值，以及 `open` 标志位与符号链接拒绝（写错的八进制转十六进制只有这里能抓到）。

端到端验收需要真实固件本地导入（仓库不含固件）：导入 → 创建实例 → `start` → `frame.raw` 出帧 → 触控注入 → 日志 → `stop` 后无残留进程。

## GitHub Actions

`.github/workflows/build-linux.yml` 在 Arch Linux 容器内构建引擎与 Linux 包并跑同一套 Python 测试；容器无法使用 bwrap，完整 boot 验收仍由本地或自托管 runner 承担。发布模式与 macOS 工作流一致：草稿 Release、按“起点 + 运行序号 − 1”递增构建号、附校验和，只需要 `contents: write`。
