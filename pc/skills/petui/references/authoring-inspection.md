# 创作目标与工具索引

从画面、数据来源和用户操作选入口。一项组件可以同时用到多项能力。新建和修复都沿用主说明的工作流。

| 创作目标 | 先选哪种能力 | 如何验收 |
|---|---|---|
| 设备离开电脑后持续读公开 API、私人 HTTPS 或 MQTT | [设备联网](wifi-data.md)，在组件内完成私人服务授权 | 设备独立供电，重启后读取一条新数据 |
| 在设备显示电脑状态或用户的本机数据 | [PC 快照与传输](live-data.md)，按来源选择提供器 | 核对实际激活的组件、数据源、USB 或 Wi-Fi 接收计数和时效 |
| 用 Python 处理用户选的文本或公开 JSON | [桌面 Python 插件](python-plugins.md)，检查工具公布的能力和输入类型 | 预览返回字段，新用户授权后检查绑定，升级再验一次 |
| 组合多个数据源、刷新或切换页面 | [组合应用](composition.md)、[工作流](workflows.md)、[页面导航](app-navigation.md) | 检查各来源的绑定、等待数据的画面和操作反馈 |
| 制作表单、列表或精细布局 | [表单](app-form.md)、[应用界面](app-surface.md)、[界面验收](ui-design.md) | 核对首屏、长文本、输入和返回 |
| 制作离线游戏、动画或带实体键的工具 | [组件包和 WASM SDK](hachiapp.md)、[输入规则](input-policy.md) | 回放首帧和主操作，按需要在设备验收实体键和触屏 |
| 播放媒体、触发音效或调用桌面动作 | [组件媒体](component-media-events.md)、[服务桥](service-bridge.md)、[快捷键](hotkeys.md) | 核对权限、开始、退出和设备能力 |

## 查看组件兼容性

先用 `petui tool-info` 查看当前客户端的 `inspection` 命令列表。

```sh
petui compatibility widget.hachiapp
petui compatibility widget.hachiapp --diagnostics device.json
```

报告按功能列出运行路径、PC 宿主来源、动作、首次授权和安装时检查的设备能力。USB 安装握手与运行时取数分别判断。设备报告可提供握手的 capabilities 对象或者直接使用能力 JSON。字段未上报时显示 unreported。设备侧功能、PC 侧控制可以有各自的运行路径，来源能经 Wi-Fi 推送也可以同时具有仅 USB 支持的刷新动作。

`canRunAllListedFeaturesWithoutPc` 是整个包已列操作的路线判断；设备联网仍需外部网络。`targetAutonomy` 另报告当前目标：`no-device-route` 表示全部已列功能都缺设备路线，`incompatible` 表示已知不满足条件，partial-device-route 表示部分功能有匹配的设备路径，network-offline 表示所需网络离线，`unconfirmed` 表示缺少能力证据，`declared-checks-match` 表示列出的检查匹配。实机效果按下文验证。`deviceCapabilityChecks` 列安装所查字段，`deviceDirectChecks` 另列取数条件，例如股票的可选设备路线、私有源的凭据存储就绪。`feedProtocol` 区分 HTTPS 和 MQTT TLS。受控 Python 的输入种类、版本授权和桌面执行依赖分别见 `hostSources`、`firstUse` 和 `routes`。

## 定位首条数据

```sh
petui first-data widget.hachiapp --fixture sample.json --diagnostics device.json
```

两个参数均可省略。Python 的 text-file 样例使用 JSON 字符串，none 使用 null，https-json 使用域名接口的样例响应，工具会先按输入 pointer 投影。普通组合组件的样例使用以来源别名为键的输出对象。仅使用时钟、文档、计数等本地来源的组合组件可以省略样例，工具会调用正式组合宿主读取首次状态。hostSnapshot 记录各来源状态、变量数量和回退字段。提供样例时使用作者自备的脱敏数据。

