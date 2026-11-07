# osm 迁移映射

基线：`clients` 仓库 `osm` 分支，commit `30edaf4a`。原仓库及 `mygo-poc` 保持不变，新项目可以单独构建。

| 原项目职责 | Qt 实现 | 验证方式 |
| --- | --- | --- |
| Rust OAuth/token/http_callback | `src/appservice.*` | HTTP mock：PKCE、state、回调、刷新并发、取消、失效响应与撤销 |
| Rust assets/detail/favorite/rename | `src/appservice.*` | 授权资产接口、合并类型分页、组织切换隔离、收藏请求 |
| Rust connect_methods/settings/version/token | `src/appservice.*` | 令牌请求与原始 JMS 返回值测试 |
| Vue 资产页、侧栏、右键菜单 | `src/mainwindow.*` | Qt Test 与离线截图 |
| Vue 连接弹窗、资产连接偏好 | `src/connectiondialog.*` | 手工/同名/托管账号、协议与方式过滤、RDP 参数、偏好隔离 |
| Vue 通用/外观/应用/关于设置 | `src/settingsdialog.*`、`src/theme.*` | UI 测试与人工界面检查 |
| Rust pull_up + Go launcher | `src/platform.*` + 原 `go-client/` | 真实子进程参数透传、退出/错误检测、Go 交叉编译 |
| Tauri deep link / 单实例 | `src/main.cpp`、`src/platform.*`、macOS `src/protocol_events.mm` | IPC 测试、原始 JMS 保真；安装后的系统协议关联另需实机验证 |
| keyring / 配置 | `CredentialStore`、`ConfigStore` | 合并、原子保存、损坏保留；可选系统凭据往返测试 |

## 接口与兼容约定

- 仅查询 `/api/v1/perms/users/self/assets/` 等用户授权接口；不改用管理端资产 API。
- 组织通过 `X-JMS-ORG` 传递，登录/组织切换后过期响应不再更新界面。
- OAuth 与 API 请求异步，网络超时、响应大小和回调生命周期均有边界。
- 与原 Rust 客户端一致，客户端 HTTPS 请求默认忽略服务端证书校验，覆盖版本预检、OAuth 令牌交换/刷新/撤销及业务 API；HTTPS 加密与 HTTP 授权检查保留，TLS 协议或握手失败仍中止请求。外部浏览器授权页和 SSH/RDP 等客户端的证书策略独立。
- `jms://` 的普通连接载荷是 Base64，不能经 `QUrl` 标准化；命令行、IPC、macOS AppleEvent 和 HTTP 返回值均保持原始字符串。只有 OAuth 回调按 URL 解析。
- Go 启动器源码保留，Qt GUI 不重新实现各第三方客户端的命令行、配置注入或自动填充。

## 有意保留的差异

- 使用 Qt 原生控件重建交互，布局、分类、卡片/表格和设置能力对应原 UI；不承诺 Web CSS 的逐像素复刻。
- 原无限滚动改为 20/50/100 条分页；重命名使用原生输入对话框。Qt 的界面与连接偏好不自动导入原 Tauri 设置。
- 登录前按原分支规则校验服务端允许的客户端主次版本；版本接口异常或返回非法格式时中止登录，避免带着未知兼容性继续认证。
- public 设置失败或缺少记忆认证策略时，默认不记忆输入凭据；网络请求不自动跟随重定向，请配置最终站点地址。
- 登录 token 不在 UI 层暴露，手工输入密码存入系统凭据库，不存于普通设置文件。
- 不读取或迁移原 Tauri 的登录 token；仅共享 Go 应用配置。
- 错误消息不直接打印 OAuth 响应体、JMS 链接或 Go 原始 stderr，以免包含连接凭据。
- Windows ARM64 原生自动化资源不完整，构建脚本明确拒绝原生 ARM64 启动器；兼容层运行 x64/x86 包需目标机验证。

## 验收范围

自动测试覆盖的是可控的协议与交互行为。真实 OAuth 站点、权限/许可证组合、第三方 SSH/RDP/VNC/数据库客户端及各系统的浏览器回调，必须使用授权测试环境逐项验收。当前没有把 mock 测试结果表述为真实资产连接成功。

验收顺序：安装并注册协议 → 冷启动回调 → 运行中回调/单实例 → 登录/刷新/退出 → 切换站点与组织 → 各资产分类/收藏/重命名 → 各账号模式与协议 → 保存/撤销输入凭据 → 客户端缺失/启动失败 → 升级保留配置。
