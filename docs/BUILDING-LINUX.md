# 在 Linux 上构建 Azki Simulator

[English](BUILDING-LINUX.en.md) · [返回首页](../README.md) · [macOS 构建说明](BUILDING.md)

## 状态

Linux 支持当前为**开发/CI 级**交付：从源码构建引擎、headless 运行固件、以本地控制面板操作。不提供 `.deb`/Flatpak 安装包，不做跨发行版承诺；官方以 Arch Linux（Wayland）为参考环境，其他发行版按同套依赖与包管理器对应安装即可。与真实手机的蓝牙射频配对（BlueZ）尚未实现。虚拟对端协议层（快应用与表盘安装等）的平台分支已经移植——AES/ECDH 走 libcrypto、对端凭据走 `SO_PEERCRED`。对端**自己这一半**现在有端到端测试（`tests/test_virtual_peer_session.py`）：unix socket 会话、内核对端凭据门禁、SMP Just Works 对话（confirm 由一份独立实现算出）、CCM/MIC 链路层、合成包的暂存与绑定，两个主机都能跑；**guest 那一半**（HCI/L2CAP/ATT、业务通道、把包装到设备上）仍然没有端到端验证，只有真的导入一个包跑过才算验过。另外发现一处对端自身代码的注释与实现不一致：`virtual_peer/smp.py` 的 `VirtualAir` 注释写「两端各一把独立密钥」，但一次穿越里用一把密钥封装、另一把密钥解封，所以只有两端派生出同一把密钥时才成立（旧配对流程里临时密钥就是链路密钥，正好成立）；这条差异已由测试钉住，改动前请先想清楚。

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

`build.sh` 在 Linux 上分派到 `scripts/build-linux.sh`：从源码构建定制 QEMU（arm-softmmu、TCG、无显示后端）、内置 CPython 3.12 与 Pillow/littlefs，编译并在一次不含固件的往返中运行 YAFFS/UnQLite 工具（建库、灌盘、读出、改写、冷重挂载后再读），校验全部 ELF 的动态链接，最后连同主机 Runtime 一起打包。桌面应用属于交付物的一部分：仓库存在 `linux/` 而主机没有 dotnet SDK 时构建直接失败，只想要引擎包请显式设置 `AZKI_SKIP_APP=1`。Release 使用 `./scripts/build-linux.sh release`。

独立引擎构建：`python3 -B scripts/build-engine.py`，输出 `dist/Engine` 与 `dist/Engine-sources.tar.gz`。缓存目录 `build/` 可复用；`dist/` 只放最终产物；一次性 staging 在仓库外临时目录完成后清理，规则与 macOS 相同。

### 下载与索引

上游 tarball 与 PyPI 源码包先取官方地址，失败后自动改走公共镜像，并按 PyPI 公布的 SHA-256 校验，镜像无法替换内容。构建 wheel 时，PEP 517 后端还会自行安装 Cython、meson-python 一类的构建依赖，其索引固定为 `https://pypi.org/simple`（不继承主机 `pip.conf`，否则一个失效的镜像只会表现为 `from versions: none`）。网络受限时可显式指定：

```sh
AZKI_PIP_INDEX_URL=https://mirrors.cloud.tencent.com/pypi/simple ./scripts/build-linux.sh
```

替换索引意味着改由该镜像供给构建期工具链；引擎运行时的依赖闭包仍逐文件校验 SHA-256，不受影响。

桌面应用（`linux/`）的文件属性校验按 glibc x86_64 的 `struct stat` 布局实现；其他主机会显式报错，不会按偏移猜字段。

桌面应用把逃逸出命令的异常写进 `~/.local/share/AzkiSimulator/com.azki.AzkiSimulator/logs/gui.log`（`AZKI_DATA_ROOT` 会改变该前缀）；窗口只显示一行状态，排查要看这个文件。

