# Windows EXE / MSI

在 **Windows 构建机**的 Qt/MSVC Developer Prompt 或匹配的 MinGW 环境中执行。脚本不会下载依赖，也不会把 macOS/Linux 产物改名为 Windows 安装包。

需要 Python 3.8+、CMake、匹配架构的 Qt 5.15/Qt 6、Go、NSIS 3（EXE）、WiX **3.14.x** 的 `candle.exe`/`light.exe`（MSI）。WiX 4+ 的单一 `wix.exe` 接口不适用。MSI 还用同一个 C++ 编译器构建无 CRT 依赖的协议归属保护 DLL。

```powershell
cmake -S . -B build-windows -A x64 `
  -DCLIENT_QT_MAJOR=5 -DCMAKE_PREFIX_PATH=C:/Qt/5.15.2/msvc2019_64 `
  -DCLIENT_GOOS=windows -DCLIENT_GOARCH=amd64 -DCLIENT_VERSION=4.1.2

python scripts/package-windows.py --build build-windows `
  --windeployqt C:/Qt/5.15.2/msvc2019_64/bin/windeployqt.exe `
  --openssl-dir C:/dependencies/openssl-1.1-x64/bin `
  --arch amd64 --format both --output dist
```

版本取自 `CMakeCache.txt` 的 `CLIENT_VERSION`，兼容旧缓存的 `CMAKE_PROJECT_VERSION`。`--format exe` / `--format msi` 可单独生成；`--skip-build` 复用现有 Release 构建。支持 `amd64` 和 `386`；32 位应使用独立的 Win32 Qt/编译目录，CMake 传 `-A Win32 -DCLIENT_GOARCH=386`，脚本传 `--arch 386`。本项目的 Windows helper 不支持原生 ARM64。

脚本依次构建 GUI/Go helper、创建临时安装树、运行匹配的 `windeployqt --release --compiler-runtime`、部署运行库、检查 PE 架构及必需文件，并从清理 Qt 开发环境变量的环境运行 `--smoke-test`。当 Qt 5 的 `bin` 目录既没有 `libGLESv2.dll`/`libEGL.dll`，也没有合并的 `QtANGLE.dll` 时（CI 的 `-no-opengl` 源码构建即如此），额外传入 `--no-angle --no-opengl-sw`，避免部署工具寻找不存在的 ANGLE/软件光栅库；自带 ANGLE 的 Qt 5 及 Qt 6 保留部署工具的默认依赖判断。最后生成：

- `OSMClient-<version>-windows-<arch>.exe`：NSIS 全局安装器。
- `OSMClient-<version>-windows-<arch>.msi`：Windows Installer 全局安装包，支持版本升级；可用 `msiexec /i <file>.msi /qn` 安装。
- 各安装包的 `.sha256` 和 `.manifest.json` 文件；manifest 记录实际 `signed` 状态。

输出目录同名文件已存在时失败；构建/部署/签名中途失败不会把临时安装器复制到输出目录。应用签名不会修改原始构建目录。`--makensis`、`--candle`、`--light`、`--compiler` 可以指定工具完整路径。

## Runtime / TLS

MSVC 默认从 Developer Prompt 的 `VCToolsRedistDir/<x64|x86>/Microsoft.VC*.CRT` 复制 app-local CRT DLL，也可通过 `--runtime-dir` 指定。不会把未执行的 `vc_redist.exe` 当成运行库部署成功，更不会从 per-user MSI 嵌套启动需要管理员权限的 redistributable 安装。较旧 Windows 如缺少 Universal CRT，需要匹配的系统更新或通过 `--ucrt-dir` 提供相应 Windows SDK 的可再分发 UCRT DLL。

OpenSSL 必须与 Qt 构建所使用的 ABI 一致（例如官方 Qt 5.15.2 通常使用 OpenSSL 1.1）。脚本优先从 `--openssl-dir`，否则从 Qt `bin` 复制 `libssl*.dll`、`libcrypto*.dll`。只有已确认 Qt 使用 Windows 原生 Schannel 的构建才可传 `--native-tls`；这个参数不改变 Qt 的 SSL 后端。部署 smoke test 不替代 HTTPS/OAuth 实站验证，也不能证明 OpenSSL ABI 匹配。

Qt 的部署工具不负责所有第三方依赖，参见 [Qt 5.15 Windows deployment](https://doc.qt.io/archives/qt-5.15/windows-deployment.html)。本项目的 Go helper 会带入对应的 `JumpServerClient*.exe`、两种架构的 AutoIt DLL 及源项目的 x86 PuTTY；amd64 安装仍要求系统具备 x86 应用支持。

## 可选签名

原 client 没有可复用的 Windows 签名证书配置；**默认产物 UNSIGNED**。显式增加 `--sign` 后，有以下凭据时启用 PFX 签名：

- `WINDOWS_CERTIFICATE`：PFX 文件的 base64 内容。
- `WINDOWS_CERTIFICATE_PASSWORD`：PFX 密码（允许证书确实使用空密码）。

证书只在临时目录解码，完成/失败后清理。脚本不打印密码、证书或含密码的 signtool 命令；签名失败输出工具退出码。`--sign` 缺少证书或未设置密码变量时明确输出跳过原因，继续生成未签名包，manifest 的 `signed` 为 `false`。密码变量已设置为空字符串可用于无密码 PFX。证书已提供但内容无效、密码不正确、签名工具缺失或实际签名失败时仍报错，不悄悄降级。`--signtool` 可指定 Windows SDK 的 `signtool.exe`；使用 SHA-256 签名和 RFC3161 时间戳，默认 `http://timestamp.digicert.com`，可用 `--timestamp-url` 覆盖。

