// Validate the native input/config-state before reconciliation or automatic USB sync.
export function requireButtonReadback(reply, board) {
  const bindings = reply?.config?.bindings ?? reply?.bindings;
  if (!board || reply?.ok !== true || reply.boardDeviceId !== board ||
      !Array.isArray(bindings) || !bindings.length ||
      (reply.bindingCount != null && reply.bindingCount !== bindings.length) ||
      !bindings.every(row => typeof row?.event === "string" && row.event &&
        typeof row.action === "string" && row.action && (row.value == null || typeof row.value === "string")) ||
      new Set(bindings.map(row => row.event)).size !== bindings.length) {
    throw new Error("未收到当前设备的有效按键映射，请重新读取。");
  }
}
