# PetUI 桌面 Python 插件

Python 插件为声明式组件提供数据。脚本随 `.hachiapp` 安装，运行在桌面端，设备只收到绑定后的字段。使用新版桌面客户端，并用 `petui tool-info` 确认返回 `pythonPlugin: "petui-python-v1"`。PC 应用宿主通过 app/snapshot 交付字段。两类板均可用 USB；当前协议还可在 WiFi6 双端支持配对应用通道时通过受信 Wi-Fi 交付。两条路径都需要 PC 运行；https-json 由 PC 获取。

## 创作

在组件目录添加 `pc/plugin.py`。使用随包的 MicroPython 语言环境，脚本用 UTF-8 编码，大小小于 16 KiB。定义 `snapshot(data)`，返回 JSON 对象。返回字段可通过 composition 的 JSON Pointer 绑定到设备变量。

```python
def snapshot(data):
    readings = data if isinstance(data, list) else []
    return {"count": len(readings), "total": sum(readings)}
```

在 `pc/application.json` 的 `content[0].sources` 下声明一个 Python 数据源。

```json
{
  "metrics": {
    "kind": "python",
    "entry": "pc/plugin.py",
    "refresh_sec": 60,
    "input": {"kind": "none"}
  }
}
```

每个组件声明一个入口和一种输入。刷新间隔可设 30–86400 秒。物理刷新动作可映射为 `metrics:refresh`，限频 15 秒。
例如以 `total` 为设备整数变量，在 `bindings` 中添加 `"total":{"source":"metrics","pointer":"/total","fallback":0}`。

| 输入 | 交给脚本的值 | 用户授权 |
| --- | --- | --- |
| `{"kind":"none"}` | `None` | 允许执行 |
| `{"kind":"text-file"}` | UTF-8 文本，最多 12 KiB | 由用户选择一个普通文件。文件被替换后需重新选择 |
| `https-json` | 经过 JSON Pointer 投影的公开 HTTPS 响应 | 允许访问声明的域名 |

使用 HTTPS 时，把 `input` 声明为 `{"kind":"https-json","endpoint":"https://example.org/data.json","hosts":["example.org"],"pointer":"/readings"}`，把示例地址换成作者实际控制或获准访问的公开 API。客户端沿用公开 HTTPS GET 的域名、重定向、DNS 和响应大小限制。

Python 只能处理传入的数据，返回用于展示的 JSON。可使用基础 Python、`json` 和 `math`。首版支持单文件入口和随包标准模块。脚本在独立进程里的 WebAssembly 内运行，每次创建新实例，使用固定内存和执行时限。

## 预览、安装与发布

1. 创建普通声明式组件，保留 `runtime/widget.json`、`buttons.json`、`pc/cover.png` 等常规文件。为每个绑定提供等待授权和运行失败时可用的 fallback。
2. 用示例数据验收。例如将 `[1,2,3]` 写入包外的 `sample.json`，执行下列命令并核对真实返回值。

   ```sh
   petui pack package plugin.hachiapp
   petui validate plugin.hachiapp
   petui python-preview plugin.hachiapp sample.json
   petui publish plugin.hachiapp
   ```

3. 打开「我的组件」中的详情，使用「Python 数据插件」区检查授权文案。启用后验证数值和更新，撤销后验证设备恢复 fallback。升级包后验证旧授权失效。
4. 社区发布时继续走预检和审核。预检检查入口、声明、脚本长度和内容，审核服务不会执行脚本。权限和本机文件选择在用户电脑上单独保存。同步设备时只发送设备资源。

脚本依赖的本机文件或在线服务应写明用途和可用平台。使用独立的无隐私示例来生成封面和预览。
