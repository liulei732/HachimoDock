export function deviceVoicePlatformWarning(agentId, platform) {
  if (agentId === "antigravity" && platform !== "macos") return "Antigravity 按键语音输入目前支持 macOS 桌面版。";
  if (agentId === "mimocode" && platform !== "macos") {
    return "MiMoCode 的按键语音输入目前仅支持 macOS 终端；当前系统可使用状态跟随与设备实时对话。";
  }
  if (agentId === "devin" && platform !== "windows") {
    return "Devin 的按键语音输入目前仅支持 Windows Devin Desktop；macOS 暂不支持。";
  }
  return "";
}
export function deviceVoiceTransportReady(agentId, platform, cliReady) {
  // macOS Codex drafts use the native desktop input path. CLI readiness
  // describes a different transport; native permission/window checks run at use.
  if (agentId === "antigravity") return platform === "macos" && cliReady === true;
  if (agentId === "devin") return platform === "windows";
  return (platform === "macos" && agentId === "codex")
    || cliReady === true;
}