桌面应用的其他本机数据路径：连接超时保存在 `~/.config/azki-simulator/settings.json`（键 `simulator.connectionTimeoutSeconds`，默认 30、范围 1–300，与 macOS 的 UserDefaults 同名，`AZKI_CONFIG_ROOT` 可改前缀），只约束 guest/external 连接的等待预算，下一次操作生效；录屏输出到 `~/Videos/Azki Simulator`（无视频目录时退回 `~/Pictures/Azki Simulator`），编码依赖系统 `ffmpeg`，缺失时录制按钮给出明确报错而不是静默降级；设备窗口的「设备数据」面板与 macOS 同义——运行中“应用”经 `live-set` 即时改变模拟硬件输入并同步保存下次开机值（`instance-settings` CLI），停止时只保存下次开机的值；「系统设置…」对话框对应 macOS 的固件设置表（persist.db 键值与开机时钟），固件 tab 支持“切换并重新开机”（先停录屏再切）。实例管理器的「主机状态共享」只读展示 `/tmp/azki-host-*`（`AZKI_TMP_ROOT`）下由 macOS 版发布的 status-files 会话，Linux 端不发布、不写请求。

## 与 macOS 构建的差异

| 项目 | macOS | Linux |
| --- | --- | --- |
| 进程隔离 | `/usr/bin/sandbox-exec`（Seatbelt SBPL） | `/usr/bin/bwrap`（bubblewrap 挂载命名空间） |
| 二进制完整性 | Mach-O 依赖重写 + ad-hoc codesign | 链接系统库；`ldd`/RUNPATH 闭合校验，`manifest.json` 逐文件 SHA-256 |
| 签名声明 | `signing.adHoc = true` | `signing.scheme = manifest-sha256` |
| GPL 对应源码 | Homebrew 配方与上游 tarball | 上游 tarball + `DEPENDENCIES.json`（系统库以构建主机发行版来源获取） |
| 设备画面 | 原生窗口 + `frame.raw` | 仅 `frame.raw`（QEMU 不编译 GTK/SDL；窗口化直接报错） |
| 虚拟蓝牙射频 | CoreBluetooth（实验功能） | 未实现；`--bluetooth reject/virtual`，BlueZ 后续支持 |

bwrap 与 Seatbelt 的授权粒度不同：绑定的只读目录（内置解释器、prepared 项目）内文件内容全部可读，而非仅清单固定的文件；可写范围仍然严格限定在该会话私有目录。bwrap 启动失败即硬失败，不会退回无沙箱执行。若内核禁用了非特权 user namespace（部分容器或加固内核），模拟器无法启动会话，需恢复 `kernel.unprivileged_userns_clone=1` 或在允许 userns 的环境运行。「缺 launcher 就硬失败」这条由 `tests/test_isolation_fail_closed.py` 钉住：launcher 路径不存在（或指向的不是普通文件）时，隔离层抛 `isolation_unavailable` 且**不产出任何 argv**，装了 launcher 的机器则必须通过这道门、栽在后面的计划检查上；变异探针把守卫删掉立刻打红。启动器路径在隔离层只有 `bwrap_policy.LAUNCHER` 一处定义，argv 与存在性检查读同一个常量。两个 trampoline 是独立脚本（`-I` 隔离模式下走不了包内导入），各自持有一份字面量，但断言按计划写成「launcher ∈ {sandbox-exec, bwrap}」的集合，而不是把某台主机的 launcher 写死：`tests/test_launch_trampoline.py` 钉住这点——集合外的 launcher、以及不是父进程发起的调用，都被硬拒（变异探针把 launch.py 的检查删掉立刻打红）。

## 测试

无需引擎产物即可回归：

```sh
python3 -m unittest discover -s tests -p 'test_*.py'
```

包含 C 设备代码的编译测试需要 `gcc`（或 clang）与 `pkg-config glib-2.0`；缺少编译器时自动跳过。加密桥由已知答案向量钉死字节契约：`tests/test_system_crypto.py` 用 FIPS-197 向量与一份独立实现的 P-256 点运算（其中 d→Q 用 NIST 公布密钥对校验）对照系统 ECDH 输出；`tests/test_aes_modes.py` 用 NIST SP 800-38C/SP 800-38A 向量对照 AES-CCM 与 AES-CTR 布局。同一套向量在 macOS 与 Linux 上都跑，所以两主机的加密桥若彼此漂移，对应平台的 CI 会红。`tests/test_peer_identity.py` 用一次真 fork 与 unix socket 对照内核给出的对端凭据（两端必须报出**对方**的 pid），钉住 `SO_PEERCRED` 的结构布局；`tests/test_command_allowlists.py` 要求 macOS UI、Linux UI 与 Python 分发器三份命令白名单互相吻合，多一个动词少一个动词都跑不过。

