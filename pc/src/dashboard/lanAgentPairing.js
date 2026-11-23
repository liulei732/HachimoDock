import { invoke } from "@tauri-apps/api/core";

export const LAN_AGENT_PAIRING_UPDATED = "lan-agent-pairing-updated";

// Bound UI waits for read-only IPC. A late native reply cannot settle the next request.
export async function boundedLanRead(request, message, milliseconds = 30_000) {
  const deadline = Date.now() + milliseconds;
  let timer;
  try {
    const value = await Promise.race([request, new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error(message)), milliseconds);
    })]);
    // On wake, the IPC callback can run before an overdue JavaScript timer.
    if (Date.now() >= deadline) throw new Error(message);
    return value;
  } finally {
    clearTimeout(timer);
  }
}

export function matchesUsbPeer(usb, status) {
  return Boolean(usb?.connected && usb.connectionId && status?.boardDeviceId &&
    usb.boardDeviceId === status.boardDeviceId && (!usb.bootId || usb.bootId === status.bootId));
}

export function readLanAgentPairing() {
  return invoke("lan_agent_pair_status");
}

export function publishLanAgentPairing(address) {
  window.dispatchEvent(new CustomEvent(LAN_AGENT_PAIRING_UPDATED, { detail: address }));
}

export function pairLanAgent(ip, codeField) {
  const code = codeField.value;
  codeField.value = "";
  return invoke("lan_agent_pair", { ip, code });
}


export async function readRealtimeWifiBoard() {
  const { address, status, error } = await boundedLanRead(
    invoke("lan_connection_snapshot"), "Wi-Fi 状态读取超时，请刷新重试。",
  );
  if (address && !status) throw new Error(error || "已保存配对，当前无法读取 Wi-Fi 设备状态。");
  return status?.wirelessRealtime && status?.controlTrusted && status?.wifiConnected
    ? status.boardDeviceId || ""
    : "";
}
