# 系统与架构兼容

目标按尽可能广的覆盖规划；“目标”不等于已经实机认证。Qt GUI、Go 启动器、第三方客户端、系统 ABI 和安装包必须同时满足目标环境。

## 构建矩阵

| 平台 | 规划架构 | 构建方向与边界 |
| --- | --- | --- |
| Windows 7 / 8.1 | x86、x86_64 | Qt 5.15 兼容工具链 + 单独旧版 Go 工具链；与现代包分开产出 |
| Windows 10 / 11 | x86_64；有需求时 x86 | Qt 5.15，后续可切 Qt 6；Qt/Go/第三方应用仍需版本匹配 |
| Windows ARM64 | 优先评估 x64/x86 兼容运行 | 原 AutoIt/PuTTY 资源未提供完整原生 ARM64 链路，暂不宣称原生支持 |
| macOS Intel | x86_64 | 最低目标 10.15；已产出独立源码 Qt/Go 构建包，旧系统实机待验收 |
| macOS Apple Silicon | arm64 | 最低目标 11.0；已产出独立源码 Qt/Go 构建包，本机启动通过，旧系统实机待验收 |
| 通用 Linux | x86_64、aarch64、i686、ARMv7、RISC-V64 | Qt Widgets/X11 优先，Wayland 单独验收；最低 glibc/libstdc++ 在旧 sysroot 中构建 |
| 统信 UOS / 银河麒麟 / 中标麒麟 | x86_64、aarch64、LoongArch64、MIPS64EL | 每个发行版系列及架构独立工具链与包；LoongArch Qt 5 需对应发行版移植版 |

麒麟和 UOS 的不同版本/CPU 组合不共享单一 ABI。旧龙芯 MIPS64EL 与 LoongArch64 也不是同一个目标。禁止将一个新系统编译的二进制直接标为“全 Linux 兼容”。

申威 `sw_64` 等缺少上游 Go/Qt 完整工具链的架构不在当前已实现目标内，需要厂商工具链及单独移植；Windows ARM64 也不能用 Go 能编译来代替第三方自动化组件支持。ARM 默认 `GOARM=7`，脚本支持 `--goarm` 显式选择指令集下限。

LoongArch 启动器通过独立 `scripts/go-loong64.mod/.sum` 将 `x/sys` 提升至 v0.6.0，以补齐旧依赖缺失的 syscall 定义；原 `go-client/go.mod` 不变，其他架构不使用该覆盖。

## 国产系统专项检查（2026-11-07）

结论：已有多架构构建基础，但尚未完成 UOS / 麒麟发行版认证，且当前 Linux 外部客户端启动链有已复现的兼容缺口。上方矩阵描述规划目标，不能作为已支持清单。

| 架构 | 本次确认 | 尚缺验证或适配 |
| --- | --- | --- |
| x86_64（含采用此架构的国产 CPU） | Go 1.21.13 两个 helper 均交叉编译通过，静态 ELF 架构正确 | 对应 UOS / 麒麟版本的 GUI、桌面和外部客户端实机验证 |
| ARM64 / aarch64 | 同上 | 发行版 Qt/ABI、桌面与外部客户端验证；既有 Debian/Rocky 容器记录不能替代国产系统验收 |
| 龙芯 LoongArch64 | Go 1.21.13 加现有 x/sys 覆盖后两个 helper 编译通过 | 必须区分新旧世界 ABI；Qt 移植版本、厂商 Go、内核和运行库需匹配 |
| 旧龙芯 MIPS64EL | Go 1.21.13 两个 helper 编译通过 | Qt GUI 未在该目标构建/运行，CPU 指令集及旧内核仍待验收 |
| RISC-V64 / ARMv7 | 构建入口存在，历史记录有 helper 编译结果 | 本次未重跑，GUI 与系统实机未验收 |
| 申威 sw_64 | 当前构建脚本不接受该架构 | 需要厂商工具链及专项移植，不能标为支持 |

本次交叉编译输出位于 `/tmp/osm-domestic-audit/`，仅证明 Go helper 可编译，不是完整 GUI 安装包。36 项打包脚本回归通过。没有重新执行历史 Debian/Rocky GUI 构建，也没有 UOS、麒麟或龙芯实机运行结果。

已确认的缺口：