- `previewGrant` 记录临时库中的授权及输入步骤。Python 的 `installedInput` 另行报告本机相同包的实际授权状态，包括未安装、版本不同、待授权和需要重选文件。
- `bindings` 逐项报告字段路径与目标变量。projected 表示通过运行时的类型和限额检查，missing-field 表示找不到路径，type-or-limit 表示运行时会采用 fallback。没有样例的来源显示 unobserved。
- `activeComponent` 使用采集报告里的 activeWidgetId 核对组件身份。`sourceReceipt` 使用现有 data-link 的来源、接收计数和新鲜度。仅有来源计数时，实际屏幕显示仍待核对。PC 应用快照和设备直连源的实机观察请按各自手册执行。
- 报告输出字段名、状态与计数。样例原文、脚本返回值和用户所选文件路径留在本机。

## 预演新用户首次安装和升级

```sh
petui install-rehearsal widget.hachiapp --fixture sample.json
petui install-rehearsal old.hachiapp --upgrade new.hachiapp --fixture sample.json
```

命令新建临时组件库，使用桌面端正式导入器导入，并重新读取当前版本。对 Python 组件，预演在临时目录选择样例文本或使用 HTTPS 响应样例，写入版本绑定的临时授权并运行脚本，随后逐项检查数据绑定。对其它组合组件可用来源输出样例验证绑定。升级包应保留组件 ID 且产生新的内容哈希。查看升级报告中的 grantBefore 可验证新版是否要求重新授权。临时库在命令结束时释放。

这一步覆盖本地导入、组件库读取和离线样例数据链路。公开审核、设备接收、屏幕实际展示及用户操作在对应阶段记录。

## 四板自治验收

用本次待测的 Pet Manager 和配套 CLI。先检查 tool-info 版本；命令入口指向旧安装时，可直接调用待测客户端可执行文件的 `--petui` 子命令。用 Pet Manager 的现有同步入口安装测试包。每块板单独建记录，保存板型探测、P4 修订、固件构建标识、包 ID/版本/摘要、屏型、触控就绪状态、握手能力及测试时间。记录冷启动前后的 bootId，脱敏凭据和板卡标识。准备独立 USB 电源、可开关的 AP，以及独立于桌面客户端的 HTTPS 服务或 MQTT broker；服务端能在指定时刻发布唯一新值。测试端点须满足包声明的公网 TLS、域名和响应约束。公开 HTTPS 和 MQTT 可沿用 [设备联网示例](wifi-data.md)，在同一行的 value 中依次放 A、B、C。WiFi6 接入其无线芯片支持的频段。

| 测试板及镜像族 | 两板族共用项目 | 另做的项目与预期 |
|---|---|---|
| WiFi6 v1（P4 0.1–1.99） | 步骤 1、2、5、6 | 跑 3、4；读取本板凭据存储状态 |
| WiFi6 v3（P4 3.0–3.99） | 步骤 1、2、5、6 | 独立跑 3、4；保存本板冷启和重连证据 |
| Pico v1（P4 0.1–1.99） | 步骤 1、2、5、6，PC 对照走 USB | 在步骤 1 检查无线专用包不兼容；混合行情仍可从 PC 接收 |
| Pico v3（含 v3.2） | 步骤 1、2、5、6，PC 对照走 USB | 单独记录无线专用包判定及离线存档；无触摸整机用实体键完成 |

1. **核对包与目标。** 准备已验证的离线存档工具、PC 来源列表、受控 Python 工具，以及 WiFi6 测试用的公开 HTTPS、匿名 MQTT、私有源。仓内可用 pc/builtin-clawpkgs/decision-capsule 作离线存档对照、computer-status 作 PC 对照、stock-watchlist 作双路线对照。有源码时，在独立输出目录用 `petui pack <源目录> <输出包.hachiapp>` 制包。逐包执行以下命令。`package.hachiapp` 换成本次包路径，`device.json` 取当前板的 hello capabilities 对象或含 capabilities 的报告。记录 `routes`、`deviceCapabilityChecks`、`deviceDirectChecks` 和 `targetAutonomy`。若只有收包计数，另取握手。用普通 USB 同步离线包和 PC 对照包；WiFi6 另同步精确能力匹配的联网包。

   ```sh
   petui tool-info
   petui validate package.hachiapp
   petui compatibility package.hachiapp --diagnostics device.json
   ```