桌面应用单独回归：`dotnet test linux/AzkiSimulator.Linux.sln`，覆盖画面头契约、bwrap argv 结构、会话描述符、PMU ADC 取值，以及 `open` 标志位与符号链接拒绝（写错的八进制转十六进制只有这里能抓到）。一次性 CLI 的写权限由 `Core/CliGrants.cs` 依据命令与 argv 自己算出，形状照抄 macOS 的 `StandaloneProcessSandbox.makeInspection`：改库（import/delete/delete-all）才给整个固件库，改实例（settings/reset/delete）只给 `<库>/instances/<uuid>` 这一个目录，`prepare-bundled` 给实例目录加预留的会话根，`sessions-cleanup` 只绑本程序自己前缀（`azki-b-`/`azki-sim-`/`azki-cmd-`）的残留根——沙箱里的 `/tmp` 是私有 tmpfs，不绑就等于清理永远是空操作。`prepare-bundled` 的实例身份在参照物里是**可选**的（带了就给实例目录，不带就只给预留的会话根），Linux 原先把它当必填，等于拒收参照物允许的命令；现在按参照物对齐，畸形身份仍然在授权前就被拒，`CliGrantTests` 钉住这两种形态（变异探针：改回必填立刻打红）。少给一棵树，命令会在沙箱里撞死在只读文件系统上（变异验证报的就是 `OSError: Read-only file system: instances`）；多给一棵，一个只想存自己设置的命令就能写光整个固件库。`tests/test_cli_grants.py` 把两份表都从各自的生产代码里解析出来逐命令比对，参照物只允许 `instance-delete` 这一个已知差集（macOS 的 switch 里根本没有该命令，它自己的调用点会报「未授权的静态命令」）。`AZKI_APP_ROOT` 指向解包后的发布件时，`InstanceSettingsCliTests` 会真跑一次 bwrap + 捆绑 Python 的往返；CI 用 `--filter "Category!=Integration"` 把它排除（容器 job 建不了 user namespace），而没有发布件时它会明说失败，不会悄悄「通过」。代理记录（native-broker 的 config.json/state.json）用 `renameat2(RENAME_NOREPLACE)` 发布：内置 helper 读这些记录时硬要求 `st_nlink == 1`，用「硬链接再删临时文件」的写法会留出一个它自己每 25 ms 轮询就能撞上的窗口；`tests/test_native_broker_wire.py` 再从 C# 的发布点里解析出记录形状，交给 Python 真正的校验器判定，桌面测试里还有一条让 helper 真发一份 launch.json、由 C# 读回（反向亦然）的跨语言往返。C# 侧读请求也走同一套规则：限定目录句柄、拒绝符号链接、限定大小、读前读后属性必须一致。两条只有跑起来才会暴露的跨语言契约另有专测：`logs` 的增量游标由 `linux/tests/…/LogStreamCursorTests.cs` 把桌面端真正构造的请求交给 Python 的 `log_stream.read_chunk` 判定——空对象 `{}` 不是「没有游标」，helper 会直接拒绝，于是每一次轮询都失败、日志面板整场会话都不出字；游标前进规则（缺文件回到无游标、旧版桥报错即停、越界偏移拒绝）与 macOS 的 `SimulatorLogStream` 对齐。装包暂存 `Core/PackageStaging.cs` 照参照物 `SimulatorPackageImport` 的写法来：源文件必须是不超过 32 MiB 的普通文件（属主是自己或 root、`st_nlink == 1`、无组/其他写位、任何一级路径都不许是符号链接），`incoming` 以 0700 建——且是**对已验证的目录句柄用 `mkdirat`** 建的（参照物 `SimulatorPackageImport.swift:48` 就是 `mkdirat(root, "incoming", 0o700)`；Linux 移植原先按路径建，等于把名字又解析了一遍，路径若被换掉就会建到别处）、暂存名以 `O_CREAT|O_EXCL` 0600 独占创建，边流式拷贝边算 SHA-256（不整包进内存、也不二次读盘），拷完复查源文件身份、目录身份与暂存名仍是同一个 inode，失败时只删自己那个 inode——绝不用别人的文件换掉再删。`PackageStagingTests` 逐条钉住这些性质（含「换名后不许删掉冒名文件」）。触摸侧 `TouchContactTracker` 复刻 macOS 画布的四态机：屏外按下不算接触、拖出屏幕立即抬手、屏外松开仍在记住的那一点抬手，否则 guest 会保着一根永远抬不起来的手指；性质测试用五千段随机指针序列只断言一件事——不会以按下收尾。录屏链路也不靠「装了 ffmpeg 就应该能录」：`RecordingTests` 直接喂 12 帧 rgb565le 给真 ffmpeg，再用 `ffprobe` 断言产物是 h264 且 `nb_frames == 12`（帧数在裸管道里丢了就会红），另有用例覆盖缺 ffmpeg、奇数尺寸、并发 stop 与产物 0600。「停实例后进程组无残留」这条以前只能靠真固件的端到端链验证；判据现在抽成 `Core/ProcessGroupSurvivors.cs`（直接读 /proc，不用 `Process.GetProcesses`+getpgid：别人的 pid 会回 EPERM，共享机器上会一直报「unconfirmed」），`ProcessGroupSurvivorTests` 用真的进程组验证：只杀组长而成员还活着时必须报出幸存者，整个组清空后必须不再报（变异探针把扫描改成恒返回 false 立刻打红）。实例管理器的「新建实例」这条路径现在也有测试（`ManagerViewModelTests`）：记录先落盘再进列表，所以存储被拒时列表不会留下幽灵实例；实例在下次打开窗口时仍在（`AZKI_DATA_ROOT` 指到临时根，该类跑在 `ProcessEnvironment` 集合里，不与别的改环境变量的用例并行）。变异探针把「先显示再落盘」改回去立刻打红。顺带撞出并发刷新的一条：窗口自己会起一次刷新，用户还能在它没跑完时再点一次，两次刷新都看到空列表、都会把同一条实例插进去，列表里就出现两个一模一样的实例——实测在整轮单测里出现过两次。现在刷新用信号量串起来，检查和插入成了一步。**但这条并发用例抓不住「去掉串行化」的变异**：安静机器上 300 轮压力探针 0 次命中（窗口太窄，要 CPU 抢占才撞上），所以它只当不变式检查，不当修复的证明——这一点如实记着。

