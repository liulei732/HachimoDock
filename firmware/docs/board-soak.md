# 四板真机长稳验收

WiFi6 v1、v3 各跑 8 小时，100 次形象切换分散在整个窗口，每小时至少一次混合负载观测。Pico v1、v3 各跑本地及 USB 烟测，初始基线采用 15 分钟。按实际装配记录屏型。每块板、每个候选 ELF、每次开机单独建档。

## 采样与现场操作

在确认空闲的 USB-UART 口上，采集器只发带请求 ID 的 `debug/identity` 和 `diagnostics/query`。macOS 同时查 cu、tty 两个设备节点，独占打开并抑制 DTR/RTS 设置。文件逐行落盘，超时保留已采样记录。默认 481 个样本，相邻查询间隔 60 秒。Pico 可用 16 个样本。

```sh
python3 firmware/tools/board_soak.py capture --port "$port" --output usb.jsonl
```

使用桌面端接管 USB 时先结束独立采集。可以分段释放和重新打开空闲端口，也可以由占用连接的验收客户端用同一对关联响应调用 `board_soak.observation(identity, diagnostics)` 生成 JSONL。连续比较要求同一主机时钟、同一板/启动指纹和候选 ELF，板端与主机采样间隙均至多 120 秒。原配对必须继续通过认证；断网试验在专用测试网络进行。

| 对象 | 每轮现场动作及记录 |
|---|---|
| 四板 | 本地切换形象直至新动画出帧，操作按键并核对页面响应，播放组件音频并退出，核对显示恢复及已停止的音频。单独记录 USB 重连到首次有效响应的毫秒数。 |
| WiFi6 | 增加语音往返、断网恢复、测试路由器重启、USB 接管。以恢复指定功能的首次成功响应作为结束点，记录每次耗时。每段音频从该流的计数器记录欠载次数，跨流重置单独计。 |
| Pico | 本地动画、输入、组件音频和原有 USB 客户端的重连均须覆盖。 |

现场记录使用一个 JSON 对象。用首个样本的 `fingerprint`、`bootId` 和 `elf` 绑定实机，`panel` 取 ST7701S 或 NV3049F。记录实测 `unrecoveredDisplays` 及持续动画时最长可见无帧时间 `maxVisibleGapMs`（可用屏幕录像逐帧计时）。诊断中的 `renderGapUs` 是开机以来渲染端最大间隙，两种测量分别保留。

`events` 按板端 uptimeMs 升序逐次填写，成功和失败都要记录。例：

```json
{"kind":"usb-reconnect","uptimeMs":75012,"ok":true,"recoveryMs":840}
```

形象、动画、按键、音频退出和 USB 重连分别用 `appearance`、`animation`、`button`、`audio-exit`、`usb-reconnect`。WiFi6 另用 `voice`、`wifi-loss`、`router-restart`、`usb-takeover`。每次恢复填 `recoveryMs`，每段语音或退出音频填 `underruns`。缺测字段留缺，零表示确实观测为零。只保存数值与技术身份，无需采集语音、账号、文本或凭证。

```sh
python3 firmware/tools/board_soak.py assess wifi6-v3 --image "$candidate_app" \
  --samples first.jsonl second.jsonl --witness observation.json
```

评估输出最小内部内存与 DMA、渲染端间隙、可见无帧时间、音频欠载和恢复耗时 P95。缺少候选实机时输出 pending-device。短测、串板、旧镜像、重启、无恢复显示、音频欠载及场景缺测均无法通过。P95 与间隙先积累同屏同连接的可比实测基线。

## 应用 OTA 与回退另行验收

在获准操作的测试板上，先用既有配对验证一次已认证只读请求，记录候选前运行 ELF 和分区，以及可重复核对的测试存档值。通过应用 OTA 升级到匹配芯片的候选；同版本标签时必须核对精确 ELF。启动确认后，复查 ELF、活动槽、原配对认证及同一存档。用受支持的旧版回退路径回到原 ELF，重复相同核对。报告只记录匿名板指纹、两端镜像哈希、分区及各项比对结果。仅静态分区核对或组件清单一致不足以证明存档保留。
