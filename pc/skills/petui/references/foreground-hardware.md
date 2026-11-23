# 前台麦克风电平

在 v4 工具的 runtime/widget.json 中声明 `"hardware":{"service":"microphone-level-v1"}`。可选整数 quiet_below 取 1 到 100，表示电平达到该值时显示“超过阈值”。相对电平为 20 毫秒、16 kHz 单声道窗口中的绝对峰值，四舍五入到满量程的百分比。测量单位是满量程百分比。

原生前台页显示组件 ID、授权用途、读数和停止方法。设备支持条件是本次握手含 widgetMicLevel=microphone-level-v1，且系统麦克风可用。在公开社区详情或无目标创作环境看到功能声明时，仍需在目标板核对握手。

实体短按 SW1 允许本次前台运行，再按停止。系统返回键、实时会话和系统审批优先。若 SW1 短按已设为系统返回，仍执行返回。退出、切换、同 ID 替换、重启、失去前台或音频占用都会结束授权。忙态解除后重新短按才能开始。采集数据在原生音频工作线程内转换成 0–100 并清除。结果仅显示在本机前台。

包使用标准的组件、预览、按键、分享和 runtime/widget.json 文件。硬件工具的运行时只接受有界基础字段，用户操作走原生服务。两个完整源码示例分别显示峰值和安静阈值。[麦克风电平](../examples/mic-level-meter/runtime/widget.json) · [安静练习](../examples/quiet-room-check/runtime/widget.json)

从技能目录执行以下离线声明检查。主机结果中的 deviceVerified 保持 false。实机可在独立供电时用安静、说话和拍手观察读数，退出再打开应重新提示，将采集中途切换到语音对话应让出麦克风。

```sh
python3 scripts/smoke_test_widget_game.py examples/mic-level-meter --json
python3 scripts/smoke_test_widget_game.py examples/quiet-room-check --json
```
