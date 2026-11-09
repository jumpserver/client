# Shell 构建入口

从项目目录运行 `./build.sh --help`。主入口依次配置 CMake、构建 GUI/Go helper、部署依赖和生成安装包；复用现有 Python 打包校验与签名逻辑。Bash 最低 3.2，Python 最低 3.8，还需对应 Qt、C++ 编译器、Go、CMake 和各平台打包工具。

默认使用当前系统和 CPU、Qt 5、4 个并行任务。输出写入新的 `dist/<系统>-<架构>-<时间>/`，拒绝覆盖已有目录；构建缓存按平台/架构/Qt 版本隔离。相对路径以调用脚本时的工作目录为准；从其他目录也可使用脚本绝对路径运行。

```sh
# Apple Silicon：Qt 和 Go 均应满足 macOS 11 基线。
./build.sh --platform macos --arch arm64 \
  --qt-prefix /path/to/qt5-arm64 --go /path/to/go1.21.13/bin/go

# ARM 主机交叉构建 Intel：Qt 库为 Intel，构建工具为原生 ARM。
./build.sh --platform macos --arch x86_64 \
  --qt-prefix /path/to/qt5-intel --qt-host-bins /path/to/qt5-arm64/bin \
  --go /path/to/go1.21.13/bin/go

# Windows：在对应 MSVC 开发环境中启动 Git Bash；EXE/MSI 同时生成。
PYTHON=python ./build.sh --platform windows --arch amd64 \
  --qt-prefix C:/Qt/5.15/msvc2019_64 --format both --generator Ninja

# Debian/UOS deb 系：在对应系统、对应 CPU 上执行。
./build.sh --platform linux --arch aarch64 --format deb --go /path/to/go

# RPM 系麒麟：在对应发行版/ABI 的机器或容器执行。
./build.sh --platform linux --arch loong64 --format rpm --go /path/to/vendor-go
```

macOS 仅接受 amd64/x86_64 与 arm64，默认最低目标分别为 10.15 和 11.0。交叉构建自动关闭目标程序 smoke，并在包清单记录；原生构建默认执行 smoke。`--qt-prefix` 必须指向匹配架构、实际满足系统下限的 Qt，打包器会逐个检查 Mach-O；不会自动选用 Homebrew Qt。可加 `--build-qt` 先调用固定源码 Qt 5 构建脚本；下载、校验、配置及缓存规则见 [兼容构建说明](macos-compat.md)。Qt 6 使用已安装的匹配前缀及 `--qt-major 6`，它自己的最低系统要求仍需单独满足。

示例中的 `/path/to/...` 是占位符，必须替换为本机实际路径。Qt 前缀应包含 `lib/cmake/Qt5/Qt5Config.cmake` 和 `bin/macdeployqt`；`--go` 可省略以使用 PATH 中的 Go。不要直接使用未经检查的本机 Qt 打旧系统兼容包。

Windows 格式可选 `exe`、`msi`、`both`，架构为 amd64/386。默认 Qt 使用原生 Schannel；使用 OpenSSL 的 Qt 应传 `--openssl-dir`。编译器/CRT、WiX、NSIS 等要求见 [Windows 打包](windows.md)。Git Bash 需要从已配置的 MSVC 开发环境启动；不在 macOS/Linux 上生成 Windows 安装器。

Linux 格式为 deb/rpm，架构支持 amd64、arm64、386、arm、loong64、mips64le、riscv64 及常见别名。脚本校验目标 CPU 与宿主一致，使用目标发行版的 Qt/libsecret 和包工具。不同 ABI、龙芯新旧世界和 Go 内核基线仍需匹配；能传入架构参数不代表已完成该系统认证，详见 [兼容性矩阵](../docs/compatibility.md)。一个新系统的包不会被自动标为兼容所有 UOS/麒麟版本。

常用选项：

| 参数 | 用途 |
| --- | --- |
| `--output DIR` / `--build-dir DIR` | 指定新交付目录、可复用构建缓存 |
| `--version X.Y.Z` | 同时设置 GUI/helper/安装器版本，默认取 CMake 项目版本 |
| `--jobs N` / `--generator NAME` | 构建并行度与 CMake generator；Linux generator 由下层脚本管理 |
| `--cmake-arg KEY=VALUE` | 重复传入工具链/编译器等 CMake 配置；不要覆盖专用参数已设置的目标信息 |
| `--dry-run` | 仅打印经过 Shell 转义的命令，不构建、不创建目录、不读取签名证书 |
| `--sign` / `--notarize` | 请求签名 / macOS 公证，公证隐含签名 |

macOS `--sign` 优先使用已有 `APPLE_SIGNING_IDENTITY`/keychain；未设置时复用 `setup-apple-cert.py` 导入同名 P12 环境凭据，退出时清理临时钥匙串。Windows 复用已有 PFX 环境凭据。缺证书跳过签名并生成未签名包，提供了无效证书则真实报错；Linux 包签名仍由发行仓库单独处理。凭据变量及公证要求见 [安装包签名说明](README.md#ci-与签名)，不要把凭据写入脚本或命令参数。

本机通过此主入口实际生成 ARM/Intel 两份 DMG，并完成原生 ARM 的挂载启动检查。Windows/Linux 分支完成 CLI 分发检查，完整安装包仍需在相应系统验证。