签名对象包括本项目 Qt EXE、Go helper、MSI 保护 DLL、NSIS 卸载器、最终 EXE/MSI；不重签 Qt、OpenSSL、AutoIt、PuTTY 等第三方二进制。证书/密码只通过构建机环境或 CI secret 注入，不应写入脚本或命令行。

## 安装、卸载与兼容性边界

两种格式默认安装到系统 `Program Files/OSMClient`，需要管理员权限，创建 `OSMClient` 开始菜单和公共桌面快捷方式并注册 `jms://`，可执行文件为 `client.exe`。旧用户级版本需先卸载，用户配置保留；安装器标题和应用列表显示名为 `OSMClient`。EXE 与 MSI 不能同时管理同一目录；切换安装格式前需卸载旧格式。NSIS 升级先调用上一版本自己的卸载器；MSI 使用同一 UpgradeCode、在安装新文件前执行 major upgrade，旧快捷方式由旧包卸载。用户配置和系统凭据库不在卸载文件清单中。

NSIS 仅删除生成清单中的文件，不递归清空用户选择的目录。NSIS 卸载前比较当前 `jms` command；MSI 通过 `windows-jms-ca.cpp` 在 `RemoveRegistryValues` 前检查同一条件。MSI 不持久化共享 `jms` 的 Registry 行：安装/修复时临时添加四行，由标准动作写入；卸载时仅在仍属本程序的情况下添加临时行供标准动作移除，其他客户端已接管则不添加。这样仍由 MSI 标准动作管理回滚，CA 不改持久表行、不直接修改注册表。依据 [Microsoft 当前安装会话限制](https://learn.microsoft.com/en-us/windows/win32/msi/accessing-the-current-installer-session-from-inside-a-custom-action) 和 [RemoveRegistryValues action](https://learn.microsoft.com/en-us/windows/win32/msi/removeregistryvalues-action)。卸载不会自动恢复安装前的旧协议处理器。

MSI 的 ICE 一致性校验限定了 authoring 位置，只用结构调整满足，不用 `light -sice:` 抑制：

- 开始菜单和公共桌面快捷方式组件位于用户配置文件目录，MSI 要求 key path 为 `HKCU`（ICE38/ICE43/ICE57 按 authoring 目录判断，与 `ALLUSERS`、`InstallScope` 无关）；`InstallScope="perMachine"` 时 `ALLUSERS=1` 仍把快捷方式写入 All Users 开始菜单和公共桌面，标准目录本身不会被删除，因此不写 `RemoveFolder`。参考 [WiX 开始菜单快捷方式](https://wixtoolset.org/docs/v3/howtos/files_and_registry/create_start_menu_shortcut/) 与 [Installation Context](https://learn.microsoft.com/en-us/windows/win32/msi/installation-context)。
- 不装文件的 `JmsRegistration` 挂在 `TARGETDIR`，不再挂在 64 位 `INSTALLFOLDER` 下：ICE80 拒绝 64 位目录中的 32 位组件，而它必须保持 32 位组件，才能让安装标记和临时 `jms` 行与 NSIS 包共用 32 位注册表视图（`windows-jms-ca.cpp` 用 `KEY_WOW64_32KEY` 读回，`windows.nsi` 用 `SetRegView 32` 判断）。标记值仍取 `[INSTALLFOLDER]`，与组件目录无关。

**旧系统兼容由 Qt、C++/CRT、Go、OpenSSL 的实际构建工具链共同决定**，EXE/MSI 扩展名和安装器的 Windows 7 下限不能降低程序本身的系统要求。Windows 7/8 的 Go helper 需要相应旧工具链（Go 1.20 为最后支持线），Qt 6 与现代 CRT/SDK 应单独建立较新系统包；不能把当前 CI 包宣称为 Windows 7 已验证包。

当前 macOS 开发机已验证 Python 行为测试、两架构 NSIS 模板编译及保护 DLL 的 x86/x64 交叉编译；测试安装器使用显式 synthetic payload 并自动删除。**Windows Qt 部署、最终 WiX 编译、真实 PFX 签名、安装/升级/卸载、协议接管保护和旧 Windows 实机运行仍需 Windows 环境验证。**

CI 使用 Qt 5.15.19 源码和 Schannel（`--native-tls`）、VS2022、NSIS 3.10、WiX 3.14.1。工作流构建之后调用以下检查：分别静默安装 EXE/MSI，运行已安装程序 smoke test，再验证正常卸载与另一客户端接管 `jms` 后卸载的保留行为。此脚本会改测试注册表，仅允许无预存安装/`jms` 注册的 GitHub 托管一次性 Windows runner；提供脚本不表示已在本机执行通过。

```powershell
./packaging/windows/verify-installed.ps1 -Dist dist -Arch amd64
```

```sh
python -m unittest discover -s tests -p 'test_packaging*.py'
```
