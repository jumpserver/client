# 原生安装包 CI

`.github/workflows/build.yml` 在独立的 `client-qt` 仓库根目录运行。只有 `v*` 标签、Pull Request 和手工 `workflow_dispatch` 触发构建；普通分支推送不再自动构建，需要验证时在 Actions 手工运行并选择目标分支。所有构建只上传 GitHub Actions artifacts（保留 14 天）。只有 `v*` 标签触发成功时，额外的 `release` job 汇总各平台 artifacts 创建同名 **draft** GitHub Release 并挂载安装包；PR 和手工运行不创建 Release。workflow 默认 `contents: read`，仅 `release` job 提升到 `contents: write`。

## 构建矩阵

| 产物 | 构建环境 | Qt / TLS | 默认架构 / 目标基线 |
| --- | --- | --- | --- |
| DMG | `macos-15-intel`，Xcode 16.4 | 官方 Qt 5.15.19 源码，Secure Transport | x86_64；部署目标 macOS 10.15 |
| DMG | `macos-15`，Xcode 16.4 | 同上 | arm64；部署目标 macOS 11.0 |
| EXE + MSI | `windows-2022`，MSVC 14.44，Windows SDK 10.0.19041.0 | 官方 Qt 5.15.19 源码，Schannel | amd64；Windows 10+ |
| EXE + MSI（手工可选） | 同上，MSVC x86 目标 | 同上 | 386；Windows 10+ |
| DEB | `debian:11.11-slim` 容器 | 发行版 Qt 5.15.2、OpenSSL / CA 包 | amd64；Debian 11 用户空间 |
| RPM | `rockylinux/rockylinux:8.10` 容器 | 发行版 Qt 5.15.3、OpenSSL / CA 包 | amd64；EL8 用户空间 |

Linux 构建在 Ubuntu 24.04 runner 的发行版容器内完成，镜像固定到已查询的 amd64 digest。RPM 使用原生 `rpmbuild`，不从 DEB 转换；Qt、libsecret 等由目标发行版包管理器提供。Debian 11 security 仓库迁移到 archive 后只调整仓库 URL 和过期元数据检查，保留包签名校验。Rocky 使用官方 HTTPS baseurl，避免地理镜像同步窗口导致的 404。

这些是编译/打包基线，不能代替旧系统实机验收。统信 UOS、麒麟需要分别确认发行版版本、CPU、Qt 5.15 和 libsecret 包名/ABI、桌面与 SSH/RDP 外部客户端。当前 CI 不承诺任意 UOS/麒麟版本、Windows 7/8、macOS 10.14 或其他 CPU 已兼容。其他目标可直接使用打包脚本和合适的本地工具链。Go 固定 1.21.13，沿用原项目 1.21 系列启动器基线；升级 Go 时需重新检查最低 OS 要求。

## 版本、签名与凭据

`scripts/ci-version.py` 将手工 `version`、标签 `vX.Y.Z` 或 CMake `CLIENT_VERSION` 默认值解析为数字版本。手工留空采用默认值；在标签上手工触发时版本必须与标签一致。为兼容 MSI，不接受预发布后缀、前导零或超出 `255.255.65535` 的值。用户输入只通过环境变量传递，不拼入 shell 代码。

| 触发方式 | macOS | Windows |
| --- | --- | --- |
| PR（非标签） | ad-hoc 签名，不公证，无需证书 | unsigned，无需证书 |
| `vX.Y.Z` 标签 | 请求 Developer ID 签名及公证；缺凭据时跳过对应步骤 | 默认 unsigned；仓库变量 `SIGN_WINDOWS=true` 时请求签名 |
| 手工运行 | `sign_macos` 请求签名及公证；标签始终请求 | `sign_windows` 请求 Authenticode；标签也遵循仓库变量 |

macOS 沿用原 `clients` 的 Secrets 名称：

- `APPLE_CERTIFICATE`：Developer ID Application 证书及私钥的 base64 P12。
- `APPLE_CERTIFICATE_PASSWORD`：P12 密码。
- `KEYCHAIN_PASSWORD`：流水线临时 keychain 密码。
- `APPLE_ID`、`APPLE_PASSWORD`：Apple ID 和应用专用密码，供 `notarytool` 使用。
- `APPLE_TEAM_ID`：可选；缺省从证书推导，提供时必须与证书一致。

`setup-apple-cert.py setup` 在证书凭据齐全时导入专用临时 keychain，导出被掩码的 identity hash、keychain 路径和 Team ID，以及 `APPLE_SIGNING_ENABLED=true`。缺少必需凭据时明确记录跳过签名，输出 `APPLE_SIGNING_ENABLED=false` 并继续生成 unsigned/ad-hoc 包；证书损坏、密码错误、签名执行失败仍使任务失败。签名可用时依次签名嵌套依赖、Go 启动器、应用及 DMG。只有公证凭据齐全才提交公证并装订票据；缺少时明确输出已签名、未公证 DMG。`always()` 清理步骤恢复原 keychain 搜索列表并删除临时 keychain。缓存和 artifacts 不含证书、私钥或临时 keychain。

