# Desktop packaging

## 快速入口

推荐使用项目根目录的 `./build.sh` 完成构建和打包，参数与多平台示例见 [Shell 主入口](build-entry.md)。以下 Python 命令保留为独立阶段入口。

所有安装包都包含 GUI 与原 Go 启动器，并保留 `jms://`。在目标系统或匹配发行版容器中执行；打包不会把一个平台的二进制转换成另一个平台。

```sh
# macOS：先按根 README 完成 Qt 构建，再部署运行库并生成拖拽安装盘。
python3 scripts/package-macos.py --build build-qt5 \
  --macdeployqt /path/to/Qt/bin/macdeployqt --output dist/OSMClient.app --deployment-target 11.0
python3 scripts/package-dmg.py --app dist/OSMClient.app \
  --output dist/OSMClient-4.1.2-macos-arm64.dmg --deployment-target 11.0

# Windows：在匹配 Qt/编译器的开发命令行中配置 CMake 后，构建并生成两种安装包。
python scripts/package-windows.py --build build-win --arch amd64 \
  --windeployqt C:/Qt/bin/windeployqt --format both --output dist --native-tls

# Debian 系和 RPM 系分别执行；不要在 Debian 中把同一 payload 转成 RPM。
python3 scripts/package-linux.py --format deb --build build-deb --output dist/deb
python3 scripts/package-linux.py --format rpm --build build-rpm --output dist/rpm
```

输出路径必须为新的包路径/目录，脚本拒绝覆盖现有安装包。macOS 可用 `--libpath` 指定需要一并部署的传递依赖；Windows 的 `--native-tls` 只用于确定已启用 Schannel 的 Qt，使用 OpenSSL 的 Qt 应改用 `--openssl-dir` 指向匹配版本的 TLS DLL。完整 Windows 参数见 [Windows 打包说明](windows.md)。

Linux 工具要求 Python ≥ 3.8、CMake ≥ 3.16、C++17 编译器、Qt Core/Gui/Widgets/Network/Svg、libsecret 开发包、`pkg-config`、`file`、`desktop-file-validate` 与 Go。DEB 另需 `dpkg-dev`；RPM 另需 `rpm-build`。可通过 `--helpers /path/to/bin` 注入同架构的预编译 Go 目录。Rocky 8 应使用 `python3.9` 等较新解释器，而非系统 Python 3.6。

Linux 检查 ELF 架构及可执行位、校验包内文件与 Qt/libsecret 依赖，并生成 metadata/manifest。macOS DMG 默认执行镜像校验、只读挂载、包内签名和依赖检查以及启动 smoke。Windows 执行 PE 架构、Qt 插件、运行库和隔离 smoke 检查；本机只有 macOS/Linux 环境，Windows 安装行为需要 Windows 验收。

统一版本使用 CMake 的 `-DCLIENT_VERSION=X.Y.Z`；Linux 脚本用 `--cmake-arg CLIENT_VERSION=X.Y.Z` 传入。各目标应独立使用构建目录。

应用显示名、快捷方式和 macOS 安装盘卷名统一为 `OSMClient`；安装包文件名使用 `OSMClient-<version>-<platform>-<arch>`。为保留升级、配置和协议关联，Linux 和 macOS 内部可执行文件、Linux 包名、桌面文件名、macOS bundle ID 不变；Windows 可执行文件名为 `client.exe`；Windows 默认安装到 `Program Files/OSMClient`，旧的用户级安装需先卸载。NSIS 升级调用旧卸载器清理其旧快捷方式；MSI 在安装新版本前卸载同一 UpgradeCode 的旧版本。

## CI 与签名

[GitHub Actions 工作流](../.github/workflows/build.yml) 使用目标平台构建，支持手动触发和标签构建，只上传 Actions artifacts；不自动发布 Release。[CI 说明](ci.md) 记录固定工具链、架构矩阵和触发规则。

macOS 复用原 `clients` 的同名 Secrets：

- `APPLE_CERTIFICATE`：Base64 编码的 Developer ID Application P12。
- `APPLE_CERTIFICATE_PASSWORD`、`KEYCHAIN_PASSWORD`：证书与临时钥匙串密码。
- `APPLE_ID`、`APPLE_PASSWORD`、`APPLE_TEAM_ID`：Apple ID、公证专用密码和团队 ID。

