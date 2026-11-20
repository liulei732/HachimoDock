# Codex 额度账号管理

为声明 `codex.accounts` 的组件在详情页提供账号管理，位置和自选股列表一致。
此改动增加 PC 宿主能力，不内置包含用户邮箱的额度看板资源包，也不启用旧的
`widget/data` / `codex.usage` 占位提供器。

## 使用与支持范围

目前支持 macOS，需要 Chrome，以及提供 `account/login/start`、`account/read`
和 `account/rateLimits/read` 的 Codex CLI。宿主优先使用已安装的 ChatGPT/Codex
应用自带 CLI，再查找 PATH。Windows 在独立凭据存储保护完成前明确拒绝启用。

1. 在桌面端组件中心导入 [Codex 额度看板 1.4.21](../community-components/codex-quota-board/codex-quota-board-1.4.21.hachiapp)，打开组件详情的“账号管理”。
2. 点击“添加账号”或“重新授权”，在自动打开的 Chrome 中完成登录。
   登录只用于额度看板的独立目录，不替换当前聊天的登录。
3. 最多管理 8 个账号。勾选设备显示的账号，用上下箭头排序；修改立即保存，
   无需重新安装组件。全部取消显示时，投影为空状态，不沿用旧账号额度。
4. 新账号默认不显示；勾选前检查组件字体能否绘制该邮箱，缺字时需先更新组件资源。
5. 通过 USB 连接桌搭子，在电脑端保存并同步组件。打开设备上的「Codex 额度看板」，保持电脑应用运行。

### 设备页面与按键

左右拨摇杆切换账号，上下拨切换「额度」「Token 概览」「明细」。短按 A 刷新，在明细页短按 B 切换近 1 小时、今日、近 24 小时、近 7 天、近 30 天。「今日」以电脑本地时间零点为起点。配套固件可在等待新页时保留上一帧。

额度页显示对应账号的会员等级、剩余额度和重置时间。概览显示近 7 天趋势，明细按类型和模型拆分。Token 统计只计入本机 Codex 日志中可归属到所选账号和用户的记录，同时读取活动会话和归档会话。每次完整扫描成功后更新结果。

## 声明与投影

在 `pc/application.json` 的 composition `content[0].sources` 中声明：

```json
{"usage":{"kind":"local-telemetry","source":"codex.accounts"}}
```

额度刷新和账号切换映射为 `usage:refresh`、`usage:previous`、`usage:next`。三页看板还使用 `usage:page_up`、`usage:page_down` 和 `usage:cycle_period`。动作名须与 runtime
的 `host.*` transition 配对。使用 [composition 契约](../skills/petui/references/composition.md)
的显式 bindings，不在组件中指定路径、可执行文件、OAuth 参数或网络请求。

一个 8 标量看板可分别绑定以下 JSON Pointer：

| 用途 | Pointer | 类型 / 缺测值 |
| --- | --- | --- |
| 会员标识 | `/account` | string / `待获取` |
| 账号序号、邮箱与状态 | `/identity` | string |
| 5 小时剩余额度 | `/primary/remaining` | int / -1 |
| 5 小时重置说明 | `/primary/detail` | string |
| 5 小时时间进度 | `/primary/timeRemainingPct` | int / -1 |
| 每周剩余额度 | `/secondary/remaining` | int / -1 |
| 每周重置说明 | `/secondary/detail` | string |
| 每周时间进度 | `/secondary/timeRemainingPct` | int / -1 |

整数变量的 limits 必须允许缺测值 -1；组件自行绘制缺测状态。只匹配明确返回的
300 / 10080 分钟窗口，不把不同长度窗口冒充 5 小时或每周额度。会员等级与额度来自
同一账号查询。无窗口、超过旧重置时间时不推算成满额。

普通查询最短间隔 60 秒，手动刷新最短 15 秒；查询在后台执行。查询期间、失败或
同账号令牌轮换时保留本进程最近的有效结果，五分钟以上或查询失败会标记过期。
重启后需要重新查询；缓存尚未持久化。切换身份、移除或完成重新授权时不复用旧结果。

## 本机数据与登录边界

原生层用独立 `CODEX_HOME` 启动 Codex app-server，由 Codex 处理登录及令牌轮换，
不实现内部额度 HTTP API，不启动聊天、任务或模型调用。查询核对邮箱、保存的账号
身份和凭据修订，仅接收 Codex 额度分组；上游错误不原样返回给组件。

凭据、显示设置及可恢复移除备份存于本机私有的 `~/.claw-pet/codex-accounts`。
账号目录权限为 0700，新写入的登录文件和设置为 0600；拒绝符号链接目标。
IPC 只接收受限操作和账号槽位，不接收任意路径、命令、令牌或 URL。
登录 URL 留在原生层，仅允许 HTTPS `auth.openai.com` 并交由 Chrome 打开。
取消、登录错账号、重复账号或旧修订提交均不替换原授权。

账号邮箱、会员标识、额度和 Token 汇总会作为组件声明的显示数据发送给本机连接设备。
凭据、原始服务响应、账户 ID 和登录 URL 不发送给设备，不写入可分发资源包。
“移除”只从看板移除，本机恢复备份不会自动撤销服务端授权。

## 验证

- `npm test`：数据源识别、显示/排序修订、字体覆盖及状态文案。
- `cargo test --locked --manifest-path pc/tests/codex-accounts-host/Cargo.toml --lib -- --test-threads=1`：
  直接编译生产账号服务，使用临时目录和模拟 app-server，不读取真实凭据或启动浏览器。
  覆盖登录、取消、重复/错误账号、移除备份、权限、缓存身份隔离、令牌轮换及旧请求回写。
- 主程序库的 `hachiapp_desktop_metadata_never_reaches_the_device_installer`：
  通过实际传输路径和模拟设备 ACK 验证电脑端清单/封面不会误发给设备。

实际新登录、设备画面与实体按键仍需另行验收；模拟测试不等于完成全部实机流程。