`xcrun stapler` 通过 HTTP/3（UDP 443）向 `api.apple-cloudkit.com` 取公证票据；GitHub `macos-15-intel` runner 到该端点的连接会间歇性断开并报 `Error 68` / `NSURLErrorDomain -1005`（[actions/runner-images#14924](https://github.com/actions/runner-images/issues/14924)）。公证构建在打包前对 x86_64 runner 临时封禁 UDP 443，强制 stapler 回退到 TCP HTTPS，仅作用于当前一次性 job；`package-dmg.py` 另外对 `staple`/`validate` 的瞬时网络错误最多重试 5 次、间隔 5 秒，非瞬时错误（如票据不存在）立即失败。公证被 Apple 拒绝仍直接报错，不重试。

原项目 Windows 配置没有证书。新增可选 Secrets `WINDOWS_CERTIFICATE`（base64 PFX）和 `WINDOWS_CERTIFICATE_PASSWORD`；请求签名但缺少它们时记录跳过并生成 unsigned 包，实际证书或签名错误仍失败。为标签请求自动签名，另外显式设置仓库 Actions variable `SIGN_WINDOWS=true`。各包随附 manifest 记录实际签名/公证状态；开关代表请求，不代表签名已经完成。

## 固定依赖与缓存

Action 均固定到完整 commit SHA，并注释对应版本。构建工具固定 Python 3.12.10、CMake 3.31.6、Ninja 1.11.1.4、Go 1.21.13；Windows 校验 runner 内 WiX 3.14.1 与 NSIS 3.10，不匹配会失败并要求更新配置。Qt 源码下载地址、SHA256 和 Windows jom 1.1.4 哈希在 `scripts/build-qt.py` 中固定。

macOS/Windows 编译 Qt Base、Qt SVG、部署工具，关闭不用的 OpenGL、数据库驱动和 Qt Tools，保留 Qt Test 库。Qt 使用共享库；Windows 使用系统 Schannel，macOS 使用系统 Secure Transport，避免捆绑旧 OpenSSL 运行库或 Homebrew 的高版本最低 OS 依赖。固定 Xcode/SDK 与部署目标同时传给 Qt 和应用。

首次源码构建耗时较长，job 给 macOS 150 分钟、Windows 180 分钟。后续缓存安装后的 Qt 前缀，cache key 包含 Qt 版本、平台、架构、编译器/SDK或部署目标以及整个构建脚本的哈希；没有宽泛回退 key。构建脚本也校验完成标记、Qt SVG/Test 和部署工具是否齐全。需要重试不完整的本地 Qt 构建时，请使用新的 `--prefix`、`--work` 目录，脚本不会自动删除已有安装。

## 验证范围

流水线通过 `BUILD_TESTING=OFF` 只构建、打包和校验安装包，不编译测试程序，也不运行 CTest、Go 单元测试或离线打包回归测试（`tests/` 保留供本地验证）。Linux 在第二个干净容器中安装生成的包，检查依赖、MIME 注册、有效载荷及 `--smoke-test`。Windows 在一次性 GitHub-hosted runner 上验证 EXE/MSI 安装、安装后启动、正常卸载，以及其他应用接管 `jms` 后卸载仍保留该处理器。macOS 打包脚本检查 Mach-O 依赖和签名；签名任务另外执行 Apple 公证及 stapler 验证。

GitHub macOS hosted runner 偶发 `hdiutil create - Resource busy` 时，DMG 创建最多尝试 10 次（失败后间隔 5 秒，最多 9 次重试），仅清理本次未完成的目标文件；之后仍执行镜像校验、挂载检查、依赖检查及可用的启动 smoke test，不降低安装包的验证范围。

标签构建的 `release` job 依赖全部构建 job 成功；任一平台构建失败则整个 release job 跳过，不会产出残缺 Release。它先校验 dmg/exe/msi/deb/rpm 五类安装包齐全，再用 `softprops/action-gh-release` 创建 **draft** Release（生成 release notes、不设为 latest），安装包和各 `.manifest.json` 校验文件作为 assets 上传。draft 不会对外可见，需要维护者在 Releases 页面人工确认后手动 publish；重复运行同一标签会复用并覆盖同名 assets。

本地已通过 actionlint、36 项打包回归，完成官方 Qt 5.15.19 的 arm64/x86_64 源码构建及两种应用构建。独立 Qt 5 与 Qt 6 的 CTest 各 5/5 通过。两个打包阶段强制逐个 Mach-O 检查架构、依赖和 minOS，并写入目标系统版本，超标立即失败。原生 ARM DMG 完成挂载内 Cocoa smoke；Intel 在无 Rosetta 的 ARM 主机交叉构建，显式记录未执行启动检查。复现说明见 [macOS 兼容构建](macos-compat.md)。

此次本机使用 Apple clang 21/Command Line Tools；源码构建产生的旧部署目标警告不代表旧系统已实测。项目尚未发布到 GitHub，本轮没有运行 GitHub Actions；固定 Xcode/MSVC 的 CI 构建、Windows 安装验证、真实签名/公证和 macOS 10.15/11 实机仍待验收。当前兼容包位于 `dist/current/`；历史开发包已清理，最新结果见 [验证记录](../docs/validation.md)。

参考官方来源：

- [GitHub-hosted runner 标签](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)；[runner 镜像软件清单](https://github.com/actions/runner-images)。
- [Qt 5.15.19 官方源码和校验值](https://download.qt.io/archive/qt/5.15/5.15.19/submodules/)；[Qt 官方 jom 下载](https://download.qt.io/official_releases/jom/)。
- [setup-python 可安装版本清单](https://github.com/actions/python-versions/blob/main/versions-manifest.json)；[MSVC developer environment action](https://github.com/ilammy/msvc-dev-cmd)。
- [Go 1.21 Windows 最低要求](https://go.dev/doc/go1.21#windows)；[WiX 3.14.1](https://github.com/wixtoolset/wix3/releases/tag/wix3141rtm)。
