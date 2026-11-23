import { voiceSpecFor } from "./persona-voice.js";

// Share the exact normalized voice used by realtime chat; never send persona
// instructions, names, credentials or legacy per-appearance volume.
export function componentPetVoice(appearance, usb, wifi) {
  // wifi is the current authenticated status snapshot, never a saved pairing.
  const boardDeviceId = usb?.connected
    ? usb.boardDeviceId
    : wifi?.wifiConnected && wifi.controlTrusted && wifi.bootId && wifi.boardDeviceId;
  if (!boardDeviceId || !appearance?.id || !appearance.personaVoice) return null;
  const { speaker, cloneSpeakerId, speed, ttsOverrides } = voiceSpecFor(appearance.personaVoice);
  return {
    boardDeviceId,
    ...(usb?.connected
      ? usb.connectionId ? { route: { usb: String(usb.connectionId) } } : {}
      : { route: { wifi: wifi.bootId } }),
    appearanceId: appearance.id, speaker, cloneSpeakerId, speed,
    ...(ttsOverrides && Object.keys(ttsOverrides).length ? { ttsOverrides } : {}),
  };
}

// Single flight with latest-wins coalescing: an old update cannot finish after
// a newer pet or a disconnect. A failed call is retried by the next heartbeat.
export function createComponentVoiceSync(invoke) {
  let pending, busy = false;
  const drain = async () => {
    if (busy) return;
    busy = true;
    try {
      while (pending !== undefined) {
        const input = pending; pending = undefined;
        try { await invoke("component_speech_set_pet_voice", { input }); } catch { /* next heartbeat retries */ }
      }
    } finally { busy = false; }
  };
  return input => { pending = input; return drain(); };
}
