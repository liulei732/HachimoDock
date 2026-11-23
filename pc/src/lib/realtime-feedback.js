import { formatModelIssue } from "./model-issue.js";

// An abandoned ASR turn is not a failed conversation. Do not hide other errors.
export function isRecoveringAsr(status) {
  return status?.active === true && status?.state === "listening"
    && status?.reason === "asr_timeout"
    && (!status.error || status.error.endsWith("仍在聆听，请再说一次。"));
}

// The native event can arrive after the user switches provider. Use only
// operation labels emitted by the failing call, never the selected settings,
// agent name, current UI state, or an HTTP code by itself.
export function speechModelMessage(error, knownStage) {
  const message = typeof error === "string" ? error : error?.message || "";
  if (!message) return "";
  const detail = message.replace(/^云端 ASR 需要用户处理：(请检查语音识别配置、授权和额度后重新进入。)?/, "");
  if (/^\[(?:ASR|TTS|LLM|CREDENTIALS|NETWORK)\]/.test(detail)) {
    return formatModelIssue("", "", detail);
  }
  if (/^(?:连接火山引擎 ASR|语音识别服务(?:返回错误|等待音频超时).*?（provider=volcengine,)/.test(detail))
    return formatModelIssue("asr", "火山引擎", detail);
  if (/^(?:自定义语音识别|语音识别(?:失败（|请求失败|响应格式无效))/.test(detail))
    return formatModelIssue("asr", "自定义", detail);
  if (/^豆包 TTS/.test(detail)) return formatModelIssue("tts", "豆包", detail);
  if (/^(?:自定义语音合成|语音合成(?:请求失败|失败（|响应格式无效))/.test(detail))
    return formatModelIssue("tts", "自定义", detail);
  if (/^(?:对话大模型|大模型输出达到|大模型未(?:完成请求|正常完成))/.test(detail))
    return formatModelIssue("chat", "", detail);
  if (knownStage === "asr") return formatModelIssue("asr", "", detail);
  if (knownStage === "tts") return formatModelIssue("tts", "", detail);
  if (knownStage === "chat") return formatModelIssue("chat", "", detail);
  if (/\bHTTP(?:\/\d(?:\.\d)?)?\s*[:：]?\s*[45]\d\d\b|Bearer\s|https?:\/\//i.test(detail))
    return formatModelIssue("", "", detail);
  return message;
}

export function realtimeErrorMessage(status) {
  if (isRecoveringAsr(status)) return speechModelMessage(status.error?.replace(/。仍在聆听，请再说一次。$/, ""), "asr") + " 仍在聆听，请再说一次。";
  return speechModelMessage(status?.error, status?.errorStage || status?.reason);
}