画布坐标映射此前没有测试，补上时撞出一个真漏项：`CanvasGeometry` 是 macOS `SimulatorCanvasGeometry` 的移植，映射公式（letterbox 居中、右下边界开区间、末端像素夹取）现在由 `CanvasGeometryTests` 钉住，并从 Swift 源码里读出参照公式逐字比对。参照物的守卫同时检查**原点**是否有限，Linux 移植只查了宽高，于是原点是 NaN 的画布会算出 `NaN, 0, 335.8, 400` 的矩形；NaN 的比较一律为 false，越界检查全部落空，窗口坐标 (10,10) 的触摸被送到设备的 **(0, 13)**——屏幕最左边一列，而不是像参照物那样被拒掉。实测探针跑的是旧算式，不是推断：`.NET` 把 NaN 饱和成 0，所以 x 落在 0 而不是负数。现在按参照物补上原点检查，探针对着**发布件**复验：新包拒绝、旧包不拒。桥接命令白名单同样改成对着参照物逐项比对（`BridgeCommandAllowlistTests` 解析 `SimulatorSessionConnection.swift` 的 permitted 列表加上 `SimulatorLiveCommands`），少一个动词、多一个动词都会打红——这些命令（PMU ADC、装包、外部桥）被拒时没有任何现场痕迹，只表现为面板上的滑块不动。`BridgeInputGuardTests` 钉住门前那一关：越界的触摸坐标在连 socket 之前就被拒（`触摸坐标超出设备画面`），未授权命令在建立连接之前就被拒（`不受支持的会话命令`），会话目录不属于当前用户或对其他用户可写时构造直接失败。共享运行时里另两条平台分支也补了测试：`quarantine` 在非 macOS 上必须是**空操作**（文件内容、mtime、权限都不许变），`--window` 在 Linux 上必须直接报错指向宿主控制面板而不是开出第二个渲染后端——两者都写在 `tests/test_platform_branches.py` 里。