2. **验证脱机前台和重启。** 给设备独立供电，拔数据线、关闭电脑和 AP。从宠物主界面进入组件中心，用实际绑定选择、打开并改变一个承诺持久化的值。例如在“纠结扭蛋机”同一分类抽取并排除一项，记下剩余数。等待存档完成，退出、重进、完全断电再上电，核对同 ID 的值和主操作。记录操作前后画面。墙钟未校准时应显示等待；WiFi6 恢复联网后核对校时。日期正常不能单独证明某个 API 已联网。
3. **验证设备直连。** PC 保持关机。WiFi6 板上打开系统设置、配网，等待取得 IP 和校时。先读公开 HTTPS 值 A，断 AP 并等待 TTL 到期，记录空态或陈旧标记；将响应改成 B，重新连接，记录 B 和服务端请求。选中自定义 feed 后冷启动，服务端改成 C，核对冷启后的请求、序号及屏幕。对匿名 MQTT 重复测试，在新订阅建立后向确切 topic 发布新的唯一值；保存 broker 的订阅记录、独立发布端的发送时间和设备显示值。若包声明 wifi.audio，手动开始、断网再连、退出，核对实际声音与停止时点。
4. **验证设备内授权。** 对 Bearer HTTPS 或私人 MQTT，在组件打开“服务账号”，用测试账号输入凭据，检查目标域名/主题。全程以独立电源供电、关闭 PC。先收到 A，再冷启动设备，让服务端发 B。撤销授权后再发 C，设备须提示重新授权且不显示 C。储存未就绪的板验收其提示与拒绝保存；此板正向持久凭据项目记为条件不足。公开订阅和本地功能另验。
5. **验证依赖 PC 的对照组。** 启动客户端并通过 USB 发布 computer.status 等新快照。对 WiFi6 再拔 USB，在已配对且该通道可用时测无线推送。采集每条路线的完整诊断与屏幕；将完整诊断中的 boardDeviceId 和 bootId 分别赋给 BOARD_ID、BOOT_ID。下列 data-link 命令的 runtime/widget.json 取自本次测试的列表组件，分别对断线前后的诊断文件运行。对 Python 包，按声明使用无隐私样例运行 first-data，再在桌面启用插件、选文件或授权域名，改变输入并确认绑定更新；撤销后验证 fallback。HTTPS 输入由独立发布端在 PC 关机后改值。文本输入可在客户端完全退出而电脑仍开机时改文件。两种情况下设备都应停在旧值或等待态。恢复授权和客户端后核对新值。对支持配对应用通道的 WiFi6 可另测 app/snapshot；记录应用实例及更新字段，列表收包计数只用于 widget/data。

   ```sh
   petui data-link runtime/widget.json receipts.json --board "$BOARD_ID" --boot "$BOOT_ID"
   petui first-data python.hachiapp --fixture sample.json
   ```

6. **验证更新和卸载。** 在测试包中写入存档，记录当前版本和值。从 Pet Manager 选同 ID 的新版，先运行 `petui install-rehearsal old.hachiapp --upgrade new.hachiapp`，再按现有同步入口更新。板上核对目录版本、旧存档、新版主操作。Python 包另核对新版需重新授权。通过现有入口选择保留存档卸载（须有该能力），验板上目录移除，重装后读回旧值。再用该测试包明确选择清除存档卸载，重装后读到初始值。记录实际走 USB 或已配对 Wi-Fi。

每板每项保留“通过 / 失败 / 条件不足 / 未测”，附实际值、期望、时间、供电和 PC/AP 状态。提醒可用 [闹钟样例及实机动作表](capability-ext-hil.md) 的 alarm 包。SW1 设置 60 秒，退到主页；先观察本次开机未授权，再经既有 PC 策略入口明确授权，检查只出现一次通知。另开两轮，分别在到期前回组件按 SW2 取消、经现有同步入口卸载，越过原截止时间仍应无提醒。保留一次有效 UTC 下设定再断电的记录，新 boot 未授权时应保持等待。样例的 SW2 操作为一次性计时；排队后精确取消与删除的交错另由主机回归覆盖。

featureManifest v1 逐项列出生产路径和授权条件。featureStatus 给出目标板每条路径的证据；未知握手、缺少能力、Wi-Fi 已断开和联网状态待确认分别展示。partial-device-route 表示部分功能有设备路径。例如自选股的刷新、编辑与页面存档分别判断。声明和握手只能证明协议条件，取得新数据仍需回执。
