import { wifiDeviceControlReady } from "./wifi-device-control.js";
import { GUIDE_TEST_MESSAGE } from "./device-guide-content.js";

export function testMessageTarget(usb, wifi, boundBoard) {
  const current = usb?.connected ? usb : wifi;
  if (!current?.boardDeviceId || (boundBoard && current.boardDeviceId !== boundBoard)) return null;
  if (usb?.connected) {
    if (usb.boardWedged || !usb.connectionId) return null;
    const bootId = usb.bootId || usb.capabilities?.debugBootId;
    const legacy = usb.capabilities?.testMessageReceipt !== true;
    if (!legacy && !/^[a-f0-9]{16}$/i.test(bootId || "")) return null;
    return { boardDeviceId: usb.boardDeviceId, bootId: bootId || "", connectionId: usb.connectionId,
      legacy, usbAck: legacy && usb.runtime === "esp-p4" };
  }
  if (!wifi?.wifiConnected || !wifiDeviceControlReady(wifi)) return null;
  return { boardDeviceId: wifi.boardDeviceId, bootId: wifi.bootId, connectionId: null, legacy: false };
}

let pending = false;
export async function sendTestMessage(invoke, target) {
  if (!target) throw new Error("请连接已确认身份的 USB 或受信 Wi-Fi 设备。");
  if (pending) throw new Error("正在发送测试消息，请等待本次回执。");
  pending = true;
  try {
    const { boardDeviceId, bootId, connectionId } = target;
    if (target.legacy) {
      await invoke("usb_send_speech", { text: GUIDE_TEST_MESSAGE, expectedBoardDeviceId: boardDeviceId, expectedConnectionId: connectionId,
        expectedBootId: bootId || null, requestReceipt: target.usbAck });
      if (target.usbAck) return "设备已确认接收测试消息，请查看桌宠主屏。";
      return "已写入 USB；此固件不提供设备投递回执，请查看桌宠屏幕。";
    }
    await invoke("send_device_test_message", { boardDeviceId, bootId, connectionId });
    return "设备已确认接收测试消息，请查看桌宠主屏。";
  } finally {
    pending = false;
  }
}