会话清理在 Linux 上比 macOS 多一段：bwrap 只能按目录绑定授权，被绑进来的过期会话根在沙箱里是**挂载点**，而挂载点永远 rmdir 不掉（真跑一次的回执就是 `remove-failed: Device or resource busy`）。把整个 `/tmp` 交给沙箱写当然能删掉，但那等于允许这条命令 unlink 本用户在 `/tmp` 下的任何自有条目，比参照物「按名字前缀授权」的规则宽得多，所以不做。分工是：沙箱里的 Python 按原有判据（进程/套接字活性、10 分钟宽限、实例事务未结算、非本前缀不碰、绝不跟随符号链接）清空目录并给出回执，宿主侧的 `Core/SessionCleanup.cs` 只对**回执点名 remove-failed、且在授权清单里、且确实空了**的路径补最后一步 rmdir——「刚创建还是空的、正在准备的会话」因此不在射程内。判据本身由 `tests/test_sessions_cleanup.py` 钉住（真 fork 一个进程拿它的 pid 当活性证据、真建目录树），`SessionCleanupCliTests` 再用打包件 + 真 bwrap 跑完整链，`SessionCleanupTests` 逐条验证宿主侧的拒绝。变异探针结果如实记录：删掉「回执判定/授权清单/名字前缀/节点类型/父目录」任一守卫都会被测出来，唯独「目录为空」这条守卫删掉后测试仍过——因为 `Directory.Delete` 本身就不肯删非空目录，该守卫是表达意图而非承担安全。

删除实例数据在 Linux 上也有同一个坑：命令要删掉实例目录**本身**，而 bwrap 绑进来的目录是挂载点，unlink 是 EBUSY、父目录只读时连 rmdir 都是 EROFS（两种都对着真 bwrap 验过）。参照物在这条命令上**根本没有授权**——macOS 的 `makeInspection` 没有 `instance-delete` 分支，Swift 调用点只会报「命令被拒绝」，实例数据留在原地。Linux 的做法是给容器 `instances/` 写权限、把每个兄弟实例目录在可写绑定**之后**用只读重绑盖住：同一条「挂载点删不掉」的性质，这次用来挡住更宽的授权。`CliGrantTests` 钉住授权形状与绑定顺序，`tests/test_cli_grants.py` 记着这仍与参照物不同（Linux 多出 `instances` 一项），`InstanceDeleteCliTests` 用真 bwrap 跑完整链并验证兄弟实例删不掉。变异探针把「去掉兄弟只读重绑 / 改成授权实例目录本身 / 把目标也保护起来 / 把只读重绑排到可写绑定之前 / 干脆不发只读重绑」五种改法全部打红。

命令本身还有一处与顺序有关的坑：沙箱里的进程继承着 stdout/stderr 管道，而 .NET 的进程对象在回收时会关掉它们——先等进程退出再读管道，管道就可能在载荷还在写的时候被关掉，载荷随之被杀。`Core/InspectionRunner.cs` 因此改成先抽干两条管道再等退出（.NET 对重定向输出的建议顺序也是这个），死线仍同时覆盖两段等待。机器内存吃紧时（这台机器上别的构建在跑，内核已经连杀两次 soong_build 触发全局 OOM），沙箱可能整段被信号杀掉：回执才是契约，回执在就算命令做完了；没有回执时错误信息会说清是被信号几终止的。

端到端验收写成了一条不许悄悄缩水的链，`FirmwareBootTests`（`Category=Integration`）：导入 → 只挑目录真正报 `availability.ready` 的 profile → prepare → start → 画面序号必须推进（`FrameReader` 自己跳过已交出的序号，所以第二条就是真·重绘）→ 屏中心按下再抬起 → 增量日志游标被接受并能续读 → 截图核 PNG 魔数、sha256 与宽高 → stop，之后只允许**本条测试自己起过**的 qemu-system-arm 消失（别人正在跑的会话不算残留）。仓库不含固件，因此两个环境变量缺一它就明确失败：

```sh
AZKI_APP_ROOT=/path/to/azki-simulator AZKI_FIRMWARE=/path/to/ota.zip \
  dotnet test linux/AzkiSimulator.Linux.sln --filter "Category=Integration"
```