CI 先运行 `setup-apple-cert.py setup`，将证书导入独立的临时钥匙串并导出 `APPLE_SIGNING_IDENTITY`；不修改默认钥匙串。`package-macos.py --sign` 由内到外签署 Go、Qt 库及 GUI，启用 Hardened Runtime 和时间戳，Apple Events entitlement 仅用于 GUI 与自动化启动器。随后 `package-dmg.py --sign --notarize` 签署 DMG，提交 `notarytool`，仅在 Accepted 后装订并验证票据；最后 always 运行 `setup-apple-cert.py cleanup`。这是对应 Apple [命令行公证流程](https://developer.apple.com/documentation/security/customizing-the-notarization-workflow) 的 Qt 实现。

原 client 的 Windows 配置没有启用证书签名。Qt 新增可选 `WINDOWS_CERTIFICATE`（Base64 PFX）、`WINDOWS_CERTIFICATE_PASSWORD`，`package-windows.py --sign` 为自建程序与最终 EXE/MSI 做 Authenticode 签名。证书不能与 Apple P12 混用。即使请求签名，缺少证书或必要签名凭据时也会明确跳过并输出未签名包；macOS 无签名时同时跳过公证，只有公证凭据缺失时保留已签名但未公证的 DMG。已提供证书却发生密码错误、证书无效或签名执行失败，仍按真实错误报告。`TAURI_PRIVATE_KEY` 属于原 Tauri 更新器签名，并非操作系统代码签名，Qt 没有复用该密钥。

Linux 当前输出原生 DEB/RPM 和 SHA256 清单；原 client 没有配置 Linux GPG 包签名，这里也不会将未签名包标为已签名。若发行仓库需要 GPG 签名，应使用其独立的仓库/包签名密钥。

The GUI uses Qt Core/Gui/Widgets/Network. On macOS also link `Security` and `CoreFoundation`; on Windows link `Advapi32`. On Linux use pkg-config `libsecret-1` and define `CLIENT_QT_HAS_LIBSECRET` for OS credential persistence. If it is unavailable, credentials are not silently saved to a file.

The protocol helper is built from the vendored `go-client` source:

```sh
python3 scripts/build-launchers.py --output build/bin --go /path/to/go --goos darwin --goarch arm64
```

Use matching Go runtime/toolchain support for the required OS baseline. Building Qt for an old OS does not make a newer Go launcher compatible with it. CMake selects the target OS/architecture through `CLIENT_GOOS`/`CLIENT_GOARCH` and the toolchain through `GO_EXECUTABLE`; when building launchers directly, pass the target explicitly. Windows helper resources currently cover amd64/386; native Windows ARM64 is rejected because the vendored AutoIt/PuTTY dependencies have no native ARM64 build. All architectures need their own Qt and helper build.

Place the generated `bin` tree beside the executable, or under `Contents/Resources/bin` in a macOS bundle. The launcher resolves paths relative to the application, never the process working directory. `CLIENT_QT_RESOURCE_DIR` optionally permits a development resource root. Application defaults are embedded as `:/config.json`; existing `jumpserver-client/config.json` is shared with the vendored helper, preserved on version upgrade, and saved atomically. Corrupt existing configuration is reported without replacement.

## macOS

For a local self-contained development app (no installation, upload or publication):

```sh
python3 scripts/package-macos.py --build build-qt5 --macdeployqt /path/to/Qt/5.15/bin/macdeployqt --output dist/OSMClient.app --deployment-target 11.0
```

The destination must not exist. This copies Qt frameworks/plugins, applies an ad-hoc signature by default, verifies bundle signatures and runs the isolated smoke test. Add `--sign` for Developer ID signing, then use `package-dmg.py --sign --notarize` for the signed/notarized disk image.

兼容发布包使用独立源码构建的 Qt 与 Go 1.21.13，禁止混入 Homebrew 动态库。两个打包阶段均必须传 `--deployment-target`：Intel 为 `10.15`，Apple Silicon 为 `11.0`。每个 Mach-O 的架构和最低系统版本都会检查，缺少版本信息或高于目标立即失败；同时写入应用的 `LSMinimumSystemVersion`。跨架构构建见 [macOS 兼容构建](macos-compat.md)。

Use `Info.plist.in` for `MACOSX_BUNDLE_INFO_PLIST`; it declares `jms://` and the Apple Events usage description needed to automate Terminal/iTerm. Deploy the Qt frameworks/plugins with the matching Qt installation's `macdeployqt`. Sign nested Go binaries/frameworks before the outer app, with hardened runtime, then notarize. Use `entitlements.macos.plist` for the app and protocol launcher that drives terminal automation; do not indiscriminately apply app entitlements to every library. The entitlement follows [Apple's hardened runtime documentation](https://developer.apple.com/documentation/security/hardened-runtime). Verify automation in the signed app on the target OS. Installing the app can affect which client receives the existing `jms` scheme.

The bundle needs `Contents/Resources/bin/darwin-<arch>/JumpServerClient`, companion `client`, and `Scripts/iterm2_loader.scpt`. The Go binaries must retain executable mode. The source AppleScript files use mode `0644` and are read by `osascript`, so they do not need an executable bit.

## Windows

Use `package-windows.py` to deploy Qt, TLS and compiler runtimes and generate the NSIS EXE / WiX MSI. The helper directory contains `JumpServerClient.exe` (amd64) or `JumpServerClient32.exe` (386), both `AutoItX3.dll`/`AutoItX3_x64.dll`, and the source project's 32-bit `putty.exe`. The amd64 package therefore requires normal Windows x86 application support. Both installers are per-machine, create Start menu and desktop shortcuts, and register `jms://`; uninstall ownership checks are designed to preserve another client's later registration. They do not remove the user's application configuration or credentials. Windows installation/upgrade/uninstall must still be validated on its target OS; see [Windows details](windows.md).

## Linux / UOS / Kylin

Install the executable under `/usr/bin/client`, helper files under `/usr/lib/client/bin`, `client.desktop` under `/usr/share/applications`, and the icon in the appropriate `hicolor` directory. A user can select the URL handler explicitly using `xdg-mime default client.desktop x-scheme-handler/jms`. For system packages run `update-desktop-database` from the package trigger when available. DEB and RPM metadata must depend on the actual Qt, XCB/Wayland, TLS and libsecret libraries shipped by the target distribution. Build in the oldest selected ABI environment for each supported architecture. Test `jms://` from an installed package in both cold-start and already-running cases.

Signing, notarization, final package installation, third-party client launches and old OS/architecture compatibility require their target environments; building a GUI alone does not validate them.

Go 1.27.1 cross-compilation was checked for Linux amd64, 386, arm64, loong64, mips64le, riscv64, and arm (`GOARM=7`); Windows amd64/386; and macOS amd64. The current macOS arm64 helper was also built. Linux outputs include both `JumpServerClient` and `client` and are static ELF binaries; Windows includes its matching launcher and vendored dependencies. These checks do not execute the output or establish compatibility with old kernels, distributions, graphics stacks or third-party clients. Go 1.20 was not locally cached, so a legacy Go build was not run.

For 32-bit ARM, pass `--goarch arm --goarm 7` to the helper script; its runtime directory is `linux-arm`. The default is always ARMv7, independent of inherited `GOARM`; use `--goarm` for an explicit override. ARMv5/6 values are accepted by the script but were not validated. Architectures without an upstream Go port, including ShenWei `sw_64`, are unsupported by this helper build and are not included in the compatibility claim.

Only Linux loong64 uses `scripts/go-loong64.mod` and `.sum`: the source's 2021 `golang.org/x/sys` lacks LoongArch syscall types, preventing the SSH companion from compiling. The overlay changes only this dependency to `v0.6.0`; every other target still uses the unchanged `go-client/go.mod` snapshot. The script supplies the overlay automatically. LoongArch distributions may require patched Qt/toolchains or a different userspace ABI, and old vendor kernels require their own verification; consult [Go's architecture and kernel requirements](https://go.dev/wiki/MinimumRequirements).

## Legacy build profiles

Use separate artifacts for each OS baseline and architecture. [Go 1.20](https://go.dev/doc/go1.20) is the final Go line supporting Windows 7/8 and Server 2008/2012, and macOS 10.13/10.14. A legacy profile should explicitly choose a maintained Qt 5.15 build suitable for that baseline and the final Go 1.20 toolchain (1.20.14), with its own compatibility/security maintenance plan. A recent compiler or prebuilt Qt package may itself require a newer OS.

Windows example (run from the matching Qt/compiler environment):

```sh
cmake -S . -B build-win-legacy -DCLIENT_QT_MAJOR=5 \
  -DCMAKE_PREFIX_PATH=C:/Qt/5.15-legacy \
  -DGO_EXECUTABLE=C:/toolchains/go1.20.14/bin/go.exe \
  -DCLIENT_GOOS=windows -DCLIENT_GOARCH=amd64
cmake --build build-win-legacy --config Release
```

macOS Intel example, using Qt frameworks actually built for 10.13:

```sh
cmake -S . -B build-mac-legacy -DCLIENT_QT_MAJOR=5 \
  -DCMAKE_PREFIX_PATH=/opt/qt5.15-macos10.13 \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=10.13 \
  -DGO_EXECUTABLE=/opt/go1.20.14/bin/go \
  -DCLIENT_GOOS=darwin -DCLIENT_GOARCH=amd64
cmake --build build-mac-legacy
```

Changing `CMAKE_OSX_DEPLOYMENT_TARGET` cannot lower the minimum OS of prebuilt Qt frameworks. Apple Silicon requires a separate arm64 package and its applicable macOS baseline. For Linux/UOS/Kylin, build Qt/C++ in the oldest selected distribution ABI environment, use the corresponding CPU toolchain and check the Go runtime's kernel baseline. Cross-compilation alone does not validate old UOS/Kylin releases. The legacy example profiles have not been executed on their target operating systems.
