// https://mimo.mi.com/docs/en-US/quick-start/usage-guide/audio/speech-synthesis-v2.5
export const MIMO_TTS_VOICES = Object.freeze(["mimo_default", "冰糖", "茉莉", "苏打", "白桦", "Mia", "Chloe", "Milo", "Dean"]);

export function mimoVoicePresets(model) {
  return model === "mimo-v2.5-tts" ? MIMO_TTS_VOICES : [];
}

export function isMimoVoiceDesignModel(model) {
  return model === "mimo-v2.5-tts-voicedesign";
}

export function hasMimoVoiceDescription(value) {
  const description = typeof value === "string" ? value.trim() : "";
  return description !== "" && description !== "mimo_default";
}