同一条链也做成了入口脚本 `scripts/verify-linux-boot.sh`：它不复制验收逻辑，只是把契约立起来——检查解包件的结构（捆绑解释器、manifest、App 程序集齐全）、固件存在、`bwrap` 在 PATH 上且真能起（起不来多半是内核关了 unprivileged user namespace，这时报的是环境问题而不是固件问题），然后跑上面那条命令，最后在 shell 层再核对一次**本次启动的** qemu-system-arm 没有残留。实测（拿一份非固件文件冒充 OTA）脚本按预期失败在导入那一步，退出码 1，其余四个集成用例通过：

```sh
AZKI_FIRMWARE=/path/to/ota.zip scripts/verify-linux-boot.sh /path/to/azki-simulator
```

多机型 OTA 在未用 `--model` 绑定时，CLI 会把选择留在 `needsSelection` 里而不是给你一个 profile，这条链会直接把候选 ID 报出来。本机没有授权固件样本，所以**真固件启动从未在这里跑通过**；已经证实的是前半条通路：拿一份非固件文件喂进去，命令确实穿过 bwrap、捆绑解释器与命令白名单撞到 Python 真正的目录校验器，并原样报回「请导入完整 OTA 固件包……」——打包件、沙箱、写权限表、CLI 往返都是活的。出帧、跟手、截图那半段后来用一份**合成的可启动 guest** 走通了（见下文），但那一半证明的是宿主侧，不是真固件的行为。

阶梯的第一站「导入」是唯一不需要真固件就能验的一步：探针只做结构清点、不做真伪判断，合成 OTA 与真包同形。`tests/test_firmware_import.py`（10 例）把导入契约钉住——完整 ZIP/BES 被识别、唯一的 `vela_ap.bin` 抽成私有库对象、AP 声明的机型决定这条固件在哪些预设下可见、非完整 OTA 被带理由地拒绝；`FirmwareImportCliTests` 再把同一条路穿过真 bwrap 与打包 CLI 跑一遍（导入 → 目录里出现 `availability.ready` 的条目 → 删除后清空）。

写这两个测试时撞出一个真缺陷：AP 声明的机型若不在内置目录里（`miwear.watch.q66nfc` 就是一个例子——运行时自己的 Q66 profile 用的是这个名字，而目录里写的是 `q66`），候选会回退到全部 17 个预设，**条目却是空的**：回执自相矛盾，macOS 的解码器会以「导入结果没有明确对应的固件型号」直接拒收整次导入，Linux 则导入成功但目录里空空如也。`firmware_catalog.py` 现在让条目跟随候选（声明的机型一个都对不上时，与「AP 根本没声明机型」同样回退到全部预设），回执因此始终满足它自己写下的契约。改的是共享运行时，两个平台都受益；语义只动了这一处。对照实验：对修复前的打包件跑导入集成用例，如实报「导入回执里的候选没有对应的条目」；把修复后的运行时放回同一打包件再跑，通过。

再往后一步「准备」也用合成固件走通了。合成 AP 是按探针读的形状造的（芯片行、分区偏移、复位序列与落在已建模 RAM 里的栈指针），结构合法但**不是固件**：真正拦住的是实例存储那一步——它要读固件里的 NAND 分区表，合成镜像给不出，所以带实例身份的准备在这里必然报 `Original NAND data partition was not found`；而参照物允许的**不带**实例身份的准备穿过真 bwrap 跑通：落地引擎、写好配方、开出会话描述与后端计划（`FirmwareImportCliTests` 第二例）。到这一步，缺的只剩一个「真能在模拟器里跑起来的载荷」。

