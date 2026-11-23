/**
 * [Input] One appearance record (with its normalized `personaVoice`), an optional
 *         `required` flag for the post-creation step, and the persona/voice helpers in
 *         `lib/persona-voice.js`.
 * [Output] The 人设与声音 modal: persona form (name, intro, style, address, language,
 *          forbidden, greeting) with one-tap templates, service-scoped MiMo/custom voices and Doubao Seed TTS 2.0 voice picker
 *          (complete official catalog with language groups/search, or a cloned speaker id,
 *          speed, volume), validation, save via
 *          `savePersonaVoice`, a "restore factory persona" action for the built-in Terrier,
 *          current-key chat instructions, API navigation without discarding drafts,
 *          shared realtime volume with native persistence, and a preview button that refreshes after speech configuration changes.
 * [Pos] component node in ref/src, opened from AppearanceGallery cards and creation flows.
 * [Sync] If this file changes, update this header and `ref/src/.folder.md`.
 */

import { formatModelIssue } from "./lib/model-issue.js";
import { speechModelMessage } from "./lib/realtime-feedback.js";
import React, { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { AlertCircle, Loader, Save, Volume2, WandSparkles, X } from "lucide-react";
import { invoke } from "@tauri-apps/api/core";
import { ask } from "@tauri-apps/plugin-dialog";
import { useDialogFocus } from "./component-center/use-dialog-focus.js";
import { useExitProtection } from "./lib/exit-protection.js";
import { savePersonaVoice } from "./lib/appearance-store.js";
import UsageHelp from "./UsageHelp.jsx";
import { usePersonaTtsSettings } from "./lib/use-persona-tts-settings.js";
import { mimoVoicePresets } from "./lib/mimo-tts-voices.js";
import {
  BUILTIN_TERRIER_PERSONA_VOICE,
  DOUBAO_SPEAKERS,
  PERSONA_LANGUAGE_OPTIONS,
  PERSONA_TEMPLATES,
  VOICE_SPEED_RANGE,
  VOICE_VOLUME_RANGE,
  createDefaultPersonaVoice,
  normalizePersonaVoice,
  personaTtsService,
  personaVoiceDescription,
  scopedPersonaVoice,
  previewTextFor,
  speakerOptionsFor,
  validatePersonaVoice,
  voiceSpecFor,
} from "./lib/persona-voice.js";

const PREVIEW_UNAVAILABLE_HINT = "请先在「模型配置 → 语音识别与合成」配置语音服务，再来试听。";

function AppearanceVoiceOverride({ service, voice, onChange, disabled }) {
  const value = scopedPersonaVoice(voice, service);
  const presets = service.provider === "mimo" ? mimoVoicePresets(service.scope.model) : [];
  const [manual, setManual] = useState(false);
  const custom = manual || (value && !presets.includes(value));
  const inherited = service.design ? personaVoiceDescription(service, service.globalVoice) || "尚未填写"
    : service.globalVoice || "服务默认值";
  return <>
    {presets.length > 0 && <label className="ui-field">
      <span className="ui-field__label">形象音色</span>
      <select className="field-input" disabled={disabled} value={custom ? "@manual" : value}
        onChange={event => {
          const selected = event.target.value;
          setManual(selected === "@manual");
          if (selected !== "@manual") onChange(selected);
        }}>
        <option value="">跟随全局（{inherited}）</option>
        {presets.map(id => <option key={id} value={id}>{id === "mimo_default" ? "使用 MiMo 默认音色" : id}</option>)}
        <option value="@manual">手动填写音色 ID</option>
      </select>
    </label>}
    {(!presets.length || custom) && <label className="ui-field">
      <span className="ui-field__label">{service.design ? "形象声音描述" : "形象音色 ID"}</span>
      {service.design
        ? <textarea className="field-input" rows={3} value={value} disabled={disabled}
            placeholder="例如：温和的青年男声，语速慢，句尾轻柔" onChange={e => onChange(e.target.value)} />
        : <input className="field-input" value={value} disabled={disabled}
            placeholder="输入此服务支持的音色 ID" onChange={e => onChange(e.target.value)} />}
      <span className="muted small">留空跟随全局。当前全局{service.design ? "描述" : "音色"}：{inherited}</span>
    </label>}
  </>;
}

async function playPersonaPreview(form, signal) {
  const previewId = await invoke("reserve_persona_voice_preview");
  const cancel = () => invoke("cancel_persona_voice_preview", { previewId });
  if (signal.aborted) { await cancel(); return; }
  const stop = () => { cancel().catch(() => console.warn("试听停止指令未确认")); };
  signal.addEventListener("abort", stop, { once: true });
  try {
    await invoke("persona_voice_preview", {
      previewId, input: { text: previewTextFor(form.persona), voice: voiceSpecFor(form) },
    });
  } finally { signal.removeEventListener("abort", stop); }
}

function initialFormState(appearance) {
  const normalized = appearance?.personaVoice
    || normalizePersonaVoice(null, {
      name: appearance?.name,
      description: appearance?.description,
      isBuiltin: appearance?.type === "builtin",
    });
  return { persona: { ...normalized.persona }, voice: { ...normalized.voice } };
}

export default function PersonaVoiceModal({ appearance, required = false, onClose, onSaved, onOpenApiSettings, previewAvailable: previewAvailableProp, onPreview = playPersonaPreview }) {
  const isBuiltin = appearance?.type === "builtin";
  const [form, setForm] = useState(() => initialFormState(appearance));
  const [savedForm, setSavedForm] = useState(() => initialFormState(appearance));
  const [confirmingClose, setConfirmingClose] = useState(false);
  const { status, error: previewReadError, loading } = usePersonaTtsSettings();
  const service = personaTtsService(status);
  const cloneNeedsSample = service?.provider === "mimo" && service.scope?.model === "mimo-v2.5-tts-voiceclone";
  const effectiveVoice = scopedPersonaVoice(form.voice, service).trim() || service?.globalVoice;
  const missingDescription = service?.design && !personaVoiceDescription(service, effectiveVoice);
  const previewReady = status?.ttsConfigured === true || (status?.ttsConfigured == null
    && previewAvailableProp === true && (!previewReadError || onPreview !== playPersonaPreview));
  const previewAvailable = !loading && !cloneNeedsSample && !missingDescription && previewAvailableProp !== false && previewReady;
  const previewHint = previewReadError || (cloneNeedsSample ? "此模型需要参考音频"
    : missingDescription ? "请填写形象声音描述，或在模型配置中设置全局描述。"
    : loading ? "正在读取合成服务…" : PREVIEW_UNAVAILABLE_HINT);
  const [errors, setErrors] = useState({});
  const [saving, setSaving] = useState(false);
  const [saveError, setSaveError] = useState("");
  const [previewing, setPreviewing] = useState(false);
  const previewOwner = useRef(null);
  const cancelPreview = useCallback(() => {
    previewOwner.current?.abort();
    previewOwner.current = null;
  }, []);
  useEffect(() => {
    setPreviewing(false);
    return cancelPreview;
  }, [appearance?.id, cancelPreview]);
  const [volumeReady, setVolumeReady] = useState(false);
  useEffect(() => {
    let disposed = false;
    invoke("load_realtime_voice_volume").then((volume) => {
      if (!Number.isFinite(volume) || volume < VOICE_VOLUME_RANGE.min || volume > VOICE_VOLUME_RANGE.max) {
        throw new Error("全局音量读取失败，请关闭后重试");
      }
      if (!disposed) {
        setForm((current) => ({ ...current, voice: { ...current.voice, volume } }));
        setSavedForm((current) => ({ ...current, voice: { ...current.voice, volume } }));
        setVolumeReady(true);
      }
    }).catch((error) => { if (!disposed) setSaveError(formatModelIssue("storage", "本机全局音量", error)); });
    return () => { disposed = true; };
  }, []);

  const [speakerQuery, setSpeakerQuery] = useState("");
  const speakerOptions = useMemo(() => speakerOptionsFor(form.voice.speaker, speakerQuery), [form.voice.speaker, speakerQuery]);
  const speakerGroups = useMemo(() => {
    const groups = new Map();
    for (const speaker of speakerOptions) {
      const language = speaker.language || "其他";
      if (!groups.has(language)) groups.set(language, []);
      groups.get(language).push(speaker);
    }
    return [...groups];
  }, [speakerOptions]);

  const updatePersona = useCallback((key, value) => {
    setForm((current) => ({ ...current, persona: { ...current.persona, [key]: value } }));
    setErrors((current) => (current[key] ? { ...current, [key]: undefined } : current));
  }, []);

  const updateVoice = useCallback((key, value) => {
    setForm((current) => ({ ...current, voice: { ...current.voice, [key]: value } }));
    setErrors((current) => (current.speaker ? { ...current, speaker: undefined } : current));
  }, []);

  function updateOverride(value) {
    const overrides = { ...form.voice.tts_overrides };
    if (value) overrides[service.provider] = { ...service.scope, voice: value };
    else delete overrides[service.provider];
    updateVoice("tts_overrides", Object.keys(overrides).length ? overrides : undefined);
    setErrors(current => ({ ...current, tts_overrides: undefined }));
  }

  const applyTemplate = useCallback((template) => {
    setForm((current) => ({
      ...current,
      persona: {
        ...current.persona,
        style: template.style,
        address: template.address,
        forbidden: template.forbidden,
      },
    }));
  }, []);

  const restoreFactory = useCallback(() => {
    const defaults = isBuiltin
      ? { persona: { ...BUILTIN_TERRIER_PERSONA_VOICE.persona }, voice: { ...BUILTIN_TERRIER_PERSONA_VOICE.voice } }
      : createDefaultPersonaVoice({ name: appearance?.name, description: appearance?.description });
    setForm((current) => ({ ...defaults, voice: { ...defaults.voice, volume: current.voice.volume } }));
    setErrors({});
  }, [appearance?.description, appearance?.name, isBuiltin]);

  const handleSave = useCallback(async () => {
    if (saving || !volumeReady) return;
    const nextErrors = validatePersonaVoice(form);
    if (Object.keys(nextErrors).length > 0) {
      setErrors(nextErrors);
      return;
    }
    setSaving(true);
    cancelPreview();
    setPreviewing(false);
    setSaveError("");
    let volumeSaved = false;
    let personaSaved = false;
    try {
      await invoke("save_realtime_voice_volume", { volume: form.voice.volume });
      volumeSaved = true;
      const saved = await savePersonaVoice(appearance.id, form);
      personaSaved = true;
      setSavedForm(form);
      await onSaved?.(saved);
    } catch (err) {
      console.error(err);
      setSaveError(`${personaSaved ? "配置已保存，但界面刷新失败，请重新打开。" : volumeSaved
        ? "全局音量已保存，但人设与音色未完成保存，请重试。" : "保存失败："}${err?.message || String(err)}`);
    } finally {
      setSaving(false);
    }
  }, [appearance?.id, form, onSaved, saving, volumeReady, cancelPreview]);

  const handlePreview = useCallback(async () => {
    if (!previewAvailable || previewOwner.current || !onPreview || !volumeReady) return;
    const { tts_overrides } = validatePersonaVoice(form);
    if (tts_overrides) {
      setErrors(current => ({ ...current, tts_overrides }));
      return;
    }
    const owner = new AbortController();
    previewOwner.current = owner;
    setPreviewing(true);
    setSaveError("");
    try {
      await onPreview(form, owner.signal);
    } catch (err) {
      if (!owner.signal.aborted) setSaveError(speechModelMessage(err));
    } finally {
      if (previewOwner.current === owner) {
        previewOwner.current = null;
        setPreviewing(false);
      }
    }
  }, [form, onPreview, previewAvailable, volumeReady]);

  const closeAllowed = !required && !saving;
  useExitProtection(saving ? "人设与声音正在保存" : JSON.stringify(form) !== JSON.stringify(savedForm)
    ? "人设与声音有未保存的修改" : "");
  const requestClose = useCallback(async () => {
    if (required || saving || confirmingClose) return;
    if (JSON.stringify(form) !== JSON.stringify(savedForm)) {
      setConfirmingClose(true);
      try {
        const discard = window.__TAURI_INTERNALS__
          ? await ask("人设与声音有未保存的修改，确定放弃吗？", {
            title: "未保存的修改", kind: "warning", okLabel: "放弃修改", cancelLabel: "继续编辑",
          })
          : window.confirm("人设与声音有未保存的修改，确定放弃吗？");
        if (!discard) return;
      } catch {
        setSaveError("无法确认退出，请保存修改后再关闭。");
        return;
      } finally { setConfirmingClose(false); }
    }
    cancelPreview();
    onClose?.();
  }, [required, saving, confirmingClose, form, savedForm, onClose, cancelPreview]);
  const dialogRef = useDialogFocus(requestClose, !closeAllowed || confirmingClose);

  return (
    <div className="modal-backdrop" onClick={closeAllowed ? requestClose : undefined}>
      <div ref={dialogRef} role="dialog" aria-modal="true" aria-label={`人设与声音 · ${appearance?.name || "宠物"}`} tabIndex={-1} className="modal-card modal-card--wide persona-voice-modal" onClick={(event) => event.stopPropagation()}>
        <div className="modal-header">
          <div>
            <h3 className="modal-title">人设与声音 · {appearance?.name}</h3>
            <div className="modal-subtitle">
              {required
                ? "先为新形象设置人设与声音，让它在「实时对话」中按你喜欢的方式聊天。"
                : "设置它的名字、性格和声音，让它在「实时对话」中按你喜欢的方式聊天。"}
            </div>
          </div>
          {!required && (
            <button className="icon-btn" type="button" onClick={requestClose} disabled={saving || confirmingClose} aria-label="关闭">
              <X size={16} />
            </button>
          )}
        </div>
        <div className="modal-body">
          <UsageHelp onOpenApiSettings={onOpenApiSettings} />
          {!previewAvailable && onOpenApiSettings && <button type="button" className="btn-ghost btn-sm" onClick={onOpenApiSettings}>配置语音服务</button>}
          <div className="persona-voice-modal__grid">
            <section className="persona-voice-modal__column">
              <div className="persona-voice-modal__section-head">
                <strong>人设</strong>
                <div className="persona-voice-modal__templates" aria-label="人设模板">
                  {PERSONA_TEMPLATES.map((template) => (
                    <button
                      key={template.id}
                      type="button"
                      className="btn-ghost btn-sm"
                      onClick={() => applyTemplate(template)}
                      disabled={saving}
                    >
                      <WandSparkles size={12} /> {template.label}
                    </button>
                  ))}
                </div>
              </div>
              <label className="ui-field">
                <span className="ui-field__label">名字</span>
                <input
                  className="field-input"
                  value={form.persona.display_name}
                  onChange={(event) => updatePersona("display_name", event.target.value)}
                  placeholder="它怎么称呼自己"
                  disabled={saving}
                />
                {errors.display_name && <span className="ui-field__error">{errors.display_name}</span>}
              </label>
              <label className="ui-field">
                <span className="ui-field__label">一句话自我介绍</span>
                <textarea
                  className="field-input"
                  rows={2}
                  value={form.persona.intro}
                  onChange={(event) => updatePersona("intro", event.target.value)}
                  placeholder="它是谁、住在哪、最喜欢做什么"
                  disabled={saving}
                />
                {errors.intro && <span className="ui-field__error">{errors.intro}</span>}
              </label>
              <label className="ui-field">
                <span className="ui-field__label">性格与说话风格</span>
                <textarea
                  className="field-input"
                  rows={3}
                  value={form.persona.style}
                  onChange={(event) => updatePersona("style", event.target.value)}
                  placeholder="语气、口头禅、被夸/被冷落时的反应"
                  disabled={saving}
                />
                {errors.style && <span className="ui-field__error">{errors.style}</span>}
              </label>
              <div className="persona-voice-modal__row">
                <label className="ui-field">
                  <span className="ui-field__label">怎么称呼用户</span>
                  <input
                    className="field-input"
                    value={form.persona.address}
                    onChange={(event) => updatePersona("address", event.target.value)}
                    placeholder="例如：叫用户「主人」"
                    disabled={saving}
                  />
                </label>
                <label className="ui-field">
                  <span className="ui-field__label">语言</span>
                  <select
                    className="field-input"
                    value={form.persona.language}
                    onChange={(event) => updatePersona("language", event.target.value)}
                    disabled={saving}
                  >
                    {PERSONA_LANGUAGE_OPTIONS.map((option) => (
                      <option key={option.id} value={option.id}>{option.label}</option>
                    ))}
                  </select>
                </label>
              </div>
              <label className="ui-field">
                <span className="ui-field__label">禁止事项</span>
                <input
                  className="field-input"
                  value={form.persona.forbidden}
                  onChange={(event) => updatePersona("forbidden", event.target.value)}
                  placeholder="用分号隔开"
                  disabled={saving}
                />
              </label>
              <label className="ui-field">
                <span className="ui-field__label">开场白</span>
                <input
                  className="field-input"
                  value={form.persona.greeting}
                  onChange={(event) => updatePersona("greeting", event.target.value)}
                  placeholder="进入实时对话时它先说的一句话"
                  disabled={saving}
                />
              </label>
            </section>

            <section className="persona-voice-modal__column">
              <div className="persona-voice-modal__section-head">
                <strong>声音</strong>
                <span className="muted small">{service?.label || (loading ? "正在读取合成服务…" : "当前合成服务未知")}</span>
              </div>
              {service?.provider === "volcengine" && <>
                <label className="ui-field">
                  <span className="ui-field__label">搜索音色 · 共 {DOUBAO_SPEAKERS.length} 种</span>
                  <input className="field-input" type="search" value={speakerQuery}
                    onChange={(event) => setSpeakerQuery(event.target.value)}
                    placeholder="搜索名称、语言或音色 ID" disabled={saving} />
                </label>
                <label className="ui-field">
                  <span className="ui-field__label">音色</span>
                  <select
                    className="field-input"
                    value={form.voice.speaker}
                    onChange={(event) => updateVoice("speaker", event.target.value)}
                    disabled={saving}
                  >
                    {speakerGroups.map(([language, speakers]) => (
                      <optgroup key={language} label={language}>
                        {speakers.map((speaker) => <option key={speaker.id} value={speaker.id}>{speaker.label}</option>)}
                      </optgroup>
                    ))}
                  </select>
                  {speakerQuery.trim() && speakerOptions.every((speaker) => speaker.language === "当前音色") && (
                    <span className="muted small">未找到匹配音色，已保留当前选择。</span>
                  )}
                  {errors.speaker && <span className="ui-field__error">{errors.speaker}</span>}
                </label>
                <label className="ui-field">
                  <span className="ui-field__label">复刻音色 ID（可选，填了优先）</span>
                  <input
                    className="field-input"
                    value={form.voice.clone_speaker_id}
                    onChange={(event) => updateVoice("clone_speaker_id", event.target.value)}
                    placeholder="火山控制台声音复刻页申请所得，形如 S_xxxx"
                    disabled={saving}
                  />
                </label>
              </>}
              {service && service.provider !== "volcengine" && (cloneNeedsSample
                ? <p className="muted small">此模型需要参考音频。请在模型配置中选择预设音色或声音设计模型。</p>
                : service.scope
                  ? <AppearanceVoiceOverride key={JSON.stringify([service.provider, service.scope])}
                      service={service} voice={form.voice} onChange={updateOverride} disabled={saving || loading} />
                  : <p className="muted small">跟随全局音色。请先在模型配置中补全合成模型和接口。</p>)}
              {errors.tts_overrides && <span className="ui-field__error">{errors.tts_overrides}</span>}
              <div className="persona-voice-modal__row">
                {service?.provider === "volcengine" && <label className="ui-field">
                  <span className="ui-field__label">语速 <em className="muted">{Number(form.voice.speed).toFixed(2)}</em></span>
                  <input
                    className="persona-voice-modal__range"
                    type="range"
                    min={VOICE_SPEED_RANGE.min}
                    max={VOICE_SPEED_RANGE.max}
                    step={VOICE_SPEED_RANGE.step}
                    value={form.voice.speed}
                    onChange={(event) => updateVoice("speed", Number(event.target.value))}
                    disabled={saving}
                  />
                </label>}
                <label className="ui-field">
                  <span className="ui-field__label">全局音量 <em className="muted">{volumeReady ? Number(form.voice.volume).toFixed(2) : "读取中"}</em></span>
                  <input
                    className="persona-voice-modal__range"
                    type="range"
                    min={VOICE_VOLUME_RANGE.min}
                    max={VOICE_VOLUME_RANGE.max}
                    step={VOICE_VOLUME_RANGE.step}
                    value={form.voice.volume}
                    onChange={(event) => updateVoice("volume", Number(event.target.value))}
                    disabled={saving || !volumeReady}
                  />
                  <span className="muted">所有形象共用。保存后用于实时对话，不影响提示音和组件音效。</span>
                </label>
              </div>
              <div className="persona-voice-modal__preview">
                <button
                  type="button"
                  className="btn-secondary btn-sm"
                  onClick={handlePreview}
                  disabled={!previewAvailable || previewing || saving || !volumeReady}
                  title={previewAvailable ? "用当前人设和音色生成一句问候并播放" : previewHint}
                >
                  {previewing ? <Loader size={14} className="spin" /> : <Volume2 size={14} />} 试听
                </button>
                {previewing && <button type="button" className="btn-ghost btn-sm"
                  onClick={() => { cancelPreview(); setPreviewing(false); }}>停止试听</button>}
                <span className="muted small">
                  {previewAvailable ? "点击试听听听效果。外语音色建议使用对应语言的开场白。" : previewHint}
                </span>
              </div>
            </section>
          </div>

          {saveError && (
            <div className="message-banner message-banner--error">
              <AlertCircle size={14} /> {saveError}
            </div>
          )}
        </div>
        <div className="modal-footer persona-voice-modal__footer">
          <button type="button" className="btn-ghost btn-sm" onClick={restoreFactory} disabled={saving}>
            {isBuiltin ? "恢复出厂人设" : "恢复默认"}
          </button>
          <div className="persona-voice-modal__footer-actions">
            {!required && (
              <button type="button" className="btn-ghost" onClick={requestClose} disabled={saving || confirmingClose}>
                取消
              </button>
            )}
            <button type="button" className="btn-primary" onClick={handleSave} disabled={saving || !volumeReady}>
              {saving ? <Loader size={14} className="spin" /> : <Save size={14} />} {required ? "保存并继续" : "保存"}
            </button>
          </div>
        </div>
      </div>
    </div>
  );
}
