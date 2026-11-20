# 本地日用分支

日常构建在 `local/daily` 进行。桌面端和固件使用同一个日用提交。新功能和修复先提交到相应功能分支，通过检查后再合入日用分支。

| 分支 | 内容 |
| --- | --- |
| `main` | 跟踪官方 `origin/main` |
| `feat/codex-quota` | 额度看板、多账号、本机 Token 统计、设备显示与交互 |
| `feat/device-voice-feedback` | 按住说话时的波形、识别结果和草稿发送反馈 |
| `feat/wifi-settings` | 连接信息、密码显示、已保存网络和自动连接 |
| `feat/component-transport` | USB 与配对 Wi-Fi 上的组件消息，当前无线支持额度看板 |

桌面构建按项目的标准流程准备对应版本的资源，在 `pc` 目录使用 `npm run tauri -- build --config src-tauri/tauri.daily.conf.json`。此配置沿用历史日用版的 macOS 应用标识和名称，版本号随官方基线。设备固件按 `firmware/BUILD.md` 选择与实体设备匹配的目标。无线组件扩展在本机分支使用 mode 22，电脑和设备均需支持该扩展。

更新官方代码时先快进 `main`，分别在涉及的功能分支适配和测试，再合入 `local/daily`。向官方提交单项功能时从对应的功能分支准备 PR。使用 `git worktree list` 可查看各分支的工作目录。