`linux/tests/…/SyntheticGuest.cs` 把那个载荷造了出来：一段 230 字节的 Cortex-M33 Thumb-2 机器码，从 ARMv8-M Mainline 的 llvm-mc 列表抄下，碰的地址全部取自模拟器源码（GPIO0/1/2 = 0x40081000/0x40089000/0x4008a000、LCDC = 0x40200000、PL011 UART0 = 0x4000b000、SRAM 里的帧缓冲 0x20010000），流程是给面板上电并选中、写 LCDC 几何（0x104/0x108/0x118 尺寸、0xfc pitch、0xf4 地址、0x100 原点、0x210 bit29 发生器、0x264 格式）、然后循环：画一帧 212×520 RGB565、清 0x1c4 的 bit27、置 0x224 启动传输、等 0x224 落下、每帧往 UART 吐一个字节。外面裹上探针读的 build_info 与 116 字节 MTD 记录，就是 `vela_ap.bin` + `vela_resource.bin`（`-rom1fs-` 魔数）+ `version.ini` 的完整 OTA。分区表不是随手写的：模拟器的 `bes_nor_layout_init` 只认目录里那 8 组精确几何（flash/ap/tail/factory/config 逐项相等），合成的 `OTA_CODE_OFFSET` 与 mode/facmode/nv 三条记录必须凑出 Q66 那一行（ap 0xc0000、tail 0xfb8000、factory 0xfc0000，且三条连续、末尾正好落在 flash 边界），否则 QEMU 以 `NOR writeback requires an exact catalog geometry…` 直接退出。阶梯本体也抽成一处 `BootLadder.cs`，让「授权固件」与「合成 guest」两条测试不可能各自缩水，`SyntheticGuestBootTests` 跑的就是同一条链。

这条合成链跑通之前，Linux 桌面端**没有真的启动过一个会话**，而它一跑就撞出七个只有跑起来才会存在的缺陷，每个都配了回归测试：

- 沙箱里看不到监督脚本：客户沙箱只绑了 Engine、项目与固件库，argv 指的 `Runtime/azki_simulator/minative_launch.py` 从没被绑进来，解释器报的是 `can't open file …`（ENOENT）。现在按参照物的形状把这一个**文件**只读绑入（参照物把它当清单固定的文件，而不是整棵 Runtime 目录），并实测确认 bwrap 会为单文件绑定自建缺失的父目录。
- `SO_PEERCRED` 在 Linux 是 17 而不是 BSD 的 `LOCAL_PEERCRED` 暗示的 16：16 是 `SO_BSDCOMPAT`，getsockopt **成功**并交回四个零字节，于是凭据检查一路失败却不报任何错（`PeerCredentialTests` 用真 unix socket 对端断言 pid/uid，把常量改回 16 立刻打红并报 `SO_PEERCRED unavailable: 0`）。
- `DataLocations.EnsurePrivateDirectory` 只收紧它自己那一级，一路创建的祖先保持 umask 的 0755（`PrivateDirectoryTests` 三条：祖先链、已有目录、已存在不越权）。
- helper 与客户沙箱都没绑私有固件库，而 `start` 会复验计划固定的每一条路径——导入包的 AP/OTA 对象正住在那棵树里，于是会话在第一条命令上就死。
- **bwrap 是 fork 而不是 exec**，于是有两处身份关系与参照物不同。其一，监督脚本原本用 `os.getppid() != parent` 断言宿主所有权，而客户进程的直接父进程是 bwrap，每次启动都被拒成 `Invalid native parent`；`minative_launch.native_parent_is` 现在按平台判定：macOS 仍是直接父子，Linux 要求中间那一环确实是 bwrap 且它的 `PPid` 仍是通告的 pid——同一条判断兼作活性检查（宿主一没，bwrap 被 reparent，等式立刻不成立）。`tests/test_platform_branches.py` 用真 bwrap 起沙箱跑这条判断：通告自己 → owner、通告别人的 pid → stranger。其二，会话进程组属于监督进程（`--new-session` 让 bwrap 的子进程在 exec 前就 setsid，载荷里再调 `setsid()` 得到的是 EPERM），而 .NET 拿到的 pid 是 **bwrap**：原先发布的 pid、`killpg` 的目标、「组内无残留」的判据都落在 bwrap 的组上，而那组是空的；实测把 SIGTERM 发给 bwrap，监督进程被 `--die-with-parent` 以 SIGKILL 收掉、跳过它有界的优雅停止与存储回执，孙进程（QEMU 的位置）活着留在一个已死的组里。现在 `ProcessGroupSurvivors.GroupLeadingChild` 从 /proc 取 bwrap 唯一那个自组领导的子进程（实测 t+29 ms 出现且之后稳定，多于一个就拒绝），发布的 pid、TERM/KILL 的目标与 `cleanup` 判据都跟着它；`PublishTerminal` 也照参照物以「组是否收回」决定 exited/failed，载荷自己的状态只作为 `exitCode` 随行（实测 bwrap 以载荷状态退出：0→0、3→3）。`ProcessGroupSurvivorTests` 用真进程组钉住这三种图景：找得到自组领导的子进程、绝不算「继承了启动器组的子进程」、两个自组领导时拒绝。
- 会话日志按 umask 建成 0644，而 helper 检查运行输出「必须私有且无符号链接」，于是整场会话在第一条命令之后就断（`Run output must remain private and free of symbolic links`）；现在两条日志都以 `O_CREAT|O_EXCL` 0600 独占创建，与参照物的 0o600 一致。
- 客户 argv 少了虚拟对端与实例设置两组参数：计划里 `virtualPeerEnabled` 为真时 runner 报出的身份缺 `virtual_peer_state_sha256`，helper 以 `Runner identity differs` 拒绝；`--instance-settings` 同理，而 `--skip-oobe` 原先写死、现在回读计划里的 `skipOOBE`。argv 形状由监督脚本自己校验（长度只能落在 8/16/20/21/26/27，三个实例文件必须正好是会话私有那三个路径），所以缺参数会在启动时硬炸而不是静默降级。
- 早期失败会把原因盖掉：代理在还没读到 launch.json 之前就抛错时，终态记录的 `id` 是空串，helper 把它读成「Native status does not match this launch」的协议错误，真失败被一句固定话术替换。现在 `id` 用自己封进 config.json 的那个会话身份（参照物用的正是 `launch.configuration.id`），launch.json 也连同 `stop.json` 一起按参照物的字段集与身份校验：通告的 id 必须等于封存的配置 id，字段集必须正好是 `id/run/version`。

