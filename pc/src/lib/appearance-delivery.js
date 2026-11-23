export function appearanceTarget(usb, wifi, boundBoard) {
  if (usb?.connected) {
    if (!usb.boardDeviceId || !usb.connectionId || (boundBoard && boundBoard !== usb.boardDeviceId)) return null;
    return { transport: "usb", boardDeviceId: usb.boardDeviceId, connectionId: usb.connectionId,
      bootId: usb.bootId || usb.capabilities?.debugBootId || "" };
  }
  if (wifi?.wifiConnected !== true || wifi.controlTrusted !== true
      || !(Number.isSafeInteger(wifi.appearanceCapacity) && wifi.appearanceCapacity > 0)
      || !/^p4-[0-9a-f]{12}$/.test(wifi.boardDeviceId || "") || !/^[0-9a-f]{16}$/.test(wifi.bootId || "")
      || (boundBoard && boundBoard !== wifi.boardDeviceId)) return null;
  return { transport: "wifi", boardDeviceId: wifi.boardDeviceId, bootId: wifi.bootId };
}

export async function deliverAppearance(invoke, target, appearanceId, requestId) {
  const { boardDeviceId, bootId } = target;
  if (target.transport === "usb") {
    const result = await invoke("usb_sync_appearance", {
      appearanceId, boardDeviceId, requestId, expectedConnectionId: target.connectionId, expectedBootId: bootId,
    });
    if (result?.ok !== true) throw new Error(result?.error || "设备未确认形象同步");
    return result.reusedSlot ? "设备已确认现有形象素材。" : `${result.fileCount} 个素材，${result.byteCount} bytes`;
  }
  if (target.transport !== "wifi") throw new Error("形象传输目标无效。");
  const outcome = await invoke("lan_sync_appearance", { appearanceId, boardDeviceId, bootId, requestId });
  if (outcome === "uploaded") return "已通过 Wi-Fi 上传并核对设备上的形象。";
  if (outcome === "activated") return "设备已切换到现有素材，并已确认当前形象。";
  if (outcome === "alreadyActive") return "设备已核对，目标形象已在使用。";
  throw new Error("设备尚未确认形象同步结果。");
}

export function wifiAppearanceProgress(event, owner) {
  if (!owner.requestId || event?.requestId !== owner.requestId
      || event.boardDeviceId !== owner.boardDeviceId || event.bootId !== owner.bootId) return null;
  if (event.stage === "activating") {
    return { text: "正在激活设备上已有的形象…", indeterminate: true };
  }
  if (event.stage === "preparing") {
    const text = event.reason === "different_pack"
      ? "设备自定义槽中的素材包与所选版本不同，正在核对待传素材…"
      : "设备尚未缓存所选素材包，正在核对待传素材…";
    return { text, indeterminate: true };
  }
  const { bytesSent, bytesTotal } = event;
  if (event.stage !== "transferring" || !Number.isSafeInteger(bytesSent)
      || !Number.isSafeInteger(bytesTotal) || bytesSent <= 0 || bytesSent > bytesTotal) return null;
  if (bytesSent === bytesTotal) return { text: "素材已传完，正在校验并激活…", indeterminate: true };
  return { text: "正在通过 Wi-Fi 传输形象…", bytesSent, bytesTotal,
    percent: Math.floor(bytesSent / bytesTotal * 100) };
}
