import { secretInputProps } from "./lib/secret-input.js";
/**
 * [Input] Shared avatar provider configs including persisted video parameters, native speech configuration and independent ASR/TTS probes, and optional return navigation.
 * [Output] Model settings with editable MiMo speech options, independent custom speech stages and per-stage test results.
 * [Pos] top-level page node in pc/src
 * [Sync] If this file changes, update this header and `pc/src/.folder.md`.
 */

import React, { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import {
  AlertCircle,
  ArrowLeft,
  CheckCircle2,
  ExternalLink,
  KeyRound,
  Loader2,
  ShieldCheck,
  Sparkles,
} from "lucide-react";
import { hasMimoVoiceDescription, isMimoVoiceDesignModel, mimoVoicePresets } from "./lib/mimo-tts-voices.js";
import { formatModelIssue } from "./lib/model-issue.js";
import PageShell from "./shell/PageShell.jsx";
import Card from "./shell/Card.jsx";

import CustomSpeechSettings from "./CustomSpeechSettings.jsx";
import LlmNetworkSettings from "./LlmNetworkSettings.jsx";
import GenerationDiagnostics from "./GenerationDiagnostics.jsx";
import TextModelSelect from "./TextModelSelect.jsx";
import KlingVideoSettings from "./KlingVideoSettings.jsx";
import { KLING_KEY_URL } from "./lib/avatar-pipeline/kling-models.js";
import ImageModelSelect from "./ImageModelSelect.jsx";
import { VideoModelSelect } from "./VideoGenerationSettings.jsx";
import { useUsageHelp } from "./shell/DeviceContext.jsx";
import UsageHelp from "./UsageHelp.jsx";
import { useExitProtection } from "./lib/exit-protection.js";
import {
  VIDEO_PROVIDERS,
  loadProviderConfig,
  saveProviderConfig,
  resolveProviderApiKey,
} from "./lib/avatar-pipeline/provider-config.js";
import {
  LLM_PRESETS,
  MIMO_CONNECTIONS,
  mimoConnection,
  DEFAULT_LLM_PROVIDER,
  describeLlmPreset,
  selectLlmProvider,
  changeLlmEndpoint,
  emitApiConfigurationUpdated,
  providerCredentialsConfigured,
} from "./lib/api-configuration.js";

function loadVideoConfigs() {
  return Object.fromEntries(
    VIDEO_PROVIDERS.map((provider) => [provider.id, loadProviderConfig(provider.id)]),
  );
}
function persistedProviderConfig(config) {
  return {
    apiKey: config.apiKey || "",
    baseUrl: config.baseUrl || "",
    model: config.model || "",
    thinkingModel: config.thinkingModel || "",
    imageModel: (config.imageModel || "").trim(),
    fastGeneration: config.fastGeneration !== false,
    videoParameters: config.videoParameters || {},
    advanced: config.advanced || {},
  };
}

function resultToneClass(tone) {
  return tone ? ` is-${tone}` : "";
}

const SPEECH_NAMES = { volcengine: "豆包语音", mimo: "小米 MiMo", openai: "自定义端点" };
const MIMO_SPEECH_DEFAULTS = { model: "mimo-v2.5-asr", ttsModel: "mimo-v2.5-tts", ttsVoice: "mimo_default" };
const ARK_GUIDE = "https://docs.volcengine.com/docs/ark/quick-start-beginner?lang=zh";

function GuideLink({ href, children }) {
  return <a className="api-settings__credential-link" href={href} target="_blank" rel="noreferrer">
    {children}<ExternalLink size={11} aria-hidden="true" />
  </a>;
}

const cleanEndpoint = url => (url || "").trim().replace(/\/+$/, "");
function llmSnapshot(s) {
  return JSON.stringify([s.llmProvider, s.llmBaseUrl || "", s.llmModel || "", s.llmWebSearch !== false,
    s.llmApiKey || "", s.llmSearchMode || "auto", s.llmSearchApiKey || ""]);
}
function generationConfig(draft, saved) {
  const sameScope = draft.providerId !== "custom" || cleanEndpoint(draft.baseUrl) === cleanEndpoint(saved.baseUrl);
  return { ...draft, apiKey: resolveProviderApiKey(draft.providerId, draft.apiKey) || (sameScope ? saved.apiKey : "") };
}
function savedSpeechProfiles(status) {
  return {
    savedVolcengine: status?.savedVolcengine === true,
    savedMimo: status?.savedMimo === true,
  };
}

export default function ApiSettings({ onBack, configReadError = "", active = true }) {
  const usageHelp = useUsageHelp();
  const [savedVideoConfigs, setSavedVideoConfigs] = useState(loadVideoConfigs);
  const [videoConfigs, setVideoConfigs] = useState(() => Object.fromEntries(Object.entries(savedVideoConfigs)
    .map(([id, config]) => [id, { ...config, apiKey: "" }])));
  const videoEdits = useRef({});
  const [savedStages, setSavedStages] = useState({ asr: null, tts: null });
  const [customState, setCustomState] = useState({ dirty: false, busy: false });
  const [presetRevision, setPresetRevision] = useState(0);
  const [providerResults, setProviderResults] = useState({});
  const [providerPending, setProviderPending] = useState({});
  const [savedLlm, setSavedLlm] = useState(null);
  const [forgetScope, setForgetScope] = useState(null);
  const [speechEnvironment, setSpeechEnvironment] = useState(false);
  const [asrApiKey, setAsrApiKey] = useState("");
  const [selectedSpeech, setSelectedSpeech] = useState(null);
  const savedView = savedStages.asr?.provider === "openai" || savedStages.tts?.provider === "openai"
    ? "openai" : savedStages.asr?.provider || savedStages.tts?.provider || "volcengine";
  const asrProvider = selectedSpeech ?? savedView;
  const [savedMimoSpeech, setSavedMimoSpeech] = useState({});
  const [mimoEdits, setMimoEdits] = useState({});
  const storedMimo = { ...MIMO_SPEECH_DEFAULTS, ...savedMimoSpeech };
  const ttsModelDraft = mimoEdits.ttsModel ?? storedMimo.ttsModel;
  const voiceModel = ttsModelDraft.trim() || MIMO_SPEECH_DEFAULTS.ttsModel;
  const voiceDesign = isMimoVoiceDesignModel(voiceModel);
  const voicePresets = mimoVoicePresets(voiceModel);
  // A preset voice name and a voice-design instruction share one native field.
  // Keep their unsaved inputs separate when the user switches model families.
  const voiceField = voiceDesign ? "description" : "ttsVoice";
  const sameVoiceMode = isMimoVoiceDesignModel(storedMimo.ttsModel) === voiceDesign;
  const mimoSpeech = {
    model: mimoEdits.model ?? storedMimo.model,
    ttsModel: ttsModelDraft,
    ttsVoice: mimoEdits[voiceField] ?? (sameVoiceMode ? storedMimo.ttsVoice : voiceDesign ? "" : MIMO_SPEECH_DEFAULTS.ttsVoice),
  };
  const mimoFieldsKnown = Object.keys(MIMO_SPEECH_DEFAULTS).every(field =>
    Object.hasOwn(savedMimoSpeech, field) || Object.hasOwn(mimoEdits, field === "ttsVoice" ? voiceField : field));
  const [asrState, setAsrState] = useState({
    loading: true,
    pending: false,
    configured: false,
    ...savedSpeechProfiles(),
    tone: "muted",
    message: "正在读取语音识别配置…",
  });
  const asrKeySaved = asrProvider === "mimo" ? asrState.savedMimo : asrState.savedVolcengine;
  // 实时对话：服务预填默认模型并允许修改；豆包 Seed TTS 2.0 复用识别 Key。
  const [voiceChat, setVoiceChat] = useState({
    loading: true,
    llmPending: false,
    llmConfigured: false,
    llmProvider: DEFAULT_LLM_PROVIDER,
    llmBaseUrl: "",
    llmModel: LLM_PRESETS.find(preset => preset.id === DEFAULT_LLM_PROVIDER).model,
    llmWebSearch: true,
    llmSearchMode: "auto",
    llmSearchApiKey: "",
    llmSearchConfigured: false,
    llmApiKey: "",
    llmApiKeyMasked: "",
    llmApiKeyMasks: {},
    llmCustomBaseUrl: "",
    llmTone: "muted",
    llmMessage: "正在读取对话大模型配置…",
  });
  const mimo = mimoConnection(voiceChat.llmProvider);
  const llmScope = voiceChat.llmProvider === "custom" ? "custom:" + cleanEndpoint(voiceChat.llmBaseUrl) : voiceChat.llmProvider;
  useEffect(() => setForgetScope(null), [llmScope]);
  const chatName = { doubao: "火山方舟", deepseek: "DeepSeek", mimo: "小米 MiMo", "mimo-token-plan-cn": "MiMo Token Plan", custom: "自定义" }[voiceChat.llmProvider];
  const speechBusy = asrState.pending || asrState.testing || asrState.loading
    || (asrProvider === "mimo" && (voiceChat.loading || !mimoFieldsKnown));
  const videoDirty = useMemo(() => VIDEO_PROVIDERS.some(({ id }) =>
    JSON.stringify(persistedProviderConfig(generationConfig(videoConfigs[id], savedVideoConfigs[id]))) !==
    JSON.stringify(persistedProviderConfig(savedVideoConfigs[id]))), [videoConfigs, savedVideoConfigs]);
  const mimoDirty = Object.keys(MIMO_SPEECH_DEFAULTS).some(field => mimoSpeech[field] !== storedMimo[field]);
  const speechDirty = mimoDirty || customState.dirty || !!asrApiKey ||
    (asrProvider !== "openai" && asrProvider !== savedView);
  const dirty = videoDirty || speechDirty || (savedLlm !== null && savedLlm !== llmSnapshot(voiceChat));
  const speechEdits = useRef(0);
  const editSpeech = () => {
    speechEdits.current++;
    setSelectedSpeech(asrProvider);
    setAsrState(current => ({ ...current, message: "", results: null, tone: "muted" }));
  };
  const editMimoSpeech = patch => {
    editSpeech();
    setMimoEdits(current => ({ ...current, ...patch }));
  };
  const saving = customState.busy || voiceChat.llmPending || voiceChat.llmTesting || asrState.pending || asrState.testing || Object.values(providerPending).some(Boolean);
  useExitProtection(saving ? "模型配置正在保存或测试" : dirty ? "模型配置有未保存的修改" : "");

  const applyVoiceChatStatus = (status, patch = {}) => {
    setSavedLlm(llmSnapshot({ ...status, llmProvider: status?.llmProvider || DEFAULT_LLM_PROVIDER }));
    setVoiceChat((current) => ({
      ...current,
      ...patch,
      loading: false,
      llmConfigured: status?.llmConfigured === true,
      llmWebSearch: status?.llmWebSearch !== false,
      llmSearchMode: status?.llmSearchMode || "auto",
      llmSearchConfigured: status?.llmSearchConfigured === true,
      llmProvider: status?.llmProvider || current.llmProvider,
      llmBaseUrl: status?.llmBaseUrl || "",
      llmModel: status?.llmModel || "",
      llmApiKeyMasked: status?.llmApiKeyMasked || "",
      llmApiKeyMasks: status?.llmApiKeyMasks || {},
      llmModels: status?.llmModels || {},
      savedConfigured: status?.llmConfigured === true,
      llmCustomBaseUrl: status?.llmCustomBaseUrl || "",
    }));
  };

  const editLlm = patch => setVoiceChat(current => ({
    ...current, ...patch, llmTone: "muted", llmMessage: "配置已更改，保存后生效。",
  }));

  const saveLlm = async () => {
    setVoiceChat((current) => ({ ...current, llmPending: true, llmTone: "muted", llmMessage: "正在保存对话大模型配置…" }));
    try {
      const custom = voiceChat.llmProvider === "custom";
      const status = await invoke("save_voice_chat_settings", {
        input: {
          llmProvider: voiceChat.llmProvider,
          llmWebSearch: voiceChat.llmWebSearch,
          llmSearchMode: voiceChat.llmSearchMode,
          llmSearchApiKey: voiceChat.llmSearchApiKey.trim() || null,
          llmBaseUrl: custom ? voiceChat.llmBaseUrl : null,
          llmModel: voiceChat.llmModel.trim(),
          llmApiKey: voiceChat.llmApiKey.trim() || null,
        },
      });
      emitApiConfigurationUpdated({ providerId: "voice-chat-llm", configured: status?.llmConfigured === true });
      applyVoiceChatStatus(status, {
        llmPending: false,
        llmApiKey: "",
        llmSearchApiKey: "",
        llmTone: status?.llmConfigured ? "success" : "warning",
        llmMessage: status?.llmConfigured ? "配置已保存，开始聊天时将使用所选模型。" : "已保存，但还没有 API Key",
      });

    } catch (error) {
      setVoiceChat((current) => ({ ...current, llmPending: false, llmTone: "error", llmMessage: "保存失败。" + formatModelIssue("storage", chatName, error) }));
    }
  };

  const testLlm = async () => {
    const tested = llmSnapshot(voiceChat);
    setVoiceChat(current => ({ ...current, llmTesting: true, llmTone: "muted", llmMessage: "正在测试所选模型…" }));
    const finish = (llmTone, llmMessage) => setVoiceChat(current => llmSnapshot(current) === tested
      ? { ...current, llmTesting: false, llmTone, llmMessage }
      : { ...current, llmTesting: false, llmTone: "muted", llmMessage: "配置已更改，请测试当前选择。" });
    try {
      await invoke("test_voice_chat_model", { input: { llmProvider: voiceChat.llmProvider,
        llmBaseUrl: voiceChat.llmProvider === "custom" ? voiceChat.llmBaseUrl : null,
        llmModel: voiceChat.llmModel.trim(), llmApiKey: voiceChat.llmApiKey.trim() || null } });
      finish("success", "所选模型已返回测试文本。配置修改仍需保存。");
    } catch (error) {
      finish("error", formatModelIssue("chat", chatName, error));
    }
  };

  const clearSearchKey = async () => {
    setVoiceChat(current => ({ ...current, llmPending: true }));
    try {
      await invoke("clear_voice_chat_secret", { which: "search" });
      setVoiceChat(current => ({ ...current, llmPending: false, llmSearchConfigured: false,
        llmSearchApiKey: "", llmTone: "success", llmMessage: "已删除独立搜索 Key。" }));
    } catch (error) {
      setVoiceChat(current => ({ ...current, llmPending: false, llmTone: "error",
        llmMessage: "删除搜索 Key 失败。" + formatModelIssue("storage", "DeepSeek", error) }));
    }
  };

  const forgetLlmKey = async () => {
    if (forgetScope !== llmScope) { setForgetScope(llmScope); return; }
    setForgetScope(null);
    setVoiceChat(current => ({ ...current, llmPending: true }));
    try {
      const status = await invoke("clear_voice_chat_secret", { which: "llm", scope: llmScope });
      setVoiceChat(current => ({ ...current, llmPending: false, llmApiKeyMasks: status.llmApiKeyMasks,
        savedConfigured: status.llmConfigured, llmConfigured: false, llmApiKeyMasked: "", llmApiKey: "",
        llmTone: "success", llmMessage: "已忘记此服务在本机保存的对话模型 Key。" }));
      emitApiConfigurationUpdated({ providerId: "voice-chat-llm", configured: status.llmConfigured });
    } catch (error) {
      setVoiceChat(current => ({ ...current, llmPending: false, llmTone: "error",
        llmMessage: "忘记 Key 失败。" + formatModelIssue("storage", chatName, error) }));
    }
  };

  useEffect(() => {
    let cancelled = false;
    invoke("load_voice_chat_settings")
      .then((status) => {
        if (cancelled) return;
        setSavedMimoSpeech(current => ({ ...current,
          ttsModel: status?.mimoTtsModel || MIMO_SPEECH_DEFAULTS.ttsModel,
          ttsVoice: status?.mimoTtsVoice || MIMO_SPEECH_DEFAULTS.ttsVoice,
        }));
        setSavedStages(current => ({ ...current, tts: current.tts ?? {
          provider: status?.ttsProvider || "volcengine", configured: status?.ttsConfigured === true,
        } }));
        applyVoiceChatStatus(status, {
          llmTone: status?.llmConfigured ? "success" : "muted",
          llmMessage: status?.llmConfigured ? "对话大模型已配置" : "选一家大模型，填入 API Key 即可",
        });
      })
      .catch((error) => {
        if (!cancelled) setVoiceChat((current) => ({ ...current, loading: false,
          llmTone: "error", llmMessage: "读取对话配置失败。" + formatModelIssue("storage", "对话大模型", error) }));
      });
    // Independent services: a failed LLM read must not suppress the ASR read.
    invoke("load_device_asr_settings")
      .then((status) => {
        if (cancelled) return;
        setSavedMimoSpeech(current => ({ ...current, model: status?.mimoAsrModel || MIMO_SPEECH_DEFAULTS.model }));
        setSpeechEnvironment(status?.credentialSource === "environment");
        const storedProvider = status?.savedProvider || status?.provider;
        setSavedStages(current => ({ ...current, asr: current.asr ?? {
          provider: Object.hasOwn(SPEECH_NAMES, storedProvider) ? storedProvider : "volcengine",
          configured: status?.configured === true,
        } }));
        setAsrState({
          loading: false,
          pending: false,
          configured: status?.configured === true,
          ...savedSpeechProfiles(status),
          tone: status?.configured ? "success" : "muted",
          message: "",
        });
      })
      .catch((error) => {
        if (cancelled) return;
        setAsrState({
          loading: false,
          pending: false,
          configured: false,
          ...savedSpeechProfiles(),
          tone: "error",
          message: "读取语音配置失败。" + formatModelIssue("storage", "语音服务", error),
        });
      });
    return () => {
      cancelled = true;
    };
  }, []);

  const configuredProviderCount = useMemo(
    () => VIDEO_PROVIDERS.filter((provider) =>
      providerCredentialsConfigured(provider.id, savedVideoConfigs[provider.id]),
    ).length,
    [savedVideoConfigs],
  );
  const configuredCount = configuredProviderCount + (savedStages.asr?.configured || savedStages.tts?.configured ? 1 : 0)
    + (voiceChat.savedConfigured ? 1 : 0);

  const updateProvider = (providerId, patch) => {
    videoEdits.current[providerId] = (videoEdits.current[providerId] || 0) + 1;
    setVideoConfigs((current) => ({
      ...current,
      [providerId]: {
        ...current[providerId],
        ...patch,
      },
    }));
    setProviderResults((current) => ({ ...current, [providerId]: null }));
  };

  const saveProvider = async (providerId, checkOnly = false) => {
    if (providerPending[providerId]) return;
    const draft = videoConfigs[providerId];
    const config = generationConfig(draft, savedVideoConfigs[providerId]);
    const submittedEdit = videoEdits.current[providerId];
    const hasKey = providerCredentialsConfigured(providerId, config);
    const checks = [{ label: "API Key", ok: hasKey,
      message: hasKey ? "已填写" : "请填写 API Key。" }];
    if (providerId === "custom") {
      let validUrl = false;
      try {
        const url = new URL(config.baseUrl);
        validUrl = ["http:", "https:"].includes(url.protocol) && !(url.username || url.password || url.search || url.hash);
      } catch { /* Report the invalid address alongside the other fields. */ }
      checks.push({ label: "接口地址", ok: validUrl, message: validUrl ? "格式有效" : "请填写完整且有效的 Base URL。" });
    }
    const hasModel = Boolean(config.model?.trim());
    checks.push({ label: "视频生成模型", ok: hasModel || providerId !== "custom",
      message: hasModel ? "已指定" : providerId === "custom" ? "请填写视频生成模型。" : "使用服务预设" });
    if (providerId === "volcengine") {
      checks.push({ label: "图像理解模型", ok: true, message: config.thinkingModel ? "已指定" : "使用默认模型" },
        { label: "图片处理模型", ok: true, message: config.imageModel ? "已指定" : "使用默认模型" });
    }
    const invalid = checks.find(check => !check.ok);
    if (checkOnly || invalid) {
      setProviderResults(current => ({ ...current, [providerId]: {
        tone: invalid ? "warning" : "success",
        message: checkOnly ? (invalid ? "请补全以下配置。" : "本地配置检查通过。模型调用权限需由服务端确认。") : invalid.message,
        checks: checkOnly ? checks : null,
      } }));
      return;
    }
    setProviderPending(current => ({ ...current, [providerId]: true }));
    try {
      await saveProviderConfig(providerId, persistedProviderConfig(config));
    } catch (error) {
      setProviderResults((current) => ({ ...current,
        [providerId]: { tone: "error", message: "保存失败。" + formatModelIssue("storage", { volcengine: "火山方舟", kling: "可灵", custom: "自定义" }[providerId], error) },
      }));
      return;
    } finally {
      setProviderPending(current => ({ ...current, [providerId]: false }));
    }
    const savedConfig = { ...config, needsKlingApiKey: false };
    setVideoConfigs(current => {
      const latest = current[providerId];
      if (latest.apiKey !== draft.apiKey || cleanEndpoint(latest.baseUrl) !== cleanEndpoint(draft.baseUrl)) return current;
      return { ...current, [providerId]: { ...latest, needsKlingApiKey: false, apiKey: "" } };
    });
    setSavedVideoConfigs(current => ({ ...current, [providerId]: savedConfig }));
    setProviderResults((current) => ({
      ...current,
      [providerId]: { tone: "success", message: videoEdits.current[providerId] === submittedEdit ? "已保存到当前客户端。" : "已保存上次提交的配置；后续修改仍待保存。" },
    }));
    emitApiConfigurationUpdated({ providerId });
  };

  const onCustomSaved = useCallback((stage, status) => {
    emitApiConfigurationUpdated({ providerId: stage === "asr" ? "volcengine-asr" : "voice-chat-tts",
      configured: status[stage].configured === true });
    setSavedStages(current => ({ ...current, [stage]: {
      provider: "openai", configured: status[stage].configured === true,
    } }));
  }, []);

  const selectSpeechProvider = (provider) => {
    if (provider === asrProvider) return;
    editSpeech();
    setSelectedSpeech(provider);
    setAsrApiKey("");
  };

  const saveAndTestAsr = async (probe = true) => {
    if (asrProvider === "openai") return;
    const providerName = SPEECH_NAMES[asrProvider];
    const submittedEdit = speechEdits.current;
    if (!asrKeySaved && !asrApiKey.trim()) {
      setAsrState((current) => ({
        ...current,
        tone: "warning",
        message: `请填写语音 API Key（${providerName}）。`,
      }));
      return;
    }
    if (probe && asrProvider === "mimo" && voiceDesign && !hasMimoVoiceDescription(mimoSpeech.ttsVoice)) {
      setAsrState(current => ({ ...current, tone: "warning", message: "请为音色设计模型填写音色描述。" }));
      return;
    }
    const submittedMimo = {
      model: mimoSpeech.model.trim() || MIMO_SPEECH_DEFAULTS.model,
      ttsModel: mimoSpeech.ttsModel.trim() || MIMO_SPEECH_DEFAULTS.ttsModel,
      ttsVoice: mimoSpeech.ttsVoice.trim() || MIMO_SPEECH_DEFAULTS.ttsVoice,
    };
    setAsrState((current) => ({
      ...current,
      pending: true,
      results: null,
      tone: "muted",
      message: probe ? "正在保存并测试语音转写与合成…" : "正在保存语音配置…",
    }));
    try {
      const saved = await invoke("save_speech_settings", {
        input: {
          apiKey: asrApiKey.trim() || null,
          provider: asrProvider,
          ...(asrProvider === "mimo" ? submittedMimo : { baseUrl: null, model: null, ttsModel: null, ttsVoice: null }),
        },
      });
      emitApiConfigurationUpdated({
        providerId: "volcengine-asr",
        configured: saved?.configured === true,
      });
      if (asrProvider === "mimo") {
        setSavedMimoSpeech(submittedMimo);
        setMimoEdits({});
      }
      setAsrApiKey("");
      const stage = { provider: asrProvider, configured: saved?.configured === true };
      setSavedStages({ asr: stage, tts: stage });
      setPresetRevision(current => current + 1);
      if (!probe) {
        setAsrState(current => ({ ...current, ...savedSpeechProfiles(saved), configured: saved?.configured === true,
          pending: false, tone: "success", message: "语音配置已保存。" }));
        return;
      }
      setAsrState(current => ({ ...current, ...savedSpeechProfiles(saved), configured: saved?.configured === true,
        pending: false, testing: true, tone: "success", message: "配置已保存。",
        results: [{ stage: "asr", label: "语音识别" }, { stage: "tts", label: "语音合成" }],
      }));
      const reportProbe = async (stage, request) => {
        let message;
        let ok = false;
        try {
          const result = await request;
          const latency = stage === "asr" ? result?.latencyMs : result;
          ok = stage === "asr" ? result?.ok === true : Number.isFinite(latency) && latency >= 0;
          if (ok) message = Number.isFinite(latency) ? `通过（${latency} ms）` : "通过";
          else message = formatModelIssue(stage, providerName, stage === "asr" ? result?.message || "未返回转写结果" : "未返回音频");
        } catch (error) {
          message = formatModelIssue(stage, providerName, error);
        }
        setAsrState(current => speechEdits.current === submittedEdit && current.results ? { ...current,
          results: current.results.map(row => row.stage === stage ? { ...row, ok, message } : row),
        } : current);
      };
      await Promise.all([
        reportProbe("asr", invoke("test_device_asr_settings")),
        reportProbe("tts", invoke("test_device_tts_settings")),
      ]);
      setAsrState(current => speechEdits.current === submittedEdit ? { ...current, testing: false }
        : { ...current, testing: false, tone: "muted", results: null, message: "语音配置已更改，请保存并测试当前选择。" });
    } catch (error) {
      setAsrState((current) => ({
        ...current,
        loading: false,
        pending: false,
        testing: false,
        tone: "error",
        message: "语音配置保存失败。" + formatModelIssue("storage", providerName, error),
      }));
    }
  };

  return (
    <div className="api-settings-page">
    <PageShell
      title="模型配置"
      subtitle="配置语音、对话和形象生成服务"
      actions={onBack && (
        <button type="button" className="btn-ghost btn-sm" onClick={onBack}>
          <ArrowLeft size={14} />返回
        </button>
      )}
    >
      {dirty && <p role="status" className="api-settings__draft">有未保存的修改。切换页面会保留草稿；关闭客户端前请保存。</p>}
      <section className="api-settings__overview" aria-label="模型配置概览">
        <div className="api-settings__overview-icon" aria-hidden="true">
          <ShieldCheck size={22} />
        </div>
        <div className="api-settings__overview-copy">
          <strong>{configReadError ? "形象生成配置暂时无法读取" : `${configuredCount} / ${VIDEO_PROVIDERS.length + 2} 项凭据已配置`}</strong>
          {configReadError && <span>{formatModelIssue("storage", "形象生成", configReadError)}</span>}
          <span>按需配置即可：给 Agent 语音输入只需语音识别；和宠物聊天还需语音合成与对话大模型；生成形象需配置形象生成服务。</span>
        </div>
        <div className="api-settings__storage-note">
          <KeyRound size={14} aria-hidden="true" />
          API Key 请勿分享给他人
        </div>
      </section>

      <nav className="settings-section-links" aria-label="跳至模型配置项目">
        {[["speech", "语音识别与合成"], ["dialogue", "对话大模型"], ["avatar", "形象生成"]].map(([key, label]) =>
          <button key={key} type="button" onClick={() => {
            const section = document.getElementById(`model-settings-${key}`);
            section?.focus({ preventScroll: true });
            section?.scrollIntoView({ block: "start" });
          }}>{label}</button>
        )}
      </nav>
      <Card id="model-settings-speech"
        title="语音识别与合成"
        subtitle={asrProvider === "openai" ? "识别和合成各自保存。只配置其中一项即可使用该项服务。"
          : "识别与合成共用一个语音 API Key。仅保存会更新本机配置，保存并测试还会分别调用两项服务，可能产生费用。"}
        actions={asrProvider !== "openai" && (
          <span className={`api-settings__status${asrKeySaved ? " is-success" : ""}`}>
            {asrState.loading ? <Loader2 size={13} className="spin" /> : asrKeySaved ? <CheckCircle2 size={13} /> : null}
            {asrState.loading ? "读取中" : asrKeySaved ? "已配置" : "未配置"}
          </span>
        )}
      >
        <div className="usage-help">
          <p><strong>语音输入：</strong>{usageHelp.voice} {usageHelp.confirm}</p>
          <p><strong>实时对话：</strong>{usageHelp.chat} 还需配置下方「对话大模型」。</p>
          {usageHelp.pending && <p className="usage-help__notice">按键配置有修改，请先同步到设备。</p>}
          <details><summary>使用前需要什么</summary><p>请保持设备连接电脑，并让 Pet Manager 持续运行。语音输入使用识别服务，宠物语音回复还会调用合成服务。</p></details>
        </div>
        {speechEnvironment && <p className="muted small">当前运行时优先使用环境变量中的语音配置。此处保存和测试的是本机配置。</p>}
        <div className="api-settings__presets" role="radiogroup" aria-label="语音服务提供方">
          {Object.entries(SPEECH_NAMES).map(([id, label]) => <button key={id} type="button" role="radio"
            aria-checked={asrProvider === id} disabled={asrState.pending || asrState.loading || customState.busy}
            className={"btn-ghost btn-sm api-settings__preset" + (asrProvider === id ? " is-active" : "")}
            onClick={() => selectSpeechProvider(id)}>{label}</button>)}
        </div>
        {asrProvider !== "openai" && <div className="api-settings__guide">
          {asrProvider === "volcengine" && <>在豆包语音控制台为同一项目开通流式语音识别 2.0（小时版）和语音合成 2.0，再填写该项目的语音 Key。
            <GuideLink href="https://docs.volcengine.com/docs/doubao-voice/console-guidance?lang=zh">豆包语音开通与密钥管理</GuideLink></>}
          {asrProvider === "mimo" && <>填写小米 MiMo 开放平台的按量 API Key，无需单独开通语音。官方接口 <code>{MIMO_CONNECTIONS[0].baseUrl}</code>。首次使用只需填写 Key，需要更换模型或音色时可展开下方设置。
            <GuideLink href="https://mimo.mi.com/docs/zh-CN/quick-start/usage-guide/multimodal-understanding/Speech-Recognition">MiMo 语音识别</GuideLink>
            <GuideLink href="https://mimo.mi.com/docs/zh-CN/quick-start/usage-guide/audio/speech-synthesis-v2.5">MiMo 语音合成与音色</GuideLink></>}
        </div>}
        {asrProvider === "mimo" && <details>
          <summary>模型与音色</summary>
          <div className="api-settings__form api-settings__form--speech">
            {[
              ["model", "语音识别模型（ASR）"],
              ["ttsModel", "语音合成模型（TTS）"],
              ["ttsVoice", voiceDesign ? "音色设计描述" : "合成音色"],
            ].map(([field, label]) => <label key={field} className="ui-field" htmlFor={`api-settings-mimo-${field}`}>
              <span className="ui-field__label">{label}</span>
              <input id={`api-settings-mimo-${field}`} className="ui-control" value={mimoSpeech[field]}
                list={field === "ttsVoice" && voicePresets.length ? "api-settings-mimo-voices" : undefined}
                disabled={asrState.pending} spellCheck={false}
                placeholder={field === "ttsVoice" && voiceDesign ? "例如：温暖、沉静的青年女声" : MIMO_SPEECH_DEFAULTS[field]}
                onChange={event => editMimoSpeech({ [field === "ttsVoice" ? voiceField : field]: event.target.value })} />
            </label>)}
          </div>
          {voicePresets.length > 0 && <datalist id="api-settings-mimo-voices">
            {voicePresets.map(voice => <option key={voice} value={voice} />)}
          </datalist>}
          <p className="muted small">{voiceDesign ? "可用中文描述声音特点。留空会保存为 mimo_default。测试此模型前，请填写实际的音色描述。" : voicePresets.length
            ? "可选择预设音色或直接输入音色名。留空使用默认值。" : "请填写当前模型支持的音色名。留空使用默认值。"}</p>
          <button type="button" className="btn-ghost btn-sm" disabled={asrState.pending}
            onClick={() => { editSpeech(); setMimoEdits(MIMO_SPEECH_DEFAULTS); }}>恢复默认</button>
        </details>}
        {asrProvider === "mimo" && !asrState.loading && !voiceChat.loading && !mimoFieldsKnown &&
          <p className="muted small">部分 MiMo 配置读取失败。可重新打开页面，或展开「模型与音色」填写缺失项或恢复默认后保存。</p>}
        {asrProvider === "openai" && savedStages.asr && savedStages.tts &&
          savedStages.asr.provider !== savedStages.tts.provider && <p className="muted small">
            当前识别服务：{SPEECH_NAMES[savedStages.asr.provider]}。当前合成服务：{SPEECH_NAMES[savedStages.tts.provider]}。
          </p>}
        <CustomSpeechSettings active={active && asrProvider === "openai"}
          onStateChange={setCustomState} onSaved={onCustomSaved} presetRevision={presetRevision} />
        {asrProvider !== "openai" && <>
          <div className="api-settings__form api-settings__speech-actions">
            <label className="ui-field" htmlFor="api-settings-asr-key">
              <span className="ui-field__label">语音 API Key（识别与合成共用）</span>
              <input
                id="api-settings-asr-key"
                className="ui-control api-settings__secret-input"
                {...secretInputProps}
                value={asrApiKey}
                onChange={event => { editSpeech(); setAsrApiKey(event.target.value); }}
                placeholder={asrKeySaved ? "已安全保存；留空可直接复测"
                  : `输入语音 API Key（${SPEECH_NAMES[asrProvider]}）`}
                disabled={asrState.pending || asrState.loading}
              />
            </label>
            <button type="button" className="btn-secondary btn-sm" disabled={speechBusy}
              onClick={() => saveAndTestAsr(false)}>仅保存</button>
            <button
              type="button"
              className="btn-primary btn-sm api-settings__save"
              onClick={() => saveAndTestAsr(true)}
              disabled={speechBusy}
            >
              {asrState.pending ? <Loader2 size={14} className="spin" /> : <CheckCircle2 size={14} />}
              保存并测试
            </button>
          </div>
          <div className={`api-settings__result${resultToneClass(asrState.tone)}`} role="status">
            {asrState.tone === "error" || asrState.tone === "warning"
              ? <AlertCircle size={13} />
              : <CheckCircle2 size={13} />}
            {asrState.message || (asrKeySaved ? "已保存此服务的 API Key" : "此服务尚未配置 API Key")}
          </div>
          {asrState.results && <ul className="api-settings__probe-results" aria-label="语音测试分项结果" aria-live="polite">
            {asrState.results.map(({ label, ok, message }) => <li key={label} className={ok === undefined ? "" : ok ? "is-success" : "is-error"}>
              <strong>{label}</strong> {message || "测试中…"}
            </li>)}
          </ul>}
        </>}
      </Card>

      <Card id="model-settings-dialogue" title="对话大模型" subtitle="决定宠物在「实时对话」中如何理解和回答。选择一家服务并填写 API Key 即可。此配置用于和宠物聊天，不影响 ChatGPT、Claude 等 Agent 的语音输入。">
        <UsageHelp />
        <div className="api-settings__presets" role="radiogroup" aria-label="对话大模型">
          {[...LLM_PRESETS, { id: "custom", label: "自定义" }].map((preset) => {
            const selected = preset.id === "mimo" ? Boolean(mimo) : voiceChat.llmProvider === preset.id;
            return <button
              key={preset.id}
              type="button"
              role="radio"
              aria-checked={selected}
              className={"btn-ghost btn-sm api-settings__preset" + (selected ? " is-active" : "")}
              onClick={() => setVoiceChat(current => selected ? current : selectLlmProvider(current, preset.id))}
              disabled={voiceChat.llmPending || voiceChat.loading}
            >
              {preset.label}
            </button>;
          })}
        </div>
        <div className="api-settings__guide">
          {voiceChat.llmProvider === "doubao" && <>在火山方舟「开通管理」开通所选对话模型，到「API Key 管理」创建该项目的方舟 Key。方舟 Key 与豆包语音 Key 分别创建。
            <GuideLink href={ARK_GUIDE}>方舟模型开通与 API Key</GuideLink></>}
          {mimo && <>选择 Key 对应的按量 API 或中国内地 Token Plan，两种接入方式各用自己的 Key 和额度。
            <GuideLink href="https://mimo.mi.com/docs/zh-CN/quick-start/faq/token-plan">MiMo 接入说明</GuideLink></>}
          {voiceChat.llmProvider === "deepseek" && <>在 DeepSeek 开放平台创建 API Key。
            <GuideLink href="https://api-docs.deepseek.com/zh-cn/">DeepSeek 接入说明</GuideLink></>}
          {voiceChat.llmProvider === "custom" && <>填写支持聊天补全的接口地址、模型 ID 和该接口的 Key。</>}
          <div>测试当前模型会发送一条简短消息，可能产生费用。草稿在点击保存后生效。</div>
        </div>
        <div className="api-settings__form api-settings__form--llm">
          {mimo && (
            <label className="ui-field" htmlFor="api-settings-mimo-connection">
              <span className="ui-field__label">MiMo 接入方式</span>
              <select id="api-settings-mimo-connection" className="ui-control" value={mimo.id}
                onChange={event => setVoiceChat(current => selectLlmProvider(current, event.target.value))}
                disabled={voiceChat.llmPending || voiceChat.loading}>
                {MIMO_CONNECTIONS.map(connection => <option key={connection.id} value={connection.id}>{connection.label}</option>)}
              </select>
              <span className="muted small">接口地址 {mimo.baseUrl}</span>
            </label>
          )}
          {voiceChat.llmProvider === "custom" ? (
            <>
              <label className="ui-field" htmlFor="api-settings-llm-url">
                <span className="ui-field__label">接口地址（OpenAI 兼容）</span>
                <input
                  id="api-settings-llm-url"
                  className="ui-control"
                  value={voiceChat.llmBaseUrl}
                  onChange={(event) => setVoiceChat((current) => changeLlmEndpoint(current, event.target.value))}
                  placeholder="https://your-endpoint/v1"
                  disabled={voiceChat.llmPending || voiceChat.loading}
                />
              </label>
            </>
          ) : !mimo && (
            <div className="api-settings__preset-summary muted small">
              {describeLlmPreset(voiceChat.llmProvider, voiceChat.llmModel)}
            </div>
          )}
          <label className="ui-field" htmlFor="api-settings-llm-model">
            <span className="ui-field__label">模型名称 / 接入点 ID</span>
            <TextModelSelect autoLoad={false} value={voiceChat.llmModel}
              onChange={llmModel => editLlm({ llmModel })}
              provider={voiceChat.llmProvider} baseUrl={voiceChat.llmBaseUrl}
              apiKey={voiceChat.llmApiKey} configured={!voiceChat.loading && voiceChat.llmConfigured}
              disabled={voiceChat.llmPending || voiceChat.loading} />
            <span id="api-settings-llm-model-help" className="muted small">可填写账号已开通的模型名称或接入点 ID；留空使用默认模型。</span>
          </label>
          <label className="ui-field" htmlFor="api-settings-llm-key">
            <span className="ui-field__label">API Key</span>
            <input
              id="api-settings-llm-key"
              className="ui-control api-settings__secret-input"
              {...secretInputProps}
              value={voiceChat.llmApiKey}
              onChange={event => editLlm({ llmApiKey: event.target.value })}
              placeholder={voiceChat.llmConfigured ? "已保存；留空保持不变"
                : mimo ? (mimo.id === "mimo" ? "粘贴 MiMo 按量 API Key" : "粘贴该集群的 Token Plan API Key")
                  : "粘贴这家模型的 API Key"}
              disabled={voiceChat.llmPending || voiceChat.loading}
            />
          </label>
          <button
            type="button"
            className="btn-primary btn-sm api-settings__save"
            onClick={saveLlm}
            disabled={voiceChat.llmPending || voiceChat.llmTesting || voiceChat.loading}
          >
            {voiceChat.llmPending ? <Loader2 size={14} className="spin" /> : <CheckCircle2 size={14} />}
            保存
          </button>
          <button type="button" className="btn-secondary btn-sm"
            title="向当前模型发送一条简短测试消息，可能产生少量费用"
            disabled={voiceChat.llmPending || voiceChat.llmTesting || voiceChat.loading || !(voiceChat.llmApiKey.trim() || voiceChat.llmConfigured)}
            onClick={testLlm}>测试当前模型</button>
          {voiceChat.llmConfigured && <button type="button" className="btn-secondary btn-sm"
            disabled={voiceChat.llmPending || voiceChat.llmTesting || voiceChat.loading}
            onClick={forgetLlmKey}>{forgetScope === llmScope
              ? `确认忘记 ${voiceChat.llmProvider === "custom" ? cleanEndpoint(voiceChat.llmBaseUrl) : chatName} 的 Key`
              : "忘记此服务的 Key"}</button>}
          {forgetScope === llmScope && <button type="button" className="btn-ghost btn-sm"
            onClick={() => setForgetScope(null)}>取消</button>}
        </div>
        <div className={`api-settings__result${resultToneClass(voiceChat.llmTone)}`} role="status">
          {voiceChat.llmTone === "error" || voiceChat.llmTone === "warning"
            ? <AlertCircle size={13} />
            : <CheckCircle2 size={13} />}
          {voiceChat.llmMessage}
        </div>
        <label className="ui-field api-settings__search-option">
          <span><input type="checkbox" checked={voiceChat.llmWebSearch}
            disabled={voiceChat.llmPending || voiceChat.loading}
            onChange={event => setVoiceChat(current => ({ ...current, llmWebSearch: event.target.checked }))} /> 按需联网检索</span>
          <span className="muted small">明确说「联网搜」可直接检索，闲聊和家居控制通常无需上网。检索可能单独计费。修改后点击保存，下次开始聊天生效。</span>
        </label>
        {voiceChat.llmWebSearch && <>
          <label className="ui-field">
            <span>检索来源</span>
            <select value={voiceChat.llmSearchMode}
              disabled={voiceChat.llmPending || voiceChat.loading}
              onChange={event => setVoiceChat(current => ({ ...current, llmSearchMode: event.target.value }))}>
              <option value="auto">自动 · 当前服务商</option>
              <option value="deepSeek">DeepSeek 搜索</option>
              <option value="autoWithDeepSeek">自动 · DeepSeek 备用</option>
            </select>
          </label>
          {(voiceChat.llmSearchMode !== "auto" || voiceChat.llmSearchConfigured) && <div>
            <label className="ui-field">
              <span>DeepSeek 搜索 API Key</span>
              <input {...secretInputProps}
                value={voiceChat.llmSearchApiKey}
                placeholder={voiceChat.llmSearchConfigured ? "已保存，留空继续使用" : "输入 DeepSeek 官方 API Key"}
                disabled={voiceChat.llmPending || voiceChat.loading}
                onChange={event => setVoiceChat(current => ({ ...current, llmSearchApiKey: event.target.value }))} />
              <span className="muted small">使用其他对话服务商时，请单独填写。对话已接入官方 DeepSeek 时可复用其 Key。启用备用后，当前服务商无法完成检索时会把公开关键词发送给 DeepSeek。</span>
            </label>
            {voiceChat.llmSearchConfigured && <button type="button" className="btn-ghost btn-sm"
              disabled={voiceChat.llmPending || voiceChat.loading}
              onClick={clearSearchKey}>删除独立搜索 Key</button>}
          </div>}
        </>}
      </Card>

      <Card title="形象生成" id="model-settings-avatar" subtitle="方舟账户需开通所选视频、图像理解及图片处理模型。检查本地配置只核对填写内容，生成任务才会调用所选模型。">
        <GenerationDiagnostics />
        <div className="api-settings__provider-grid">
          {VIDEO_PROVIDERS.map((provider) => {
            const draft = videoConfigs[provider.id];
            const saved = savedVideoConfigs[provider.id];
            const config = generationConfig(draft, saved);
            const configured = providerCredentialsConfigured(provider.id, savedVideoConfigs[provider.id]);
            const result = providerResults[provider.id];
            const isKling = provider.id === "kling";
            const isCustom = provider.id === "custom";
            return (
              <section className="api-settings__provider" key={provider.id}>
                <header className="api-settings__provider-head">
                  <span className="api-settings__service-mark" aria-hidden="true">
                    <Sparkles size={18} />
                  </span>
                  <span className="api-settings__provider-title">
                    <strong>{provider.label}</strong>
                    <small>
                      {provider.sub}
                      {provider.id === "volcengine" && (
                        <GuideLink href={ARK_GUIDE}>方舟模型开通与 API Key</GuideLink>
                      )}
                    </small>
                  </span>
                  <span className={`api-settings__status${configured ? " is-success" : ""}`}>
                    {configReadError ? "读取失败" : configured ? "已配置" : "未配置"}
                  </span>
                </header>

                <fieldset className="api-settings__provider-fields" disabled={Boolean(configReadError)} style={{border:0,padding:0,margin:0,minWidth:0}}>
                  <label className="ui-field" htmlFor={`api-settings-${provider.id}-key`}>
                    <span className="ui-field__label">API Key</span>
                    <input
                      id={`api-settings-${provider.id}-key`}
                      className="ui-control api-settings__secret-input"
                      {...secretInputProps}
                      value={draft.apiKey}
                      onChange={(event) => updateProvider(provider.id, { apiKey: event.target.value })}
                      placeholder={generationConfig({ ...draft, apiKey: "" }, saved).apiKey ? "已保存；留空保持原 Key" : "输入" + provider.label + " API Key"}
                    />
                  </label>
                  {isKling && <>
                    <a className="api-settings__credential-link" href={KLING_KEY_URL} target="_blank" rel="noreferrer">
                      在可灵国内开发者控制台获取 API Key <ExternalLink size={11} aria-hidden="true" />
                    </a>
                    {config.needsKlingApiKey && <span className="muted small">旧版凭据需要换成新版 API Key，保存后生效。</span>}
                    <KlingVideoSettings model={config.model} value={config.videoParameters}
                      onModel={model => updateProvider(provider.id, { model })}
                      onChange={videoParameters => updateProvider(provider.id, { videoParameters })} />
                  </>}
                  {isCustom && (
                    <label className="ui-field" htmlFor="api-settings-custom-url">
                      <span className="ui-field__label">Base URL</span>
                      <input
                        id="api-settings-custom-url"
                        className="ui-control api-settings__secret-input"
                        value={config.baseUrl}
                        onChange={event => updateProvider(provider.id, { baseUrl: event.target.value,
                          ...(cleanEndpoint(event.target.value) !== cleanEndpoint(draft.baseUrl) ? { apiKey: "" } : {}) })}
                        placeholder="https://api.example.com"
                      />
                    </label>
                  )}
                  {isCustom && <label className="ui-field" htmlFor="api-settings-custom-model">
                    <span className="ui-field__label">视频生成模型</span>
                    <input id="api-settings-custom-model" className="ui-control" value={config.model}
                      onChange={event => updateProvider(provider.id, { model: event.target.value })} />
                  </label>}
                  {provider.id === "volcengine" && <div className="ui-field">
                    <span className="ui-field__label">视频生成模型</span>
                    <VideoModelSelect apiKey={config.apiKey} baseUrl={config.baseUrl}
                      model={config.model} onModel={model => updateProvider(provider.id, { model })}
                      autoSelectDefault={false} autoLoad={false} />
                    <span className="muted small">用于生成形象视频；与下方提示词模型独立。修改后点击保存。</span>
                  </div>}
                  {provider.id === "volcengine" && <div className="ui-field">
                    <span className="ui-field__label">图像理解 / 提示词模型</span>
                    <TextModelSelect vision autoLoad={false} label="图像理解模型" value={config.thinkingModel}
                      onChange={thinkingModel => updateProvider(provider.id, { thinkingModel })}
                      apiKey={config.apiKey} baseUrl={config.baseUrl} />
                  </div>}
                  {provider.id === "volcengine" && <div className="ui-field">
                    <span className="ui-field__label">图片处理模型</span>
                    <ImageModelSelect autoLoad={false} value={config.imageModel}
                      onChange={imageModel => updateProvider(provider.id, { imageModel })}
                      apiKey={config.apiKey} baseUrl={config.baseUrl} />
                  </div>}
                </fieldset>

                {result?.checks && <ul className="api-settings__probe-results" aria-label="本地配置检查分项">
                  {result.checks.map(check => <li key={check.label} className={check.ok ? "" : "is-error"}>
                    <strong>{check.label}</strong> {check.message}
                  </li>)}
                </ul>}
                <footer className="api-settings__provider-foot">
                  <span className={`api-settings__provider-result${resultToneClass(result?.tone)}`}>
                    {result?.message || (configReadError ? "原配置保留，暂不可修改" : configured ? "凭据已保存" : "等待填写凭据")}
                  </span>
                  <button type="button" className="btn-ghost btn-sm" disabled={Boolean(configReadError) || providerPending[provider.id]}
                    onClick={() => saveProvider(provider.id, true)}>检查本地配置</button>
                  <button
                    type="button"
                    className="btn-secondary btn-sm"
                    onClick={() => saveProvider(provider.id)}
                    disabled={providerPending[provider.id] === true || Boolean(configReadError)}
                  >
                    {providerPending[provider.id] ? "保存中…" : "保存"}
                  </button>
                </footer>
              </section>
            );
          })}
        </div>
      </Card>
      <Card title="云服务网络" id="model-settings-network"><LlmNetworkSettings /></Card>
    </PageShell>
    </div>
  );
}