`tests/test_native_broker_wire.py` 把 C# 发布点里解析出的每条记录交给 Python 真正的校验器判定，标识符改名后它的 witness 表跟着换（`_sessionId`/`_guestGroup`）——它同时也是这几处记录形状没被改坏的守门人。`tests/test_ci_workflow.py` 里「会起 bwrap 的测试必须带 Integration 分类」那条检查原本逐文件判定，把阶梯抽成共享助手 `BootLadder.cs` 之后它报红了：助手自己没有测试方法，但两个调用点都带分类。检查现在按调用点跟进——不含测试方法的文件由它的调用者承担该分类，「起沙箱的链」因此不能靠搬家绕过容器 job 的过滤。

合成链证明的是宿主侧：真 bwrap、真监督脚本、runner 从 AP 推出的板级几何、helper 镜像出的共享画面文件、桥接、截图，以及 stop 之后没有模拟器残留；它对真实固件的行为**什么都不证明**——那份证据仍属于 `FirmwareBootTests`，而本机没有授权固件字节。对着解包发布件连跑两次（6 秒、5 秒）均通过，并且用旁路探针独立抓到了产物：`<会话>/frame.raw` 220512 字节 = 32 字节头 + 212×520×2，序号 46（偶数即稳定写入）、110240 像素里有 55089 种不同颜色，截图 PNG 的 IHDR 就是 212×520。

## GitHub Actions

`.github/workflows/build-linux.yml` 在 Arch Linux 容器内构建引擎与 Linux 包并跑同一套 Python 测试；容器无法使用 bwrap，完整 boot 验收由 `boot` job 交给自托管 runner：它只认带 `azki-boot` 标签的 self-hosted 机器，要仓库变量 `AZKI_BOOT_RUNNER=true` 且手动触发才跑，固件不经过 workflow 传递，secret `AZKI_FIRMWARE_PATH` 只写机器上那份授权 OTA 的路径。发布模式与 macOS 工作流一致：草稿 Release、按“起点 + 运行序号 − 1”递增构建号、附校验和，只需要 `contents: write`。构建号会先写回 `BUILD_NUMBER` 文件再打包，所以包文件名（`Azki-Simulator-<版本>-build<号>-release-linux-<架构>.tar.gz`）与 Release 标签（`v<版本>-build<号>`）说的是同一个构建；容器里的 `dotnet test` 按 `Category!=Integration` 排除需要 bwrap 的用例，`tests/test_ci_workflow.py` 会盯着这几处约定不被改坏。
