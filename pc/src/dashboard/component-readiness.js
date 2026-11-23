// Only combine observations from the selected board and the same boot.
// All display and clipboard strings are local; protocol replies can contain private data.
export function componentReadiness({ boardDeviceId, bootId, usb, wifi, lastResetReason }) {
  const matches = (source) => Boolean(boardDeviceId && typeof bootId === "string" && /^[0-9a-f]{16}$/.test(bootId)
    && source?.boardDeviceId === boardDeviceId && source.bootId === bootId);
  const caps = matches(usb) && usb.connected === true ? usb.capabilities : null;
  const lan = matches(wifi) && wifi.controlTrusted === true && wifi.wifiConnected === true ? wifi : null;
  const pico = caps?.firmwareUpdate?.hardwareBoardModel === "pico";
  const feature = (field, version) => caps?.[field] === version;
  const vault = typeof caps?.widgetCredentialStorageReady === "boolean" ? caps.widgetCredentialStorageReady
    : lan?.widgetCredentialStorageReady === true ? true : undefined;
  const rows = [];
  function row(id, label, state, detail) {
    rows.push({ id, label, state, detail });
  }
  function radioRow(id, label, state, detail) {
    if (pico) row(id, label, "本板不支持", "此功能需要 WiFi6 底板。");
    else row(id, label, state, detail);
  }
  const unknown = caps ? "固件未声明" : "未取得握手";
  radioRow("wlan", "设备 WLAN", lan ? "已认证连接" : "待确认",
    lan ? "已通过配对的局域网连接读到这台设备。" : "在设备的「WIFI 配置」查看连接状态，使用获准的 2.4 GHz 网络。");
  radioRow("clock", "设备校时", lan ? (lan.clockSynchronized ? "已同步" : "待校时") : "待确认",
    lan?.clockSynchronized ? "设备已报告网络校时成功。" : "联网后确认设备完成校时，再使用 HTTPS 服务。");
  const mic = feature("widgetMicLevel", "microphone-level-v1");
  row("mic", "麦克风电平", mic ? "可请求本次授权" : unknown, mic
    ? "打开电平组件，按设备 SW1 后比较安静和拍手时的读数。" : "检查设备音频初始化和组件电平能力。");
  const globalCues = feature("globalAudioCues", "p4-global-audio-cues-v1");
  const wirelessCues = !usb?.connected && lan?.globalAudioCues === true;
  const needsTrust = !usb?.connected && matches(wifi) && wifi.wifiConnected === true
    && wifi.globalAudioCues === true && wifi.controlTrusted !== true;
  const cueState = globalCues ? "支持 USB 独立下发" : wirelessCues ? "支持 Wi-Fi 独立下发"
    : needsTrust ? "需授权 Wi-Fi 控制" : caps ? "固件未声明" : "需 USB 握手";
  row("cues", "全局提示音下发", cueState, globalCues || wirelessCues
    ? "编辑全局提示音后，点击「下发音效」并等待设备确认。"
    : needsTrust ? "在设备上授权 Wi-Fi 控制，或连接 USB 下发。" : "可在电脑上编辑和试听。通过 USB 连接支持独立下发的固件后应用。");
  const ble = feature("widgetBleAccessory", "p4-ble-read-v1");
  radioRow("ble", "BLE 配件读取", ble ? "可请求本次授权" : unknown, ble
    ? "键盘空闲时，在前台组件选择已知的标准电池或温度配件，按设备 SW1 后核对读数。"
    : "核对 WiFi6 配件读取能力和已知的标准电池或温度配件。");
  row("cold", "独立供电启动", (caps || lan) && lastResetReason === "power_on" ? "上次为上电复位" : "待现场核对",
    "保存组件进度，在独立电源下冷启动，核对打开的组件与存档。");
  radioRow("vault", "私密凭据存储", vault === true ? "保护存储已就绪" : vault === false ? "保护存储未就绪" : "待确认",
    vault === true ? "在设备上授权私有数据源，核对重新打开后的读取结果。" : "使用已配置受保护密钥的测试板，在设备上核对私有数据源设置。");
  radioRow("community", "签名组件社区", "待设备验签",
    "连接 WLAN 并校时，在设备「组件社区」刷新目录。测试固件须信任该目录的签名公钥和 HTTPS 服务。");
  return rows;
}