- `go-client/pkg/awaken/awaken_linux.go` 的内置 SSH 桌面选择使用完整字符串匹配，`DDE`、`Deepin:GNOME` 返回空命令；`UKUI` 固定选择 `gnome-terminal`，没有检查并回退到已安装终端。数据库终端也存在同类逻辑。不能假设所有 UOS/麒麟桌面均设置相同标识或安装 GNOME 终端。
- 同文件的 RDP 使用 `AppItem.Name` 启动而非 `AppItem.Path`，因此设置中的自定义客户端路径不起作用。这也影响以 RDP 拉起的远程应用。
- 上述 SSH/RDP 问题已在复制到临时目录的源代码上编译测试程序，并在无网络的 Debian arm64 一次性容器中复现：DDE/组合标识返回 nil，缺少 GNOME 终端时 UKUI 启动失败，RDP 忽略可执行的配置路径。这里是缺陷复现通过，不能解读为系统兼容测试通过。未改动业务实现。
- `go-client/config.json` 的 Linux TigerVNC 下载链接固定为 x86_64，不能用于 ARM/龙芯；Linux `filetransfer` 默认目录为空，SFTP 不能据此宣称开箱可用。
- CMake 要求 Qt >= 5.15；目标系统仅有更旧 Qt 时不能直接安装使用当前动态链接包，需提供匹配 ABI 的 Qt 5.15。RPM 依赖名称按现有构建发行版固定，必须在各目标软件源验证；不能简单将 DEB/RPM 后缀视为发行版兼容。
- `.github/workflows/build.yml` 的 Linux 作业只构建 amd64 Debian/Rocky 包，没有 UOS、麒麟、ARM 或龙芯作业。打包脚本检查 ELF machine/端序，但没有检测 LoongArch 新旧世界 ABI 或自动限定 GLIBC/GLIBCXX 上限。
- 直接 CMake 原生构建时 `mips64` 未自动映射到 `mips64le`；`package-linux.py` 已处理小端 `mips64` 并显式传入架构，直接构建需指定 `CLIENT_GOARCH=mips64le`。

龙芯关键边界：上游 Go 的 LoongArch 实现采用 LP64D，初始支持要求 Linux 5.19；官方明确指出历史商业发行版的旧 syscall ABI 即使静态链接也不兼容，应使用发行版提供的 Go。不能仅凭 `uname -m=loongarch64` 或静态 ELF 判定可运行，厂商回移植内核还需具体确认。参见 [Go 官方 LoongArch 说明](https://go.dev/doc/go1.19#loong64) 和 [AOSC 新旧世界兼容层说明](https://aosc.io/liblol)。

交付前应按“系统版本/补丁 + 架构/ABI + Qt/Go 工具链”分别产包，在目标桌面验证安装升级卸载、`jms://`、OAuth/凭据恢复、SSH/RDP/VNC/远程应用。推荐先覆盖 x86_64 / ARM64 的 UOS 与麒麟，再分别落地 LoongArch 新世界、旧世界和 MIPS64EL。

## Go 工具链基线

原项目 `go.mod` 的 `go 1.17` 是语言/模块约定，不是编译后运行系统的保证。Go 1.21 起要求 Windows 10 或 Windows Server 2016；覆盖 Windows 7/8.1 需要独立旧工具链构建策略，见 [Go 1.21 官方发布说明](https://go.dev/doc/go1.21#ports)。旧工具链的维护与补丁策略需随产品支持周期管理。

```sh
cmake -S . -B build-win-legacy -DCLIENT_QT_MAJOR=5 \
  -DCMAKE_PREFIX_PATH=C:/Qt/5.15.2/msvc2019_64 \
  -DGO_EXECUTABLE=C:/toolchains/go1.20/bin/go.exe \
  -DCLIENT_GOOS=windows -DCLIENT_GOARCH=amd64
```

此示例表达工具链组合方式，不表示该组合已经在目标系统验收。不要只降低 `CMAKE_OSX_DEPLOYMENT_TARGET` 就声称旧 macOS 兼容：Qt 动态库、SDK API 和 Go 运行时也必须满足下限。

macOS 编译显式指定目标下限，并检查包内每个二进制的架构、依赖和最低系统版本。当前 `dist/current/` 两份 DMG 使用源码 Qt 5.15.19 和 Go 1.21.13，Intel/ARM 包内最高 minOS 分别为 10.15/11.0，不再包含 Homebrew 库。此前要求 macOS 27 的开发包已随历史产物移出交付目录；当前验证结果见 [验证记录](validation.md)。可复现命令及验证边界见 [macOS 兼容构建](../packaging/macos-compat.md)。

## Linux 打包

- 在选定最旧发行版/sysroot 上构建每种架构，检查 ELF 依赖的 GLIBC/GLIBCXX 版本。
- 包含 Qt 所需的 platform、imageformats、iconengines、TLS 插件；或明确依赖目标发行版对应运行库。
- Secret Service/libsecret 用于持久凭据；没有桌面 keyring 的环境只能保留当前内存会话。
- DEB/RPM 应注册 desktop、图标和 MIME，保持 Go helper 可执行权限；用户选择 `jms` 默认处理程序。
- X11/Wayland、中文输入、高 DPI、多显示器、托盘、系统浏览器、终端模拟器、外部 SSH/RDP 客户端分别验收。

## Qt 6 迁移

业务层和 UI 共用一套源码，CMake 显式选择 Qt 5 或 Qt 6，避免依赖 WebEngine、Qt 私有头文件和平台专用 UI 框架。Qt 6 的构建通过只证明源码兼容；Qt 6 包的系统下限仍由所选 Qt 6 版本和工具链决定。保留 Qt 5 旧系统发布线，不强迫旧用户使用 Qt 6 包。
