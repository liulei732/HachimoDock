import { secretInputProps } from "./lib/secret-input.js";
import { useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { formatModelIssue } from "./lib/model-issue.js";
import { hasStageKey, readSpeechSnapshot, speechForm, speechSaveInput, updateSpeechField } from "./lib/custom-speech-settings.js";
import "./CustomSpeechSettings.css";

const titles = { asr: "语音识别（ASR）", tts: "语音合成（TTS）" };
const stages = Object.keys(titles);

function isEdited(form, saved) {
  return Boolean(form) && Object.entries(speechForm(saved)).some(([key, value]) => form[key] !== value);
}

export default function CustomSpeechSettings({ active = true, presetRevision = 0, onStateChange, onSaved }) {
  const [snapshot, setSnapshot] = useState(null);
  const saved = useRef(null);
  const [drafts, setDrafts] = useState({});
  const [messages, setMessages] = useState({});
  const [pending, setPending] = useState({});
  const locks = useRef(new Set());
  const [readError, setReadError] = useState("");
  const [refresh, setRefresh] = useState(0);
  const [readyRevision, setReadyRevision] = useState(null);
  const initialized = useRef(false);
  const mounted = useRef(false);
  const revision = useRef(presetRevision);
  useEffect(() => {
    mounted.current = true;
    return () => { mounted.current = false; };
  }, []);
  useEffect(() => {
    revision.current = presetRevision;
    setMessages({});
  }, [presetRevision]);

  // Start on first visit. Later preset saves refresh the active flags even if
  // this section is hidden. Toggling visibility alone never replaces a draft.
  const shouldLoad = active || initialized.current;
  useEffect(() => {
    if (!shouldLoad) return;
    initialized.current = true;
    let current = true;
    setReadError("");
    setReadyRevision(null);
    invoke("load_custom_speech_settings").then(readSpeechSnapshot).then(data => {
      if (!current) return;
      saved.current = data;
      setSnapshot(data);
      setReadyRevision(presetRevision);
    }).catch(error => {
      if (current) setReadError(formatModelIssue("storage", "自定义", error));
    });
    return () => { current = false; };
  }, [shouldLoad, presetRevision, refresh]);
  const ready = snapshot !== null && readyRevision === presetRevision;
  const dirty = stages.some(stage => isEdited(drafts[stage], snapshot?.[stage]));
  const busy = stages.some(stage => pending[stage]);
  useEffect(() => { onStateChange?.({ dirty, busy }); }, [dirty, busy, onStateChange]);

  function setMessage(stage, text, tone) {
    setMessages(current => ({ ...current, [stage]: { text, tone } }));
  }
  function edit(stage, field, value) {
    setDrafts(current => {
      const baseline = saved.current[stage];
      const next = updateSpeechField(stage, current[stage] || speechForm(baseline), field, value, baseline);
      return { ...current, [stage]: isEdited(next, baseline) ? next : null };
    });
    setMessage(stage, "", "");
  }

  async function submit(stage, save, test) {
    if (!active || !ready || locks.current.has(stage)) return;
    const form = drafts[stage] || speechForm(saved.current[stage]);
    let input;
    if (save) {
      try { input = speechSaveInput(stage, form, saved.current[stage]); }
      catch (error) { setMessage(stage, error.message, "warning"); return; }
    }
    locks.current.add(stage);
    setPending(current => ({ ...current, [stage]: true }));
    const current = () => mounted.current && revision.current === presetRevision;
    let phase = save ? "storage" : stage;
    let committed = false;
    try {
      if (save) {
        setMessage(stage, "正在保存…", "");
        const receipt = readSpeechSnapshot(await invoke("save_custom_speech_stage", { input }));
        if (!current()) return;
        // Overlapping saves may resolve out of order. Each receipt owns only its side.
        const merged = { ...saved.current, [stage]: receipt[stage] };
        saved.current = merged;
        setSnapshot(merged);
        setDrafts(previous => ({ ...previous, [stage]: null }));
        committed = true;
        onSaved?.(stage, merged);
      }
      if (test) {
        phase = stage;
        setMessage(stage, "正在测试已保存的服务…", "");
        const result = await invoke("test_custom_speech_stage", { stage });
        if (!current()) return;
        if (result?.stage !== stage || !Number.isFinite(result.latencyMs) || result.latencyMs < 0) {
          throw Error("测试回执无效");
        }
        setMessage(stage, (committed ? "已保存，" : "") + `测试通过（${result.latencyMs} ms）`, "success");
      } else setMessage(stage, "已保存。", "success");
    } catch (error) {
      if (current()) setMessage(stage, (committed ? "已保存，测试失败。" : phase === "storage" ? "保存失败。" : "测试失败。")
        + formatModelIssue(phase, "自定义", error), "error");
    } finally {
      locks.current.delete(stage);
      if (mounted.current) setPending(previous => ({ ...previous, [stage]: false }));
    }
  }

  return <div hidden={!active}>
    <p className="muted small">识别和合成可选不同服务，保存后该项立即生效。填写 OpenAI 兼容接口地址；使用 /chat/completions 接口时，请填写完整端点地址。测试会调用所选服务，可能产生费用。</p>
    {!ready && <div role="status">{readError || "正在读取语音配置…"}
      {readError && <button type="button" className="btn-secondary btn-sm" onClick={() => setRefresh(value => value + 1)}>重新读取</button>}
    </div>}
    {stages.map(stage => {
      const status = snapshot?.[stage];
      const form = drafts[stage] || speechForm(status);
      const needsSave = !status?.configured || isEdited(drafts[stage], status);
      const message = messages[stage];
      let stateLabel = "尚未配置此服务";
      if (status?.configured) stateLabel = status.active ? "正在使用此自定义服务" : "配置已保存，当前使用其他服务";
      const field = (key, label, placeholder) => <label className="ui-field">
        <span className="ui-field__label">{label}</span>
        <input className="ui-control" {...(key === "apiKey" ? secretInputProps : { type: "text", autoComplete: "off" })}
          value={form[key]} onChange={event => edit(stage, key, event.target.value)}
          spellCheck={false} placeholder={placeholder}/>
      </label>;
      return <fieldset key={stage} className="custom-speech__stage" disabled={!ready || pending[stage]}>
        <legend>{titles[stage]}</legend>
        <div className="api-settings__form api-settings__form--speech">
          {field("baseUrl", "接口地址", "https://api.example.com/v1")}
          {field("model", "模型 ID", stage === "asr" ? "faster-whisper-large-v3" : "cosyvoice-300m-sft")}
          {stage === "tts" && field("voice", "合成音色", "服务支持的音色 ID 或音色描述")}
        </div>
        <div className="api-settings__form api-settings__speech-actions">
          {field("apiKey", "API Key", hasStageKey(stage, form, status) ? "此接口已保存 Key，可留空" : "填写此接口的 API Key")}
          <button type="button" className="btn-secondary btn-sm" onClick={() => submit(stage, true, false)}>仅保存</button>
          <button type="button" className="btn-primary btn-sm" onClick={() => submit(stage, needsSave, true)}>
            {needsSave ? "保存并测试" : "测试已保存配置"}
          </button>
        </div>
        <div role="status" className={`api-settings__result${message?.tone ? " is-" + message.tone : ""}`}>
          {ready && <span>{stateLabel}</span>}
          {message?.text && <span>{message.text}</span>}
          {!message?.text && drafts[stage] && <span>有未保存的更改</span>}
        </div>
      </fieldset>;
    })}
  </div>;
}
