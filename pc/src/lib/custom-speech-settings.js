// Keep credential identity in step with voice_chat_settings::custom_speech_endpoint.
const chatHosts = new Set(["api.xiaomimimo.com", "token-plan-cn.xiaomimimo.com",
  "token-plan-sgp.xiaomimimo.com", "token-plan-ams.xiaomimimo.com"]);

export function speechRequestUrl(stage, value) {
  if (stage !== "asr" && stage !== "tts") return "";
  try {
    const url = new URL(value.trim());
    const loopback = url.hostname === "localhost" || url.hostname === "[::1]" || /^127(?:\.\d{1,3}){3}$/.test(url.hostname);
    if ((url.protocol !== "https:" && !(loopback && url.protocol === "http:"))
      || url.username || url.password || /[?#]/.test(url.href)) return "";
    const address = url.href.replace(/\/+$/, "");
    if (address.endsWith("/chat/completions")) return address;
    const audio = /\/audio\/(?:speech|transcriptions)$/.test(address);
    let root = audio ? address.replace(/\/audio\/(?:speech|transcriptions)$/, "") : address;
    if (!audio && !root.endsWith("/v1")) root += "/v1";
    if (!audio && chatHosts.has(url.hostname)) return root + "/chat/completions";
    return root + (stage === "asr" ? "/audio/transcriptions" : "/audio/speech");
  } catch { return ""; }
}

export function speechForm(status) {
  return { baseUrl: status?.baseUrl || "", model: status?.model || "", voice: status?.voice || "", apiKey: "" };
}

export function hasStageKey(stage, form, saved) {
  const endpoint = speechRequestUrl(stage, form.baseUrl);
  return Boolean(saved?.configured && endpoint && endpoint === speechRequestUrl(stage, saved.baseUrl));
}

export function updateSpeechField(stage, form, field, value, saved) {
  const next = { ...form, [field]: value };
  if (stage === "tts" && field === "voice") next.voiceDestination = speechRequestUrl(stage, form.baseUrl);
  if (field === "baseUrl" && value !== form.baseUrl) {
    const endpoint = speechRequestUrl(stage, value);
    if (!endpoint || endpoint !== speechRequestUrl(stage, form.baseUrl)) {
      next.apiKey = "";
      if (stage === "tts") {
        delete next.voiceDestination;
        next.voice = endpoint && endpoint === speechRequestUrl(stage, saved?.baseUrl) ? saved.voice : "";
      }
    }
  }
  return next;
}

export function speechSaveInput(stage, form, saved) {
  const destination = speechRequestUrl(stage, form.baseUrl);
  if (!destination) throw Error("请填写有效的 HTTPS 接口地址。本机服务可用 HTTP，地址中请勿携带密码或查询参数。");
  if (!form.model.trim()) throw Error("请填写此服务的模型 ID。");
  if (!form.apiKey.trim() && !hasStageKey(stage, form, saved)) throw Error("请填写此接口的 API Key。");
  const input = { stage, baseUrl: form.baseUrl.trim(), model: form.model.trim() };
  if (form.apiKey.trim()) input.apiKey = form.apiKey.trim();
  // Omission lets native retain the saved voice on this route or discard it
  // when switching routes. An explicit empty string clears the current voice.
  if (stage === "tts" && form.voiceDestination === destination) input.voice = form.voice.trim();
  return input;
}

export function readSpeechSnapshot(value) {
  const result = {};
  for (const stage of ["asr", "tts"]) {
    const row = value?.[stage];
    if (!row || typeof row.active !== "boolean" || typeof row.configured !== "boolean"
      || ["baseUrl", "model", "voice"].some(field => typeof row[field] !== "string")) {
      throw Error("语音配置回执不完整。");
    }
    const { baseUrl, model, voice, active, configured } = row;
    result[stage] = { baseUrl, model, voice, active, configured };
  }
  return result;
}
