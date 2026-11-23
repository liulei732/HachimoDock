/*
 * [Input] 对话大模型（DeepSeek / 豆包 / MiMo 预设或自定义 OpenAI 兼容端点）与豆包 Seed TTS 2.0
 *         的凭据；合并保存时在同一加密记录中提交 ASR/TTS，导入旧语音配置并按服务地址隔离凭据。
 * [Output] One authenticated encrypted JSON file under the app data dir (`voice-chat-settings.json`,
 *          local private key on macOS / OS vault on Windows, 0600 on unix), masked status for the UI, and resolved LLM/TTS configs for the realtime chat
 *          pipeline. LLM keys are saved per provider (custom keys also per endpoint),
 *          with only masks exposed to the UI. Separate global realtime volume persistence migrates legacy per-pet gain once.
 *          Public builds embed nothing; internal builds may embed defaults.
 * [Pos] Tauri-side credential store for realtime persona chat (mirrors volcengine_asr's file mode).
 * [Sync] If fields change, update `ApiSettings.jsx`, `pc/.folder.md`, and `pc/docs/realtime-chat.md`.
 */

pub(crate) mod custom_speech;

use crate::model_issue::{self, LLM};

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, OnceLock};

use crate::doubao_tts::{DoubaoTtsConfig, DEFAULT_RESOURCE_ID as TTS_DEFAULT_RESOURCE_ID};
use crate::persona_llm::{LlmConfig, DEFAULT_BASE_URL as LLM_DEFAULT_BASE_URL, DEFAULT_MODEL as LLM_DEFAULT_MODEL};

use crate::volcengine_asr::{self, DeviceAsrSettingsInput, DeviceAsrSettingsStatus, StoredSpeechSettings};

const FILE_NAME: &str = "voice-chat-settings.json";

static STORAGE_DIR: OnceLock<PathBuf> = OnceLock::new();

// Separate from credentials and per-appearance voices. Saving volume must not
// rewrite keys, and switching appearance must never restore an old gain.
const VOLUME_FILE: &str = "realtime-voice-volume.json";
pub(crate) const DEFAULT_REALTIME_VOLUME: f32 = 0.8;
static VOLUME_LOCK: Mutex<()> = Mutex::new(());
static SETTINGS_MUTATION: Mutex<()> = Mutex::new(());

fn valid_volume(volume: f32) -> bool {
    volume.is_finite() && (0.2..=1.5).contains(&volume)
}

fn write_volume_at(dir: &Path, volume: f32) -> Result<f32, String> {
    if !valid_volume(volume) { return Err("实时对话音量须在 0.20–1.50 之间".into()); }
    fs::create_dir_all(dir).map_err(|_| "无法创建音量配置目录")?;
    let mut temp = tempfile::NamedTempFile::new_in(dir).map_err(|_| "无法创建音量配置临时文件")?;
    let bytes = serde_json::to_vec(&serde_json::json!({"version":1,"volume":volume})).map_err(|_| "音量配置无效")?;
    temp.write_all(&bytes).and_then(|_| temp.as_file().sync_all()).map_err(|_| "无法写入音量配置")?;
    temp.persist(dir.join(VOLUME_FILE)).map_err(|_| "无法保存音量配置")?;
    Ok(volume)
}

fn read_volume_at(dir: &Path) -> Result<f32, String> {
    match fs::read(dir.join(VOLUME_FILE)) {
        Ok(raw) => {
            let value: serde_json::Value = serde_json::from_slice(&raw).map_err(|_| "全局音量配置损坏，请重新保存音量")?;
            return value["volume"].as_f64().map(|v| v as f32).filter(|v| valid_volume(*v))
                .ok_or_else(|| "全局音量配置无效，请重新保存音量".into());
        }
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {},
        Err(_) => return Err("无法读取全局音量配置".into()),
    }
    // One-time migration: preserve the user's most recently saved voice gain.
    // Do not read media or credentials, follow symlinks, or export persona text.
    let mut latest = None;
    if let Ok(entries) = fs::read_dir(dir.join("custom-appearances")) {
        for entry in entries.take(1024).flatten() {
            if !entry.file_type().is_ok_and(|t| t.is_dir()) { continue; }
            let path = entry.path().join("persona-voice.json");
            let Ok(meta) = fs::symlink_metadata(&path) else { continue };
            if !meta.is_file() || meta.len() > 65536 { continue; }
            let Ok(raw) = fs::read(&path) else { continue };
            let Ok(value) = serde_json::from_slice::<serde_json::Value>(&raw) else { continue };
            let Some(volume) = value["voice"]["volume"].as_f64().map(|v| v as f32).filter(|v| valid_volume(*v)) else { continue };
            let Ok(modified) = meta.modified() else { continue };
            if latest.is_none_or(|(time, _)| modified > time) { latest = Some((modified, volume)); }
        }
    }
    write_volume_at(dir, latest.map(|(_, v)| v).unwrap_or(DEFAULT_REALTIME_VOLUME))
}

pub fn realtime_volume() -> Result<f32, String> {
    let _lock = VOLUME_LOCK.lock().map_err(|_| "音量配置暂不可用")?;
    read_volume_at(STORAGE_DIR.get().ok_or("语音对话配置目录尚未初始化")?)
}

pub fn save_realtime_volume(volume: f32) -> Result<f32, String> {
    let _lock = VOLUME_LOCK.lock().map_err(|_| "音量配置暂不可用")?;
    write_volume_at(STORAGE_DIR.get().ok_or("语音对话配置目录尚未初始化")?, volume)
}

/// Built-in chat defaults: endpoints are fixed, model names may be overridden.
pub struct LlmPreset {
    pub id: &'static str,
    pub label: &'static str,
    pub base_url: &'static str,
    pub model: &'static str,
}

pub const LLM_PRESETS: &[LlmPreset] = &[
    LlmPreset { id: "deepseek", label: "DeepSeek", base_url: "https://api.deepseek.com", model: "deepseek-flash" },
    LlmPreset { id: "doubao", label: "豆包（火山方舟）", base_url: "https://ark.cn-beijing.volces.com/api/v3", model: "doubao-seed-2-1-lite-260915" },
    LlmPreset { id: "mimo", label: "Xiaomi MiMo", base_url: "https://api.xiaomimimo.com/v1", model: "mimo-v2.6-flash" },
    LlmPreset { id: "mimo-token-plan-cn", label: "MiMo Token Plan", base_url: "https://token-plan-cn.xiaomimimo.com/v1", model: "mimo-v2.6-flash" },
];
pub const DEFAULT_LLM_PROVIDER: &str = "doubao";

pub fn llm_preset(id: &str) -> Option<&'static LlmPreset> {
    LLM_PRESETS.iter().find(|preset| preset.id == id)
}

fn normalize_provider(value: &str) -> String {
    let id = value.trim().to_ascii_lowercase();
    if id == "custom" || llm_preset(&id).is_some() {
        id
    } else {
        DEFAULT_LLM_PROVIDER.to_string()
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase", default)]
pub struct VoiceChatSettings {
    /// A preset ID from LLM_PRESETS, or "custom". The model can be set independently.
    pub llm_provider: String,
    pub llm_base_url: String,
    pub llm_model: String,
    #[serde(deserialize_with = "deserialize_saved_key")]
    pub llm_api_key: String,
    /// Presets are scoped by provider; custom keys are additionally scoped by endpoint.
    pub llm_api_keys: BTreeMap<String, String>,
    pub llm_models: BTreeMap<String, String>,
    pub llm_custom_base_url: String,
    pub llm_web_search: Option<bool>,
    pub llm_search_mode: crate::web_search::Mode,
    #[serde(deserialize_with = "deserialize_saved_key")]
    pub llm_search_api_key: String,
    #[serde(deserialize_with = "deserialize_saved_key")]
    pub tts_api_key: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    speech_profiles: Option<StoredSpeechSettings>,
    speech_preset_checked: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    custom_tts_credential: Option<custom_speech::BoundKey>,
    pub tts_resource_id: String,
    /// The editable endpoint/model/voice below retain the custom profile across preset switches.
    #[serde(default = "default_tts_provider")]
    pub tts_provider: String,
    #[serde(default)]
    pub tts_base_url: String,
    #[serde(default)]
    pub tts_model: String,
    #[serde(default)]
    pub tts_voice: String,
    mimo_tts_model: String,
    mimo_tts_voice: String,
}

fn default_tts_provider() -> String { "volcengine".into() }

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "camelCase", default)]
pub struct VoiceChatSettingsInput {
    pub llm_provider: Option<String>,
    pub llm_base_url: Option<String>,
    pub llm_model: Option<String>,
    pub llm_api_key: Option<String>,
    pub llm_web_search: Option<bool>,
    pub llm_search_mode: Option<crate::web_search::Mode>,
    pub llm_search_api_key: Option<String>,
    pub tts_api_key: Option<String>,
    pub tts_resource_id: Option<String>,
    pub tts_provider: Option<String>,
    pub tts_base_url: Option<String>,
    pub tts_model: Option<String>,
    pub tts_voice: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct VoiceChatSettingsStatus {
    pub llm_web_search: bool,
    pub llm_search_mode: crate::web_search::Mode,
    pub llm_search_configured: bool,
    pub llm_configured: bool,
    pub tts_configured: bool,
    pub llm_provider: String,
    /// true when the TTS key falls back to the speech-recognition key
    pub tts_uses_asr_key: bool,
    pub llm_base_url: String,
    pub llm_model: String,
    pub llm_api_key_masked: String,
    pub llm_api_key_masks: BTreeMap<String, String>,
    pub llm_models: BTreeMap<String, String>,
    pub llm_custom_base_url: String,
    pub tts_api_key_masked: String,
    pub tts_resource_id: String,
    pub tts_provider: String,
    pub tts_base_url: String,
    pub tts_model: String,
    pub tts_voice: String,
    pub mimo_tts_model: String,
    pub mimo_tts_voice: String,
    pub custom_tts_model: String,
    pub custom_tts_voice: String,
    pub credential_source: &'static str,
}

pub fn configure_storage_dir(path: PathBuf) -> Result<(), String> {
    match STORAGE_DIR.set(path.clone()) {
        Ok(()) => Ok(()),
        Err(_) if STORAGE_DIR.get() == Some(&path) => Ok(()),
        Err(_) => Err("语音对话配置目录已初始化为其他路径".to_string()),
    }
}

fn settings_path() -> Result<PathBuf, String> {
    let dir = STORAGE_DIR.get().ok_or_else(|| "[CREDENTIALS] 语音对话配置目录尚未初始化".to_string())?;
    Ok(dir.join(FILE_NAME))
}

pub(crate) fn credential_placeholder(value: &str) -> bool {
    value.contains('…') || value.trim() == "••••" || value.trim() == crate::service_credentials::ARK_HANDLE
}

pub(crate) fn deserialize_saved_key<'de, D: serde::Deserializer<'de>>(deserializer: D) -> Result<String, D::Error> {
    let key = String::deserialize(deserializer)?;
    Ok(if credential_placeholder(&key) { String::new() } else { key })
}

pub fn mask_secret(value: &str) -> String {
    let trimmed = value.trim();
    if trimmed.is_empty() {
        return String::new();
    }
    let chars: Vec<char> = trimmed.chars().collect();
    if chars.len() <= 8 {
        return "••••".to_string();
    }
    let head: String = chars[..3].iter().collect();
    let tail: String = chars[chars.len() - 4..].iter().collect();
    format!("{head}…{tail}")
}

fn read_file(path: &Path) -> Result<Option<VoiceChatSettings>, String> {
    match crate::secure_config::read(path,"voice-chat").map_err(|e| model_issue::local(model_issue::CREDENTIALS, e))? {
        Some(raw) => serde_json::from_str::<VoiceChatSettings>(&raw)
            .map_err(|_| "[CREDENTIALS] 语音对话配置格式损坏；请保留原文件".to_string())
            .and_then(|mut settings| {
                settings.llm_api_keys.retain(|_, key| !credential_placeholder(key));
                // Pre-provider files used DeepSeek by default. Do not redirect
                // an old credential when changing the first-install default.
                if settings.llm_provider.trim().is_empty() && (!settings.llm_api_key.is_empty()
                    || !settings.llm_base_url.is_empty() || !settings.llm_model.is_empty()) {
                    settings.llm_provider = LLM_PRESETS.iter().find(|p|
                        (!settings.llm_base_url.is_empty() && settings.llm_base_url.trim().trim_end_matches('/') == p.base_url)
                        || (settings.llm_base_url.is_empty() && settings.llm_model.starts_with(p.id)))
                        .map(|p| p.id).unwrap_or(if settings.llm_base_url.is_empty() { "deepseek" } else { "custom" }).into();
                }
                upgrade_legacy_mimo(&mut settings, volcengine_asr::read_legacy_config)?;
                Ok(Some(settings))
            }),
        None => Ok(None),
    }
}

fn write_file(path: &Path, settings: &VoiceChatSettings) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|_| "[CREDENTIALS] 无法创建凭据目录，请检查写入权限")?;
    }
    let raw = serde_json::to_string_pretty(settings).map_err(|_| "[CREDENTIALS] 无法编码本地凭据设置")?;
    crate::secure_config::write(path,"voice-chat",raw.as_bytes()).map_err(|e| model_issue::local(model_issue::CREDENTIALS, e))
}

/// Stored user credentials and public model defaults; no embedded credentials.
pub fn load() -> Result<(VoiceChatSettings, &'static str), String> {
    let stored = read_file(&settings_path()?)?.unwrap_or_default();
    let mut settings = stored;
    remember_active_key(&mut settings);
    if settings.llm_api_key.trim().is_empty() {
        settings.llm_api_key = settings.llm_api_keys.get(&credential_scope(&settings)).cloned().unwrap_or_default();
    }
    apply_llm_defaults(&mut settings);
    settings.tts_resource_id = TTS_DEFAULT_RESOURCE_ID.to_string();
    Ok((settings, "local"))
}

fn apply_llm_defaults(settings: &mut VoiceChatSettings) {
    settings.llm_provider = normalize_provider(&settings.llm_provider);
    settings.llm_model = settings.llm_model.trim().to_string();
    // Migrate the previous incomplete preset name, not user-selected model IDs.
    if settings.llm_provider == "mimo" && settings.llm_model == "mimo-v2.6" {
        settings.llm_model = "mimo-v2.6-flash".into();
    }
    if let Some(preset) = llm_preset(&settings.llm_provider) {
        // Keep preset credentials on the official endpoint, but honor the saved model.
        settings.llm_base_url = preset.base_url.to_string();
        if settings.llm_model.is_empty() {
            settings.llm_model = preset.model.to_string();
        }
    } else {
        if settings.llm_base_url.trim().is_empty() {
            settings.llm_base_url = LLM_DEFAULT_BASE_URL.to_string();
        }
        if settings.llm_model.trim().is_empty() {
            settings.llm_model = LLM_DEFAULT_MODEL.to_string();
        }
    }
}

pub(crate) const MIMO_ROOT: &str = "https://api.xiaomimimo.com/v1";
pub(crate) const MIMO_ASR: &str = "mimo-v2.5-asr";
const MIMO_TTS: &str = "mimo-v2.5-tts";
pub(crate) const MIMO_VOICE: &str = "mimo_default";

pub(crate) fn official_mimo_endpoint(address: &str) -> bool {
    // Match only literal, previously routable official URLs. A proxy, Token Plan
    // host, different protocol or path must keep its original credential scope.
    matches!(address.trim().strip_suffix('/').unwrap_or(address.trim()),
        "https://api.xiaomimimo.com" | MIMO_ROOT | "https://api.xiaomimimo.com/v1/chat/completions")
}

fn upgrade_legacy_mimo(settings: &mut VoiceChatSettings,
    legacy: impl FnOnce() -> Result<Option<StoredSpeechSettings>, String>,
) -> Result<(), String> {
    if settings.custom_tts_credential.is_some() || !matches!(settings.tts_provider.as_str(), "openai" | "volcengine")
        || !official_mimo_endpoint(&settings.tts_base_url) || settings.tts_model.trim().is_empty() {
        return Ok(());
    }
    let promote = !settings.speech_preset_checked && settings.tts_provider == "openai"
        && settings.tts_model.trim() == MIMO_TTS && matches!(settings.tts_voice.trim(), "" | MIMO_VOICE);
    let key = if settings.tts_provider == "openai" { settings.tts_api_key.as_str() } else { "" };
    let unified = settings.speech_profiles.is_some();
    let profiles = match settings.speech_profiles.take() {
        Some(p) => Some(p),
        None => legacy()?,
    };
    if let Some(mut profiles) = profiles {
        if profiles.assess_mimo_key(key, promote) {
            settings.tts_provider = "mimo".into();
            settings.tts_api_key.clear(); // identical to the promoted ASR key
        }
        if unified || profiles.has_mimo_key() { settings.speech_profiles = Some(profiles); }
    }
    settings.speech_preset_checked = true;
    Ok(())
}

pub(crate) fn buffered_speech(provider: &str) -> bool { matches!(provider, "openai" | "mimo") }

pub(crate) fn speech_value_or_default<'a>(value: &'a str, default: &'a str) -> &'a str {
    match value.trim() {
        "" => default,
        value => value,
    }
}

impl VoiceChatSettings {
    fn set_mimo_tts(&mut self, model: Option<String>, voice: Option<String>) {
        if let Some(model) = model { self.mimo_tts_model = model.trim().into(); }
        if let Some(voice) = voice { self.mimo_tts_voice = voice.trim().into(); }
    }

    fn mimo_tts_route(&self) -> (&str, &str, &str) {
        (MIMO_ROOT, speech_value_or_default(&self.mimo_tts_model, MIMO_TTS),
            speech_value_or_default(&self.mimo_tts_voice, MIMO_VOICE))
    }

    fn tts_route(&self) -> (&str, &str, &str) {
        if self.tts_provider == "mimo" { self.mimo_tts_route() }
        else { (&self.tts_base_url, &self.tts_model, &self.tts_voice) }
    }

    fn speech_provider(&self) -> &str {
        if self.tts_provider.is_empty() { "volcengine" } else { &self.tts_provider }
    }
}

pub(crate) struct CustomSpeechEndpoint {
    pub api_root: String,
    pub uses_chat_completions: bool,
}

impl CustomSpeechEndpoint {
    pub(crate) fn authorize(&self, request: reqwest::RequestBuilder, key: &str) -> reqwest::RequestBuilder {
        let key = key.trim();
        if key.is_empty() { return request; }
        if self.uses_chat_completions {
            crate::web_search::authorize(request, &format!("{}/chat/completions", self.api_root), key)
        } else {
            request.bearer_auth(key)
        }
    }
}

/// Explicit API paths select the protocol. Existing MiMo base URLs use the
/// native speech contract without asking users to reenter their credentials.
pub(crate) fn custom_speech_endpoint(value: &str) -> Result<CustomSpeechEndpoint, String> {
    let url = reqwest::Url::parse(value.trim()).map_err(|_| "语音服务地址无效")?;
    let host = url.host_str().ok_or("语音服务地址缺少主机")?;
    let loopback = host == "localhost"
        || host.trim_matches(['[', ']']).parse::<std::net::IpAddr>().is_ok_and(|ip| ip.is_loopback());
    if !(url.scheme() == "https" || (url.scheme() == "http" && loopback))
        || !url.username().is_empty() || url.password().is_some()
        || url.query().is_some() || url.fragment().is_some()
    {
        return Err("语音服务需使用 HTTPS，本机回环地址可用 HTTP，地址中请勿携带凭据".into());
    }
    let address = url.as_str().trim_end_matches('/');
    if let Some(root) = address.strip_suffix("/chat/completions") {
        return Ok(CustomSpeechEndpoint { api_root: root.into(), uses_chat_completions: true });
    }
    if let Some(root) = address.strip_suffix("/audio/transcriptions").or_else(|| address.strip_suffix("/audio/speech")) {
        return Ok(CustomSpeechEndpoint { api_root: root.into(), uses_chat_completions: false });
    }
    let api_root = if url.path().trim_end_matches('/').ends_with("/v1") {
        address.to_owned()
    } else {
        format!("{address}/v1")
    };
    Ok(CustomSpeechEndpoint {
        api_root,
        uses_chat_completions: matches!(
            host,
            "api.xiaomimimo.com"
                | "token-plan-cn.xiaomimimo.com"
                | "token-plan-sgp.xiaomimimo.com"
                | "token-plan-ams.xiaomimimo.com"
        ),
    })
}

fn saved_tts_key(settings: &VoiceChatSettings) -> Result<Option<String>, String> {
    let provider = settings.speech_provider();
    if provider == "openai" {
        if let Some(bound) = &settings.custom_tts_credential { return Ok(bound.for_tts(&settings.tts_base_url)); }
        if !settings.tts_api_key.trim().is_empty() && custom_speech_endpoint(&settings.tts_base_url).is_ok() {
            return Ok(Some(settings.tts_api_key.clone()));
        }
    }
    let key = match &settings.speech_profiles {
        Some(speech) => speech.tts_key_for_scope(provider, settings.tts_route().0),
        None => volcengine_asr::read_legacy_config()?.and_then(|s| s.tts_key_for_scope(provider, settings.tts_route().0)),
    };
    Ok(key)
}

fn resolve_tts_key(settings: &VoiceChatSettings) -> Result<(String, bool), String> {
    let provider = settings.speech_provider();
    if provider == "openai" {
        if let Some(bound) = &settings.custom_tts_credential {
            return Ok((bound.for_tts(&settings.tts_base_url).unwrap_or_default(), false));
        }
        // Honor an explicit legacy TTS credential even when ASR has an environment key.
        let legacy;
        let profiles = match &settings.speech_profiles {
            Some(saved) => Some(saved),
            None => { legacy = volcengine_asr::read_legacy_config()?; legacy.as_ref() }
        };
        let saved = if !settings.tts_api_key.trim().is_empty() {
            Some(settings.tts_api_key.clone())
        } else {
            profiles.and_then(|p| p.tts_key_for_scope(provider, &settings.tts_base_url))
        };
        if let Some(key) = saved {
            let shared = profiles.and_then(StoredSpeechSettings::custom_asr)
                .is_some_and(|(url, _, asr_key)| asr_key == key && custom_speech::Stage::Tts.same_destination(url, &settings.tts_base_url));
            return Ok((key, shared));
        }
    }
    if let Some(key) = volcengine_asr::environment_key_for_scope(provider, settings.tts_route().0) {
        return Ok((key, true));
    }
    Ok(match saved_tts_key(settings)? { Some(key) => (key, true), None => (String::new(), false) })
}

pub(crate) fn saved_speech_profiles() -> Result<Option<StoredSpeechSettings>, String> {
    let Some(dir) = STORAGE_DIR.get() else { return Ok(None); };
    Ok(read_file(&dir.join(FILE_NAME))?.and_then(|settings| settings.speech_profiles))
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SpeechSettingsInput {
    #[serde(flatten)]
    pub asr: DeviceAsrSettingsInput,
    pub tts_model: Option<String>,
    pub tts_voice: Option<String>,
}

pub fn save_speech(input: SpeechSettingsInput) -> Result<DeviceAsrSettingsStatus, String> {
    save_speech_at(&settings_path()?, input, volcengine_asr::read_legacy_config, write_file)
}

fn save_speech_at(path: &Path, mut input: SpeechSettingsInput,
    legacy: impl FnOnce() -> Result<Option<StoredSpeechSettings>, String>,
    commit: impl FnOnce(&Path, &VoiceChatSettings) -> Result<(), String>,
) -> Result<DeviceAsrSettingsStatus, String> {
    let _guard = SETTINGS_MUTATION.lock().map_err(|_| "语音配置繁忙")?;
    let mut settings = read_file(path)?.unwrap_or_default();
    let custom = match input.asr.provider.as_deref() {
        Some("openai") => true, Some("volcengine" | "mimo") => false,
        _ => return Err("请选择语音服务".into()),
    };
    if custom {
        custom_speech_endpoint(input.asr.base_url.as_deref().unwrap_or_default())?;
        if input.asr.model.as_deref().unwrap_or_default().trim().is_empty()
            || input.tts_model.as_deref().unwrap_or_default().trim().is_empty() {
            return Err("请填写语音识别模型和合成模型".into());
        }
    }
    if settings.custom_tts_credential.is_some() && custom {
        custom_speech::bind_combined_keys(&mut settings, &mut input)?;
    }
    let split = settings.custom_tts_credential.is_some();
    let profiles = materialize_speech(&mut settings, legacy)?;
    if split { profiles.pin_selection(); }
    let mut status = profiles.apply(input.asr)?;
    status.credential_source = "secure-config";
    settings.speech_preset_checked = true;
    settings.tts_provider = status.provider.clone();
    if custom {
        settings.tts_base_url = status.base_url.clone();
        settings.tts_model = input.tts_model.unwrap_or_default().trim().into();
        settings.tts_voice = input.tts_voice.unwrap_or_default().trim().into();
    } else if status.provider == "mimo" {
        settings.set_mimo_tts(input.tts_model, input.tts_voice);
    }
    settings.tts_resource_id = TTS_DEFAULT_RESOURCE_ID.into();
    upgrade_legacy_mimo(&mut settings, || Ok(None))?;
    status.saved_mimo = settings.speech_profiles.as_ref().is_some_and(StoredSpeechSettings::has_mimo_key);
    // Validate and prepare the public receipt before the single atomic replacement.
    commit(path, &settings)?;
    Ok(status)
}

fn materialize_speech(settings: &mut VoiceChatSettings,
    legacy: impl FnOnce() -> Result<Option<StoredSpeechSettings>, String>,
) -> Result<&mut StoredSpeechSettings, String> {
    let profiles = match &mut settings.speech_profiles {
        Some(profiles) => profiles,
        slot @ None => slot.insert(legacy()?.unwrap_or_default()),
    };
    if !settings.tts_api_key.is_empty() {
        let url = if settings.tts_provider == "mimo" { MIMO_ROOT } else { &settings.tts_base_url };
        profiles.import_key(&settings.tts_provider, url, &settings.tts_api_key)?;
        settings.tts_api_key.clear();
    }
    Ok(profiles)
}

pub(crate) fn save_asr(input: DeviceAsrSettingsInput) -> Result<DeviceAsrSettingsStatus, String> {
    save_asr_at(STORAGE_DIR.get().map(|dir| dir.join(FILE_NAME)).as_deref(), input)
}

fn save_asr_at(path: Option<&Path>, input: DeviceAsrSettingsInput) -> Result<DeviceAsrSettingsStatus, String> {
    let _guard = SETTINGS_MUTATION.lock().map_err(|_| "语音配置繁忙")?;
    if let Some(path) = path {
        if let Some(mut settings) = read_file(path)? {
            if let Some(profiles) = settings.speech_profiles.as_mut() {
                let mut input = input;
                if settings.custom_tts_credential.is_some() {
                    custom_speech::bind_asr_edit(profiles, &mut input)?;
                    profiles.pin_selection();
                }
                let mut status = profiles.apply(input)?;
                status.credential_source = "secure-config";
                write_file(path, &settings)?;
                return Ok(status);
            }
        }
    }
    volcengine_asr::save_legacy_asr(input)
}

/// Keep the existing ASR key; migrate a TTS-only installation before removing its old override.
/// Copy first, then clear: a failed credential-store write must never lose the only key.
pub fn migrate_shared_speech_key() -> Result<(), String> {
    migrate_shared_speech_key_at(&settings_path()?, crate::volcengine_asr::import_legacy_key)
}

fn migrate_shared_speech_key_at(path: &Path,
    import_key: impl FnOnce(&str, &str, &str) -> Result<bool, String>) -> Result<(), String> {
    let _guard = SETTINGS_MUTATION.lock().map_err(|_| "语音配置繁忙")?;
    if let Some(mut stored) = read_file(path)? {
        let has_key = !stored.tts_api_key.trim().is_empty();
        if has_key {
            let url = if stored.tts_provider == "mimo" { MIMO_ROOT } else { &stored.tts_base_url };
            let imported = if let Some(speech) = stored.speech_profiles.as_mut() {
                speech.import_key(&stored.tts_provider, url, stored.tts_api_key.trim())?;
                true
            } else { import_key(&stored.tts_provider, url, stored.tts_api_key.trim())? };
            if imported { stored.tts_api_key.clear(); }
        }
        if (has_key && stored.tts_api_key.is_empty()) || stored.tts_resource_id != TTS_DEFAULT_RESOURCE_ID {
            stored.tts_resource_id = TTS_DEFAULT_RESOURCE_ID.into();
            write_file(path, &stored)?;
        }
    }
    Ok(())
}

pub fn status() -> Result<VoiceChatSettingsStatus, String> {
    migrate_shared_speech_key()?;
    let (settings, source) = load()?;
    let (tts_key, tts_uses_asr_key) = resolve_tts_key(&settings)?;
    Ok(VoiceChatSettingsStatus {
        llm_web_search: settings.llm_web_search.unwrap_or(true),
        llm_search_mode: settings.llm_search_mode,
        llm_search_configured: !settings.llm_search_api_key.trim().is_empty(),
        llm_configured: !settings.llm_api_key.trim().is_empty(),
        tts_configured: !tts_key.is_empty(),
        llm_provider: settings.llm_provider.clone(),
        tts_uses_asr_key,
        llm_base_url: settings.llm_base_url.clone(),
        llm_model: settings.llm_model.clone(),
        llm_api_key_masked: mask_secret(&settings.llm_api_key),
        llm_api_key_masks: credential_masks(&settings),
        llm_models: settings.llm_models.clone(),
        llm_custom_base_url: settings.llm_custom_base_url.clone(),
        tts_api_key_masked: mask_secret(&tts_key),
        tts_resource_id: settings.tts_resource_id.clone(),
        tts_provider: settings.speech_provider().into(),
        tts_base_url: settings.tts_route().0.into(),
        tts_model: settings.tts_route().1.into(),
        tts_voice: settings.tts_route().2.into(),
        mimo_tts_model: settings.mimo_tts_route().1.into(),
        mimo_tts_voice: settings.mimo_tts_route().2.into(),
        custom_tts_model: settings.tts_model.clone(),
        custom_tts_voice: settings.tts_voice.clone(),
        credential_source: source,
    })
}

fn credential_scope(settings: &VoiceChatSettings) -> String {
    let provider = normalize_provider(&settings.llm_provider);
    if provider == "custom" {
        let url = settings.llm_base_url.trim().trim_end_matches('/');
        format!("custom:{}", if url.is_empty() { LLM_DEFAULT_BASE_URL } else { url })
    } else { provider }
}

fn remember_active_key(settings: &mut VoiceChatSettings) {
    let scope = credential_scope(settings);
    if settings.llm_model.trim().is_empty() {
        settings.llm_models.remove(&scope);
    } else {
        settings.llm_models.insert(scope, settings.llm_model.trim().to_owned());
    }
    if normalize_provider(&settings.llm_provider) == "custom" {
        settings.llm_custom_base_url = settings.llm_base_url.trim().to_owned();
    }
    if !settings.llm_api_key.trim().is_empty() {
        settings.llm_api_keys.insert(credential_scope(settings), settings.llm_api_key.clone());
    }
}

fn credential_masks(settings: &VoiceChatSettings) -> BTreeMap<String, String> {
    let mut masks: BTreeMap<_, _> = settings.llm_api_keys.iter().filter(|(_, key)| !key.trim().is_empty())
        .map(|(scope, key)| (scope.clone(), mask_secret(key))).collect();
    if !settings.llm_api_key.trim().is_empty() {
        masks.insert(credential_scope(settings), mask_secret(&settings.llm_api_key));
    }
    masks
}

/// Keep each saved credential in its own scope. An empty input reuses only the
/// target provider/endpoint's key, never the previously selected provider's key.
pub fn save(input: VoiceChatSettingsInput) -> Result<VoiceChatSettingsStatus, String> {
    save_voice_at(&settings_path()?, input)?;
    status()
}

fn save_voice_at(path: &Path, input: VoiceChatSettingsInput) -> Result<(), String> {
    let _guard = SETTINGS_MUTATION.lock().map_err(|_| "语音配置繁忙")?;
    let input_edits_llm = input.llm_provider.is_some() || input.llm_base_url.is_some() || input.llm_api_key.is_some() || input.llm_model.is_some();
    let mut prior = read_file(path)?.unwrap_or_default();
    if input.tts_provider.as_deref().unwrap_or(&prior.tts_provider) == "mimo"
        && input.tts_base_url.as_deref().is_some_and(|v| !official_mimo_endpoint(v)) {
        return Err("小米 MiMo 预设使用固定的服务地址".into());
    }
    let mut input = input;
    if prior.custom_tts_credential.is_some() {
        custom_speech::bind_legacy_tts_edit(&mut prior, &mut input)?;
    }
    if !prior.tts_api_key.is_empty() && (input.tts_provider.is_some() || input.tts_base_url.is_some()) {
        // Pin a legacy key to its recorded destination before applying a legacy TTS edit.
        materialize_speech(&mut prior, volcengine_asr::read_legacy_config)?;
    }
    let mut settings = merge_settings(prior, input);
    if settings.speech_profiles.is_some() && !settings.tts_api_key.is_empty() {
        materialize_speech(&mut settings, volcengine_asr::read_legacy_config)?;
    }
    if settings.llm_provider == "custom" && input_edits_llm {
        validate_model_endpoint(&settings.llm_base_url)?;
    }
    write_file(path, &settings)
}

fn merge_settings(mut settings: VoiceChatSettings, input: VoiceChatSettingsInput) -> VoiceChatSettings {
    if let Some(enabled) = input.llm_web_search { settings.llm_web_search = Some(enabled); }
    if let Some(mode) = input.llm_search_mode { settings.llm_search_mode = mode; }
    if let Some(key) = input.llm_search_api_key {
        let trimmed = key.trim();
        if !trimmed.is_empty() && !credential_placeholder(trimmed) {
            settings.llm_search_api_key = trimmed.to_owned();
        }
    }
    remember_active_key(&mut settings);
    let previous_scope = credential_scope(&settings);
    if let Some(value) = input.llm_provider {
        let provider = normalize_provider(&value);
        if normalize_provider(&settings.llm_provider) != provider {
            settings.llm_model.clear();
            settings.llm_base_url.clear();
            if provider == "custom" { settings.llm_base_url = settings.llm_custom_base_url.clone(); }
        }
        settings.llm_provider = provider;
    }
    if let Some(value) = input.llm_base_url {
        settings.llm_base_url = value.trim().to_string();
    }
    let next_scope = credential_scope(&settings);
    if previous_scope != next_scope {
        settings.llm_model = settings.llm_models.get(&next_scope).cloned().unwrap_or_default();
    }
    if previous_scope != next_scope || settings.llm_api_key.trim().is_empty() {
        settings.llm_api_key = settings.llm_api_keys.get(&next_scope).cloned().unwrap_or_default();
    }
    if let Some(value) = input.llm_model {
        settings.llm_model = value.trim().to_string();
    }
    if let Some(value) = input.llm_api_key {
        let trimmed = value.trim();
        if !trimmed.is_empty() && !credential_placeholder(trimmed) {
            settings.llm_api_key = trimmed.to_string();
        }
    }
    if let Some(value) = input.tts_api_key {
        let trimmed = value.trim();
        if !trimmed.is_empty() && !credential_placeholder(trimmed) {
            settings.tts_api_key = trimmed.to_string();
        }
    }
    if let Some(value) = input.tts_resource_id {
        settings.tts_resource_id = value.trim().to_string();
    }
    if let Some(value) = input.tts_provider {
        let v = value.trim();
        settings.tts_provider = if buffered_speech(v) { v.into() } else { "volcengine".into() };
        settings.speech_preset_checked = true;
    }
    if settings.tts_provider == "mimo" {
        settings.set_mimo_tts(input.tts_model, input.tts_voice);
    } else {
        if let Some(value) = input.tts_base_url {
            settings.tts_base_url = value.trim().to_string();
        }
        if let Some(value) = input.tts_model {
            settings.tts_model = value.trim().to_string();
        }
        if let Some(value) = input.tts_voice {
            settings.tts_voice = value.trim().to_string();
        }
    }
    remember_active_key(&mut settings);
    settings
}

fn forget_llm_key(settings: &mut VoiceChatSettings, scope: &str) -> Result<(), String> {
    if llm_preset(scope).is_none() && !scope.strip_prefix("custom:").is_some_and(|url| !url.trim().is_empty()) {
        return Err("请选择要忘记 Key 的服务地址".into());
    }
    settings.llm_api_keys.remove(scope);
    if credential_scope(settings) == scope { settings.llm_api_key.clear(); }
    Ok(())
}

pub fn clear_secret(which: &str, scope: Option<&str>) -> Result<VoiceChatSettingsStatus, String> {
    let guard = SETTINGS_MUTATION.lock().map_err(|_| "语音配置繁忙")?;
    let path = settings_path()?;
    let mut settings = read_file(&path)?.unwrap_or_default();
    match which {
        "llm" => {
            let active = credential_scope(&settings);
            forget_llm_key(&mut settings, scope.unwrap_or(&active))?;
        },
        "tts" => {
            settings.tts_api_key.clear();
            if settings.tts_provider == "openai" {
                materialize_speech(&mut settings, volcengine_asr::read_legacy_config)?.retain_legacy_mimo();
                settings.custom_tts_credential = Some(Default::default());
            }
        }
        "search" => settings.llm_search_api_key.clear(),
        _ => return Err("unknown secret".to_string()),
    }
    write_file(&path, &settings)?;
    drop(guard);
    status()
}

pub fn llm_config() -> Result<LlmConfig, String> {
    let (settings, _) = load()?;
    Ok(LlmConfig {
        search: crate::web_search::Settings { mode: settings.llm_search_mode, api_key: settings.llm_search_api_key },
        web_search: settings.llm_web_search.unwrap_or(true),
        base_url: settings.llm_base_url,
        model: settings.llm_model,
        api_key: settings.llm_api_key,
    })
}

fn catalog_settings(settings: VoiceChatSettings, input: VoiceChatSettingsInput) -> Result<VoiceChatSettings, String> {
    let mut settings = merge_settings(settings, input);
    if settings.llm_provider == "custom" && settings.llm_base_url.trim().is_empty() {
        return Err("请先填写自定义接口地址".into());
    }
    apply_llm_defaults(&mut settings);
    if settings.llm_api_key.trim().is_empty() { return Err("请先保存或填写这家服务的 API Key".into()); }
    validate_model_endpoint(&settings.llm_base_url)?;
    Ok(settings)
}

fn validate_model_endpoint(base: &str) -> Result<(), String> {
    let url = reqwest::Url::parse(base).map_err(|_| "请填写完整的模型接口地址")?;
    let host = url.host_str().ok_or("模型服务地址缺少主机")?;
    let loopback = host == "localhost" || host.trim_matches(['[', ']']).parse::<std::net::IpAddr>()
        .is_ok_and(|ip| ip.is_loopback());
    if !(url.scheme() == "https" || (loopback && url.scheme() == "http")) || !url.username().is_empty()
        || url.password().is_some() || url.query().is_some() || url.fragment().is_some() {
        return Err("模型接口需要 HTTPS 或本机 HTTP 地址，地址中请勿携带凭据".into());
    }
    Ok(())
}

fn model_test_config(settings: VoiceChatSettings) -> LlmConfig {
    LlmConfig { api_key: settings.llm_api_key, model: settings.llm_model, base_url: settings.llm_base_url,
        web_search: false, search: Default::default() }
}

pub async fn test_model(input: VoiceChatSettingsInput) -> Result<(), String> {
    let cfg = model_test_config(catalog_settings(load()?.0, input)?);
    let client = model_issue::client(LLM, std::time::Duration::from_secs(20))?;
    probe_completion(&client, &cfg).await
}

fn completion_probe_request(client: &reqwest::Client, cfg: &LlmConfig) -> reqwest::RequestBuilder {
    let url = cfg.completions_url();
    let request = client.post(&url).json(&serde_json::json!({
        "model": cfg.model, "messages": [{"role":"user","content":"Reply OK."}],
        "stream": false, "max_tokens": 64
    }));
    crate::web_search::authorize(request, &url, &cfg.api_key)
}

async fn probe_completion(client: &reqwest::Client, cfg: &LlmConfig) -> Result<(), String> {
    let response = completion_probe_request(client, cfg).send().await
        .map_err(|e| model_issue::network(LLM,&e))?;
    let bytes = model_issue::read_response(LLM,response,64 * 1024).await?;
    let value: serde_json::Value = serde_json::from_slice(&bytes).map_err(|_| "[LLM] 服务未返回有效 JSON")?;
    model_issue::check_reply(LLM,&value)?;
    if value["choices"][0]["message"]["content"].as_str().unwrap_or("").trim().is_empty() {
        return Err("[LLM] 模型未返回可用文本".into());
    }
    Ok(())
}

fn catalog_model_ids(value: &serde_json::Value) -> Result<Vec<String>, String> {
    let rows = value["data"].as_array().filter(|_| value["has_more"] != true)
        .ok_or("模型目录不完整，请手动填写模型 ID")?;
    let mut ids = std::collections::BTreeSet::new();
    if rows.len() > 4096 { return Err("模型目录过大，请手动填写模型 ID".into()); }
    for row in rows {
        let Some(id) = row["id"].as_str().map(str::trim).filter(|id| !id.is_empty() && id.len() <= 200) else { continue };
        if ["available", "enabled", "active"].iter().any(|key| row[*key] == false) { continue; }
        let status = row["lifecycle"]["status"].as_str().or_else(|| row["lifecycle"].as_str()).or_else(|| row["status"].as_str()).unwrap_or("").to_ascii_lowercase();
        if ["retir", "shutdown", "disabled", "unavailable"].iter().any(|word| status.contains(word)) { continue; }
        if row["modalities"]["output_modalities"].as_array().is_some_and(|outputs| !outputs.iter().any(|o| o == "text")) { continue; }
        if ["seedance", "seedream", "embedding", "rerank", "tts", "asr"].iter().any(|word| id.to_ascii_lowercase().contains(word)) { continue; }
        ids.insert(id.to_owned());
    }
    Ok(ids.into_iter().collect())
}

/// Read-only: scope the credential exactly like save, but never persist the
/// draft or return secrets. Redirects are disabled by the shared network layer.
pub async fn list_models(input: VoiceChatSettingsInput) -> Result<Vec<String>, String> {
    let settings = catalog_settings(load()?.0, input)?;
    let client = model_issue::client(LLM, std::time::Duration::from_secs(15))?;
    let cfg = model_test_config(settings);
    let url = cfg.completions_url();
    let root = url.strip_suffix("/chat/completions").expect("completion route");
    let request = client.get(format!("{root}/models"));
    let response = crate::web_search::authorize(request, &url, &cfg.api_key)
        .send().await.map_err(|e| model_issue::network(LLM,&e))?;
    let bytes = model_issue::read_response(LLM,response,1024 * 1024).await?;
    let value = serde_json::from_slice(&bytes).map_err(|_| "模型目录格式不正确")?;
    model_issue::check_reply(LLM,&value)?;
    catalog_model_ids(&value)
}

/// News retrieval stays on the explicitly selected Doubao native search,
/// independently of the model used to edit the briefing. Never persist a
/// provider switch or reuse another provider's (or speech) credential.
pub fn news_search_config() -> Result<LlmConfig, String> {
    news_search_config_from(&load()?.0)
}

fn news_search_config_from(settings:&VoiceChatSettings)->Result<LlmConfig,String>{
    if settings.llm_web_search==Some(false){
        return Err("请在 API 配置启用联网检索，资讯搜索使用豆包".into());
    }
    let preset=llm_preset("doubao").expect("built-in Doubao preset");
    let active=settings.llm_provider=="doubao";
    let key=if active&&!settings.llm_api_key.trim().is_empty(){Some(&settings.llm_api_key)}
        else{settings.llm_api_keys.get("doubao")}
        .filter(|s|!s.trim().is_empty()).ok_or("请先在 API 配置保存豆包对话模型的 Key，用于资讯联网检索；语音合成 Key 不适用")?;
    Ok(LlmConfig{
        search: Default::default(),
        web_search:true,base_url:preset.base_url.into(),api_key:key.trim().into(),
        model:if active&&!settings.llm_model.trim().is_empty(){settings.llm_model.trim().into()}else{preset.model.into()},
    })
}

pub fn tts_config(speaker: &str, clone_speaker_id: &str, speed: f32) -> Result<DoubaoTtsConfig, String> {
    migrate_shared_speech_key()?;
    let (settings, _) = load()?;
    let (tts_key, _) = resolve_tts_key(&settings)?;
    tts_config_for(settings, &tts_key, speaker, clone_speaker_id, speed)
}

pub fn saved_tts_probe_config() -> Result<DoubaoTtsConfig, String> {
    migrate_shared_speech_key()?;
    let (settings, _) = load()?;
    let key = saved_tts_key(&settings)?
        .ok_or("当前合成服务没有已保存的 API Key")?;
    tts_config_for(settings, &key, "zh_female_vv_uranus_bigtts", "", 1.0)
}

fn tts_config_for(settings: VoiceChatSettings, tts_key: &str, speaker: &str, clone_speaker_id: &str, speed: f32) -> Result<DoubaoTtsConfig, String> {
    if tts_key.trim().is_empty() {
        if settings.tts_provider == "mimo" { return Err("请先配置小米 MiMo 语音 Key".into()); }
        if settings.tts_provider == "openai" && settings.custom_tts_credential.is_some() {
            return Err("请先保存此合成服务的 API Key".into());
        }
    }
    let mut cfg = DoubaoTtsConfig::new(
        tts_key,
        speaker,
        clone_speaker_id,
        speed,
        &settings.tts_resource_id,
    );
    cfg.tts_provider = settings.speech_provider().into();
    if buffered_speech(&settings.tts_provider) {
        let (url, model, voice) = settings.tts_route();
        custom_speech_endpoint(url)?;
        cfg.tts_base_url = url.into();
        cfg.tts_model = model.into();
        cfg.tts_voice = voice.trim().into();
    }
    Ok(cfg)
}

#[cfg(test)]
pub(crate) async fn mock_speech_response(
    content_type: &'static str,
    body: Vec<u8>,
) -> (String, tokio::task::JoinHandle<(String, Vec<u8>)>) {
    mock_model_response("200 OK",content_type,body).await
}

#[cfg(test)]
pub(crate) async fn mock_model_response(status: &'static str, content_type: &'static str, body: Vec<u8>)
    -> (String, tokio::task::JoinHandle<(String, Vec<u8>)>) {
    use tokio::io::{AsyncBufReadExt, AsyncReadExt, AsyncWriteExt, BufReader};

    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let origin = format!("http://{}", listener.local_addr().unwrap());
    let worker = tokio::spawn(async move {
        let (stream, _) = listener.accept().await.unwrap();
        let mut reader = BufReader::new(stream);
        let mut headers = String::new();
        loop {
            let mut line = String::new();
            assert!(reader.read_line(&mut line).await.unwrap() > 0);
            if line == "\r\n" {
                break;
            }
            headers.push_str(&line);
        }
        let length = headers.lines()
            .filter_map(|line| line.split_once(':'))
            .find(|(name, _)| name.eq_ignore_ascii_case("content-length"))
            .map(|(_, value)| value.trim().parse().unwrap())
            .unwrap();
        let mut request = vec![0; length];
        reader.read_exact(&mut request).await.unwrap();
        let stream = reader.get_mut();
        let response = format!(
            "HTTP/1.1 {status}\r\nContent-Type: {content_type}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",
            body.len(),
        );
        stream.write_all(response.as_bytes()).await.unwrap();
        stream.write_all(&body).await.unwrap();
        (headers, request)
    });
    (origin, worker)
}

#[cfg(test)]
mod tests {
    #[test]
    fn custom_speech_endpoint_preserves_prefix_and_selects_protocol() {
        for (address, root, chat) in [
            ("https://api.xiaomimimo.com/v1/", "https://api.xiaomimimo.com/v1", true),
            ("https://token-plan-cn.xiaomimimo.com", "https://token-plan-cn.xiaomimimo.com/v1", true),
            ("https://token-plan-ams.xiaomimimo.com/v1", "https://token-plan-ams.xiaomimimo.com/v1", true),
            ("https://example.test/openai/v1", "https://example.test/openai/v1", false),
            ("https://example.test/openai/v1/chat/completions", "https://example.test/openai/v1", true),
            ("https://api.xiaomimimo.com/v1/audio/speech", "https://api.xiaomimimo.com/v1", false),
        ] {
            let endpoint = super::custom_speech_endpoint(address).unwrap();
            assert_eq!(endpoint.api_root, root);
            assert_eq!(endpoint.uses_chat_completions, chat);
        }
        assert!(super::custom_speech_endpoint("https://example.test/v1?key=fixture").is_err());
        assert!(super::custom_speech_endpoint("http://example.test/v1").is_err());
    }

    #[test]
    fn mimo_china_token_plan_never_reuses_the_pay_as_you_go_key() {
        let initial = VoiceChatSettings { llm_provider: "mimo".into(), llm_api_key: "payg-fixture".into(), ..Default::default() };
        let token = merge_settings(initial, VoiceChatSettingsInput {
            llm_provider: Some("mimo-token-plan-cn".into()),
            ..Default::default()
        });
        assert!(token.llm_api_key.is_empty());
        let supplied = VoiceChatSettingsInput {
            llm_api_key: Some("plan-fixture".into()),
            llm_base_url: Some("https://unrelated.example".into()),
            ..Default::default()
        };
        let token = catalog_settings(token, supplied).unwrap();
        assert_eq!(token.llm_base_url, "https://token-plan-cn.xiaomimimo.com/v1");
        assert_eq!(token.llm_api_key, "plan-fixture");
        let payg = catalog_settings(token, VoiceChatSettingsInput { llm_provider: Some("mimo".into()), ..Default::default() }).unwrap();
        assert_eq!(payg.llm_base_url, "https://api.xiaomimimo.com/v1");
        assert_eq!(payg.llm_api_key, "payg-fixture");
        assert_eq!(payg.llm_api_keys["mimo-token-plan-cn"], "plan-fixture");
    }

    #[test]
    fn independent_search_settings_remain_separate_when_chat_provider_changes() {
        use crate::web_search::Mode;
        let legacy: VoiceChatSettings = serde_json::from_value(serde_json::json!({
            "llmProvider":"deepseek", "llmApiKey":"chat-fixture"
        })).unwrap();
        assert_eq!(legacy.llm_search_mode, Mode::Auto);
        assert!(legacy.llm_search_api_key.is_empty());
        let saved = merge_settings(legacy, VoiceChatSettingsInput {
            llm_search_mode: Some(Mode::AutoWithDeepSeek),
            llm_search_api_key: Some(" search-fixture ".into()),
            ..Default::default()
        });
        let switched = merge_settings(saved, VoiceChatSettingsInput {
            llm_provider: Some("doubao".into()),
            llm_api_key: Some("doubao-fixture".into()),
            ..Default::default()
        });
        assert_eq!(switched.llm_search_mode, Mode::AutoWithDeepSeek);
        assert_eq!(switched.llm_search_api_key, "search-fixture");
        assert_eq!(switched.llm_api_key, "doubao-fixture");
        assert_eq!(switched.llm_api_keys["deepseek"], "chat-fixture");
    }

    #[test]
    fn catalog_uses_only_target_provider_or_exact_endpoint_key_and_never_mutates_input() {
        let mut settings = VoiceChatSettings { llm_provider: "deepseek".into(), llm_api_key: "ds-secret".into(), ..Default::default() };
        settings.llm_api_keys.insert("doubao".into(), "ark-secret".into());
        settings.llm_api_keys.insert("custom:https://one.invalid/v1".into(), "custom-secret".into());
        let selected = catalog_settings(settings.clone(), VoiceChatSettingsInput { llm_provider: Some("doubao".into()), ..Default::default() }).unwrap();
        assert_eq!(selected.llm_api_key, "ark-secret");
        assert_eq!(selected.llm_base_url, "https://ark.cn-beijing.volces.com/api/v3");
        assert_eq!(selected.llm_model, "doubao-seed-2-1-lite-260915");
        let custom = |url: &str| VoiceChatSettingsInput { llm_provider: Some("custom".into()), llm_base_url: Some(url.into()), ..Default::default() };
        assert!(catalog_settings(settings.clone(), custom("https://two.invalid/v1")).is_err());
        assert_eq!(catalog_settings(settings.clone(), custom("https://one.invalid/v1/")).unwrap().llm_api_key, "custom-secret");
        assert_eq!(settings.llm_api_key, "ds-secret");
    }

    fn speech_input(provider: &str, key: Option<&str>, model: &str) -> SpeechSettingsInput {
        serde_json::from_value(serde_json::json!({
            "provider": provider, "apiKey": key, "baseUrl": "https://speech.invalid/v1",
            "model": model, "ttsModel": format!("{model}-tts"), "ttsVoice": "demo-voice"
        })).unwrap()
    }

    fn mimo_input(key: Option<&str>) -> SpeechSettingsInput {
        serde_json::from_value(serde_json::json!({"provider":"mimo","apiKey":key})).unwrap()
    }

    #[test]
    fn mimo_migration_requires_both_exact_routes_models_voice_and_matching_key() {
        for (asr_url, asr_model, tts_url, tts_model, voice, tts_key, promote) in [
            (MIMO_ROOT, MIMO_ASR, MIMO_ROOT, MIMO_TTS, "", "", true),
            ("https://api.xiaomimimo.com/v1/chat/completions", MIMO_ASR, MIMO_ROOT, MIMO_TTS, MIMO_VOICE, "test-key", true),
            ("https://token-plan-cn.xiaomimimo.com/v1", MIMO_ASR, MIMO_ROOT, MIMO_TTS, "", "", false),
            ("https://proxy.invalid/v1/chat/completions", MIMO_ASR, MIMO_ROOT, MIMO_TTS, "", "", false),
            (MIMO_ROOT, MIMO_ASR, "https://api.xiaomimimo.com.proxy.invalid/v1", MIMO_TTS, "", "", false),
            ("https://api.xiaomimimo.com:444/v1", MIMO_ASR, MIMO_ROOT, MIMO_TTS, "", "", false),
            (MIMO_ROOT, MIMO_ASR, "https://api.xiaomimimo.com/v1/audio/speech", MIMO_TTS, "", "", false),
            (MIMO_ROOT, "other", MIMO_ROOT, MIMO_TTS, "", "", false),
            (MIMO_ROOT, MIMO_ASR, MIMO_ROOT, "other", "", "", false),
            (MIMO_ROOT, MIMO_ASR, MIMO_ROOT, MIMO_TTS, "茉莉", "", false),
            (MIMO_ROOT, MIMO_ASR, MIMO_ROOT, MIMO_TTS, "", "separate-test-key", false),
        ] {
            let mut old: VoiceChatSettings = serde_json::from_value(serde_json::json!({
                "ttsProvider":"openai","ttsBaseUrl":tts_url,"ttsModel":tts_model,"ttsVoice":voice,
                "ttsApiKey":tts_key,"speechProfiles":{"version":1,"resourceId":"","provider":"openai",
                    "baseUrl":asr_url,"model":asr_model,"apiKey":"test-key"}
            })).unwrap();
            upgrade_legacy_mimo(&mut old, || panic!()).unwrap();
            let speech = old.speech_profiles.unwrap();
            assert_eq!(old.tts_provider == "mimo", promote);
            let reusable = official_mimo_endpoint(asr_url) && official_mimo_endpoint(tts_url)
                && matches!(tts_key, "" | "test-key");
            assert_eq!(speech.key_for_scope("mimo", MIMO_ROOT).is_some(), reusable);
            assert_eq!(speech.key_for_scope("openai", asr_url).as_deref(), Some("test-key"));
        }
    }

    #[test]
    fn mimo_explicit_selection_reuses_only_unambiguous_official_speech_keys() {
        for conflict in [false, true] {
            let dir = tempfile::tempdir().unwrap();
            let path = dir.path().join(FILE_NAME);
            let old: VoiceChatSettings = serde_json::from_value(serde_json::json!({
                "ttsProvider":"openai", "ttsBaseUrl":MIMO_ROOT, "ttsModel":MIMO_TTS, "ttsVoice":"茉莉",
                "ttsApiKey":if conflict { "tts-fixture" } else { "" },
                "speechProfiles":{"provider":"openai","version":1,"apiKey":"asr-fixture","resourceId":"",
                    "model":MIMO_ASR,"baseUrl":MIMO_ROOT}
            })).unwrap();
            write_file(&path, &old).unwrap();
            migrate_shared_speech_key_at(&path, |_,_,_| panic!()).unwrap();
            let reopened = read_file(&path).unwrap().unwrap();
            assert_eq!(reopened.tts_provider, "openai");
            assert_eq!(saved_tts_key(&reopened).unwrap().as_deref(),
                Some(if conflict { "tts-fixture" } else { "asr-fixture" }));
            assert_eq!(reopened.speech_profiles.unwrap().has_mimo_key(), !conflict);
            let save = |input| save_speech_at(&path, input, || panic!(), write_file);
            assert_eq!(save(mimo_input(None)).is_ok(), !conflict);
            if conflict { save(mimo_input(Some("independent-fixture"))).unwrap(); }
            let mimo = read_file(&path).unwrap().unwrap();
            assert_eq!(mimo.tts_voice, "茉莉");
            assert_eq!(mimo.tts_route(), (MIMO_ROOT, MIMO_TTS, MIMO_VOICE));
            assert_eq!(saved_tts_key(&mimo).unwrap().as_deref(),
                Some(if conflict { "independent-fixture" } else { "asr-fixture" }));
        }
    }

    #[cfg(target_os = "macos")]
    #[tokio::test]
    async fn mimo_cold_restart_probes_and_runtime_over_loopback_tls() {
        const NAME: &str = "voice_chat_settings::tests::mimo_cold_restart_probes_and_runtime_over_loopback_tls";
        let Some(directory) = std::env::var_os("PET_MIMO_LOOPBACK_DIR") else {
            // Keep the wire assertions with the executable when concurrent builds use this checkout.
            let fixture = tempfile::tempdir().unwrap();
            let script = fixture.path().join("mimo_speech_loopback.py");
            fs::write(&script, include_bytes!("../test-fixtures/mimo_speech_loopback.py")).unwrap();
            fs::write(fixture.path().join("speech-probe.s16le"), include_bytes!("../test-fixtures/speech-probe.s16le")).unwrap();
            let result = std::process::Command::new("python3").arg(script)
                .arg(std::env::current_exe().unwrap()).arg(NAME).output().unwrap();
            assert!(result.status.success(), "{} {}", String::from_utf8_lossy(&result.stdout), String::from_utf8_lossy(&result.stderr));
            return;
        };
        let dir = PathBuf::from(directory).canonicalize().unwrap();
        assert_eq!(dir.parent().unwrap(), std::env::temp_dir().canonicalize().unwrap());
        assert!(dir.file_name().unwrap().to_string_lossy().starts_with("pet-mimo-loopback-"));
        let port: u16 = std::env::var("PET_MIMO_PROXY_PORT").unwrap().parse().unwrap();
        configure_storage_dir(dir.clone()).unwrap();
        volcengine_asr::configure_storage_dir(dir.clone()).unwrap();
        crate::llm_network::configure_storage_dir(dir.clone()).unwrap();
        crate::llm_network::save_llm_network_settings(serde_json::from_value(serde_json::json!({
            "mode":"manual", "proxyUrl":format!("http://127.0.0.1:{port}"), "caFile":dir.join("ca.pem")
        })).unwrap()).unwrap();
        match std::env::var("PET_MIMO_STAGE").unwrap().as_str() {
            "legacy" => {
                let mut input = speech_input("openai", Some("offline-mimo-fixture"), MIMO_ASR);
                input.asr.base_url = Some(MIMO_ROOT.into());
                volcengine_asr::save_settings(input.asr).unwrap();
                let old = VoiceChatSettings {
                    tts_provider: "openai".into(), tts_base_url: MIMO_ROOT.into(),
                    tts_model: MIMO_TTS.into(), ..Default::default()
                };
                write_file(&dir.join(FILE_NAME), &old).unwrap();
            }
            "save" => {
                let restored = volcengine_asr::settings_status().unwrap();
                assert!(restored.saved_mimo && restored.saved_custom.is_some());
                assert_eq!(restored.saved_provider, "mimo");
                let saved = save_speech(mimo_input(None)).unwrap();
                assert_eq!(saved.saved_provider, "mimo");
            }
            "edit" => {
                let mut input = mimo_input(None);
                input.asr.model = Some("  edited-asr  ".into());
                input.tts_model = Some(" edited-tts ".into());
                input.tts_voice = Some(" 茉莉 ".into());
                save_speech(input).unwrap();
                save_speech(speech_input("openai", Some("other-fixture"), "other-asr")).unwrap();
            }
            "restore" => {
                let asr = volcengine_asr::settings_status().unwrap();
                let tts = status().unwrap();
                assert_eq!((asr.provider.as_str(), asr.model.as_str()), ("openai", "other-asr"));
                assert_eq!(asr.mimo_asr_model, "edited-asr");
                assert_eq!((tts.mimo_tts_model.as_str(), tts.mimo_tts_voice.as_str()), ("edited-tts", "茉莉"));
                assert_eq!((tts.tts_model.as_str(), tts.tts_voice.as_str()), ("other-asr-tts", "demo-voice"));
                let receipt = save_speech(mimo_input(None)).unwrap();
                assert_eq!(receipt.model, "edited-asr");
                assert_eq!(status().unwrap().custom_tts_voice, "demo-voice");
            }
            "persona" => crate::persona_tts::tests::verify_appearance_routes(&dir).await,
            "persona-restart" => {
                crate::component_voice::configure_preparation(dir.join("prepared-voice.json"));
                assert_eq!(crate::component_voice::preparation_config("").unwrap().tts_voice, "pet-specific");
                assert!(!crate::component_voice::ready("test-board"));
            }
            "custom-persona" => {
                crate::persona_tts::tests::verify_custom_appearance_routes().await;
                save_speech(mimo_input(None)).unwrap();
            }
            "reset" => {
                let mut input = mimo_input(None);
                input.asr.model = Some("  ".into());
                input.tts_voice = Some(String::new());
                let receipt = save_speech(input).unwrap();
                assert_eq!(receipt.mimo_asr_model, MIMO_ASR);
                let tts = status().unwrap();
                assert_eq!((tts.tts_model.as_str(), tts.tts_voice.as_str()), ("edited-tts", MIMO_VOICE));
                let mut input = mimo_input(None);
                input.tts_model = Some(" ".into());
                save_speech(input).unwrap();
                assert_eq!(status().unwrap().tts_model, MIMO_TTS);
                let mut design = mimo_input(None);
                design.tts_model = Some("mimo-v2.5-tts-voicedesign".into());
                save_speech(design).unwrap();
                let error = crate::test_device_tts_settings().await.unwrap_err();
                assert!(error.contains("描述"));
            }
            "env-default" => {
                let status = volcengine_asr::settings_status().unwrap();
                assert_eq!(status.credential_source, "environment");
                assert_eq!(status.model, MIMO_ASR);
                assert_eq!(status.mimo_asr_model, "edited-asr");
            }
            "probe" => {
                assert_eq!(status().unwrap().tts_provider, "mimo");
                assert_eq!(crate::test_device_asr_settings().await.unwrap().provider, "mimo");
                crate::test_device_tts_settings().await.unwrap();
            }
            "runtime" | "env-runtime" => {
                use volcengine_asr::{StreamingSpeechEvent, StreamingSpeechRecognizer};
                let (send, mut recv) = tokio::sync::mpsc::unbounded_channel();
                let asr = StreamingSpeechRecognizer::start(move |event| { let _ = send.send(event); }).unwrap();
                asr.push_pcm(include_bytes!("../test-fixtures/speech-probe.s16le")).unwrap();
                asr.finish().unwrap();
                let final_text = tokio::time::timeout(std::time::Duration::from_secs(12), async {
                    loop {
                        match recv.recv().await.unwrap() {
                            StreamingSpeechEvent::Final { text, .. } => break text,
                            StreamingSpeechEvent::Error(e) => panic!("{e}"),
                            _ => (),
                        }
                    }
                }).await.unwrap();
                assert_eq!(final_text, "Speech test.");
                if std::env::var("PET_MIMO_STAGE").unwrap() == "env-runtime" { return; }
                let voice: crate::realtime_chat::VoiceSpec = serde_json::from_value(serde_json::json!({
                    "ttsOverrides":{"mimo":{"model":"edited-tts","voice":"runtime-pet"}}
                })).unwrap();
                let mut tts = crate::doubao_tts::DoubaoTtsClient::new(voice.tts_config().unwrap());
                let mut pcm = Vec::new();
                assert_eq!(tts.synthesize("Speech test.", |chunk| pcm.extend_from_slice(chunk)).await.unwrap(), 400);
                assert_eq!(pcm, [0x34, 0x12].repeat(200));
                let preview = serde_json::from_value(serde_json::json!({"text":"试听","voice":{"volume":1.0}})).unwrap();
                assert_eq!(crate::realtime_chat::preview_pcm(preview).await.unwrap(), pcm);
            }
            _ => panic!("unexpected fixture stage"),
        }
    }

    #[test]
    fn mimo_three_profiles_survive_encrypted_reopen_and_reject_foreign_credential_reuse() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join(FILE_NAME);
        let save = |input| save_speech_at(&path, input, || Ok(None), write_file);
        save(speech_input("volcengine", Some("volc-fixture"), "")).unwrap();
        save(speech_input("openai", Some("private-fixture"), "private-asr")).unwrap();
        let before = fs::read(&path).unwrap();
        assert!(save(mimo_input(Some("••••"))).is_err());
        let mut wrong = mimo_input(Some("private-fixture"));
        wrong.asr.base_url = Some("https://speech.invalid/v1".into());
        assert!(save(wrong).is_err());
        assert_eq!(before, fs::read(&path).unwrap());
        let receipt = save(mimo_input(Some("mimo-fixture"))).unwrap();
        assert!(receipt.saved_mimo && receipt.saved_volcengine && receipt.saved_custom.is_some());
        for (input, key) in [(speech_input("volcengine", None, ""), "volc-fixture"),
            (speech_input("openai", None, "private-asr"), "private-fixture"), (mimo_input(None), "mimo-fixture")] {
            save(input).unwrap();
            let reopened = read_file(&path).unwrap().unwrap();
            assert_eq!(reopened.tts_model, "private-asr-tts");
            assert_eq!(reopened.tts_voice, "demo-voice");
            assert_eq!(saved_tts_key(&reopened).unwrap().as_deref(), Some(key));
        }
        let reopened = read_file(&path).unwrap().unwrap();
        assert_eq!(reopened.tts_route(), (MIMO_ROOT, MIMO_TTS, MIMO_VOICE));
        let public = serde_json::to_string(&receipt).unwrap();
        let disk = fs::read_to_string(&path).unwrap();
        for secret in ["mimo-fixture", "private-fixture", "volc-fixture"] {
            assert!(!public.contains(secret) && !disk.contains(secret));
        }
    }

    #[test]
    fn atomic_speech_save_migrates_profiles_preserves_other_fields_and_returns_public_status() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join(FILE_NAME);
        let mut old: VoiceChatSettings = serde_json::from_value(serde_json::json!({
            "ttsProvider":"openai", "ttsBaseUrl":"https://legacy.invalid/v1",
            "ttsApiKey":"legacy-tts-fixture", "llmApiKey":"llm-fixture", "llmProvider":"deepseek"
        })).unwrap();
        write_file(&path, &old).unwrap();
        let legacy = serde_json::from_value(serde_json::json!({
            "version":1,"provider":"openai","baseUrl":"https://speech.invalid/v1","model":"old-asr",
            "resourceId":"","apiKey":"asr-fixture", "inactive":{
                "version":1,"provider":"volcengine","resourceId":"","apiKey":"builtin-fixture"}
        })).unwrap();
        let before = fs::read(&path).unwrap();
        assert!(save_speech_at(&path, speech_input("openai", None, "unused"),
            || Err("legacy store unavailable".into()), |_, _| panic!("no write after failed import")).is_err());
        assert_eq!(fs::read(&path).unwrap(), before);
        let status = save_speech_at(&path, speech_input("openai", Some("••••"), "new-asr"),
            || Ok(Some(legacy)), write_file).unwrap();
        let public = serde_json::to_string(&status).unwrap();
        assert!(status.saved_volcengine && status.configured);
        old = read_file(&path).unwrap().unwrap();
        assert_eq!(old.llm_api_key, "llm-fixture");
        assert!(old.tts_api_key.is_empty());
        assert_eq!(old.tts_model, "new-asr-tts");
        assert_eq!(old.tts_voice, "demo-voice");
        assert_eq!(saved_tts_key(&old).unwrap().as_deref(), Some("asr-fixture"));
        let speech = old.speech_profiles.unwrap();
        assert_eq!(speech.key_for_scope("openai", "https://legacy.invalid/v1").as_deref(), Some("legacy-tts-fixture"));
        assert!(speech.key_for_scope("openai", "https://other.invalid/v1").is_none());
        for key in ["llm-fixture", "asr-fixture", "builtin-fixture", "legacy-tts-fixture"] {
            assert!(!public.contains(key));
            assert!(!fs::read(&path).unwrap().windows(key.len()).any(|bytes| bytes == key.as_bytes()));
        }
        // Legacy ASR-only IPC still edits the authoritative record without changing synthesis.
        save_asr_at(Some(&path), speech_input("volcengine", None, "unused").asr).unwrap();
        let after = read_file(&path).unwrap().unwrap();
        assert_eq!(after.tts_provider, "openai");
        assert_eq!(saved_tts_key(&after).unwrap().as_deref(), Some("asr-fixture"));
        save_speech_at(&path, speech_input("volcengine", None, "unused"),
            || panic!("committed profiles must supersede the old store"), write_file).unwrap();
        assert_eq!(saved_tts_key(&read_file(&path).unwrap().unwrap()).unwrap().as_deref(), Some("builtin-fixture"));
        // The old combined LLM/TTS command changes its own fields and leaves ASR active.
        save_voice_at(&path, VoiceChatSettingsInput {
            llm_model: Some("new-chat".into()), tts_provider: Some("openai".into()),
            tts_base_url: Some("https://legacy.invalid/v1".into()), tts_model: Some("legacy-tts".into()),
            ..Default::default()
        }).unwrap();
        let after = read_file(&path).unwrap().unwrap();
        assert_eq!(after.llm_model, "new-chat");
        assert_eq!(after.tts_model, "legacy-tts");
        assert_eq!(saved_tts_key(&after).unwrap().as_deref(), Some("legacy-tts-fixture"));
        assert_eq!(serde_json::to_value(after.speech_profiles.unwrap()).unwrap()["provider"], "volcengine");
    }

    #[cfg(target_os = "macos")]
    #[test]
    fn atomic_speech_legacy_commands_and_cold_restart() {
        const NAME: &str = "voice_chat_settings::tests::atomic_speech_legacy_commands_and_cold_restart";
        let Some(dir) = std::env::var_os("PET_SPEECH_ATOMIC_TEST_DIR") else {
            let dir = tempfile::Builder::new().prefix("pet-speech-atomic-").tempdir().unwrap();
            for stage in ["save", "restart"] {
                let child = std::process::Command::new(std::env::current_exe().unwrap())
                    .args(["--exact", NAME]).env("PET_SPEECH_ATOMIC_TEST_DIR", dir.path())
                    .env("PET_SPEECH_ATOMIC_TEST_STAGE", stage)
                    .env_remove("VOLCENGINE_ASR_API_KEY").env_remove("HACHIMO_ASR_PROVIDER").output().unwrap();
                assert!(child.status.success(), "{}", String::from_utf8_lossy(&child.stdout));
            }
            return;
        };
        let dir = PathBuf::from(dir).canonicalize().unwrap();
        assert!(dir.starts_with(std::env::temp_dir().canonicalize().unwrap()));
        assert!(dir.file_name().unwrap().to_string_lossy().starts_with("pet-speech-atomic-"));
        configure_storage_dir(dir.clone()).unwrap();
        volcengine_asr::configure_storage_dir(dir).unwrap();
        if std::env::var("PET_SPEECH_ATOMIC_TEST_STAGE").unwrap() == "save" {
            volcengine_asr::save_settings(speech_input("openai", Some("cold-asr-fixture"), "old-model").asr).unwrap();
            save(VoiceChatSettingsInput { llm_provider: Some("deepseek".into()), llm_api_key: Some("cold-llm-fixture".into()),
                tts_provider: Some("volcengine".into()), tts_api_key: Some("cold-builtin-fixture".into()),
                ..Default::default() }).unwrap();
            save_speech(speech_input("openai", None, "cold-model")).unwrap();
        }
        let asr = volcengine_asr::settings_status().unwrap();
        let chat = status().unwrap();
        assert_eq!(asr.model, "cold-model");
        assert!(asr.saved_volcengine && chat.llm_configured && chat.tts_configured);
        assert_eq!(chat.tts_model, "cold-model-tts");
        assert_eq!(saved_tts_probe_config().unwrap().api_key, "cold-asr-fixture");
        if std::env::var("PET_SPEECH_ATOMIC_TEST_STAGE").unwrap() == "restart" {
            // Pre-existing IPC updates the unified source after restart.
            volcengine_asr::save_settings(speech_input("volcengine", None, "").asr).unwrap();
            save(VoiceChatSettingsInput {tts_provider: Some("volcengine".into()), ..Default::default()}).unwrap();
            assert_eq!(saved_tts_probe_config().unwrap().api_key, "cold-builtin-fixture");
        }
    }

    #[test]
    fn atomic_speech_failure_preserves_ciphertext_and_a_waiting_writer_wins() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join(FILE_NAME);
        let save = |input| save_speech_at(&path, input, || Ok(None), write_file);
        save(speech_input("openai", Some("initial-fixture"), "original")).unwrap();
        let original = fs::read(&path).unwrap();
        let mut invalid = speech_input("openai", None, "bad");
        invalid.tts_model = None;
        assert!(save(invalid).is_err());
        // Actual encrypted writer rejects an oversized candidate before replacing the file.
        let mut oversized = speech_input("openai", None, "oversized");
        oversized.tts_voice = Some("v".repeat(1024 * 1024));
        assert!(save(oversized).is_err());
        assert_eq!(fs::read(&path).unwrap(), original);
        std::thread::scope(|threads| {
            let (ready, pending) = std::sync::mpsc::channel();
            let (go, queued) = std::sync::mpsc::channel();
            let path = &path;
            let failing = threads.spawn(move || save_speech_at(path, speech_input("openai", Some("failed-fixture"), "failed"),
                || panic!(), |_, _| {
                    ready.send(()).unwrap();
                    queued.recv().unwrap();
                    Err("fixture commit error".into())
                }));
            pending.recv().unwrap();
            let newer = threads.spawn(move || {
                go.send(()).unwrap();
                save_speech_at(path, speech_input("openai", Some("newer-fixture"), "newer"), || panic!(), write_file)
            });
            assert!(failing.join().unwrap().is_err());
            assert_eq!(newer.join().unwrap().unwrap().model, "newer");
        });
        let final_settings = read_file(&path).unwrap().unwrap();
        assert_eq!(final_settings.tts_model, "newer-tts");
        assert_eq!(saved_tts_key(&final_settings).unwrap().as_deref(), Some("newer-fixture"));
    }

    #[test]
    fn stored_display_masks_do_not_restore_as_credentials() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("voice-chat-settings.json");
        let legacy = serde_json::json!({"llmProvider": "deepseek", "llmApiKey": "••••",
            "ttsApiKey": "old…mask", "llmSearchApiKey": "••••",
            "llmApiKeys": {"deepseek":"old…mask", "doubao":"fixture-secret"}});
        crate::secure_config::write(&path, "voice-chat", legacy.to_string().as_bytes()).unwrap();
        let restored = read_file(&path).unwrap().unwrap();
        assert!(restored.llm_api_key.is_empty() && restored.llm_search_api_key.is_empty() && restored.tts_api_key.is_empty());
        assert!(!restored.llm_api_keys.contains_key("deepseek"));
        assert_eq!(restored.llm_api_keys["doubao"], "fixture-secret");
    }

    #[test]
    fn custom_speech_auth_matches_selected_protocol_and_token_plan() {
        let client = reqwest::Client::new();
        for (url, token_plan) in [
            ("https://token-plan-cn.xiaomimimo.com", true),
            ("https://api.xiaomimimo.com", false),
            ("https://proxy.invalid/v1/chat/completions", false),
            ("https://compatible.invalid/v1/audio/transcriptions", false),
        ] {
            let endpoint = custom_speech_endpoint(url).unwrap();
            let request = endpoint.authorize(client.post(url), " fixture ").build().unwrap();
            assert_eq!(request.headers().get("api-key").map(|v| v.to_str().unwrap()), token_plan.then_some("fixture"));
            assert_eq!(request.headers().get("authorization").map(|v| v.to_str().unwrap()), (!token_plan).then_some("Bearer fixture"));
            assert!(endpoint.authorize(client.post(url), "").build().unwrap().headers().is_empty());
        }
    }

    #[test]
    fn settings_probe_uses_each_presets_own_route_model_and_auth_header() {
        let client = reqwest::Client::new();
        for (provider, url) in [
            ("doubao","https://ark.cn-beijing.volces.com/api/v3/chat/completions"),
            ("deepseek","https://api.deepseek.com/v1/chat/completions"),
            ("mimo","https://api.xiaomimimo.com/v1/chat/completions"),
            ("mimo-token-plan-cn","https://token-plan-cn.xiaomimimo.com/v1/chat/completions"),
            ("custom","https://fixture.invalid/private/v1/chat/completions"),
        ] {
            let input = VoiceChatSettingsInput { llm_provider: Some(provider.into()), llm_base_url: Some(url.into()),
                llm_model: Some("fixture-model".into()), llm_api_key: Some("fixture-secret".into()), ..Default::default() };
            let cfg = model_test_config(catalog_settings(VoiceChatSettings::default(), input).unwrap());
            let request = completion_probe_request(&client, &cfg).build().unwrap();
            assert_eq!(request.url().as_str(), url);
            if provider == "mimo-token-plan-cn" {
                assert_eq!(request.headers()["api-key"], "fixture-secret");
                assert!(!request.headers().contains_key("authorization"));
            } else {
                assert_eq!(request.headers()["authorization"], "Bearer fixture-secret");
                assert!(!request.headers().contains_key("api-key"));
            }
            let body: serde_json::Value = serde_json::from_slice(request.body().unwrap().as_bytes().unwrap()).unwrap();
            assert_eq!(body["model"], "fixture-model");
            assert_eq!(body["stream"], false);
        }
    }

    #[tokio::test]
    async fn settings_probe_requires_text_and_sends_only_the_selected_model() {
        let client = reqwest::Client::builder().no_proxy().build().unwrap();
        for (reply, ok) in [(r#"{"choices":[{"message":{"content":"OK"}}]}"#, true),
                            (r#"{"choices":[]}"#, false), ("not-json", false)] {
            let (origin, server) = mock_speech_response("application/json", reply.as_bytes().to_vec()).await;
            let cfg = model_test_config(catalog_settings(VoiceChatSettings::default(), VoiceChatSettingsInput {
                llm_provider: Some("custom".into()), llm_base_url: Some(format!("{origin}/prefix/v1/chat/completions")),
                llm_model: Some("private-model".into()), llm_api_key: Some("fixture-only".into()), ..Default::default()
            }).unwrap());
            assert_eq!(probe_completion(&client, &cfg).await.is_ok(), ok);
            let (headers, bytes) = server.await.unwrap();
            assert!(headers.starts_with("POST /prefix/v1/chat/completions "));
            assert!(headers.to_lowercase().contains("authorization: bearer fixture-only"));
            let body: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
            assert_eq!(body["model"], "private-model");
            assert_eq!(body["messages"].as_array().unwrap().len(), 1);
            assert!(body.get("tools").is_none());
        }
    }

    #[tokio::test]
    async fn model_issue_llm_probe_distinguishes_http_business_and_empty_success() {
        use super::*;
        let client=reqwest::Client::builder().no_proxy().build().unwrap();
        for (status,reply,expected) in [
            ("200 OK",r#"{"choices":[],"message":"private-key"}"#,"[LLM] 模型未返回可用文本"),
            ("429 Too Many Requests",r#"{"message":"private-key https://secret.invalid"}"#,"[LLM] HTTP 429"),
            ("200 OK",r#"{"error":{"code":"model_not_found","message":"private-key https://secret.invalid"}}"#,"[LLM] code=model_not_found"),
            ("200 OK",r#"{"error":{"code":"https://secret.invalid","message":"private-key"}}"#,"[LLM] code=unknown"),
        ] {
            let (origin,server)=mock_model_response(status,"application/json",reply.as_bytes().to_vec()).await;
            let cfg=LlmConfig{base_url:origin,model:"private-model".into(),api_key:"private-key".into(),web_search:false,search:Default::default()};
            let error=probe_completion(&client,&cfg).await.unwrap_err();
            assert!(error.starts_with(expected)); assert!(!error.contains("private") && !error.contains("http"));
            server.await.unwrap();
        }
    }

    #[test]
    fn model_issue_storage_write_preserves_original_and_hides_private_path() {
        use super::*;
        let dir=tempfile::tempdir().unwrap();
        let parent=dir.path().join("private-username-key");
        std::fs::write(&parent,b"existing").unwrap();
        let error=write_file(&parent.join("settings"),&VoiceChatSettings::default()).unwrap_err();
        assert!(error.starts_with("[CREDENTIALS]"));
        assert!(!error.contains("private"));
        assert_eq!(std::fs::read(parent).unwrap(),b"existing");
    }

    #[test]
    fn model_catalog_returns_only_active_text_ids_and_checks_completeness() {
        let ids = catalog_model_ids(&serde_json::json!({"data":[
            {"id":"doubao-seed-2-1-lite-260915","modalities":{"output_modalities":["text"]}},
            {"id":"doubao-seed-2-1-pro-260915"}, {"id":"retired","status":"Retiring"},
            {"id":"gone","status":"Shutdown"}, {"id":"off","enabled":false},
            {"id":"audio","modalities":{"output_modalities":["audio"]}},
            {"id":"doubao-seedance-2-0-fast-test"}, {"id":"doubao-seed-2-1-pro-260915"}
        ]})).unwrap();
        assert_eq!(ids, ["doubao-seed-2-1-lite-260915", "doubao-seed-2-1-pro-260915"]);
        assert!(catalog_model_ids(&serde_json::json!({"data":[], "has_more":true})).is_err());
    }

    #[test]
    fn news_search_uses_only_doubao_credential_and_keeps_chat_settings(){
        let mut settings=VoiceChatSettings{llm_provider:"deepseek".into(),llm_model:"deepseek-flash".into(),llm_api_key:"other-provider-key".into(),tts_api_key:"speech-only-key".into(),..Default::default()};
        assert!(news_search_config_from(&settings).is_err());
        settings.llm_api_keys.insert("doubao".into(),"doubao-test-key".into());
        let cfg=news_search_config_from(&settings).unwrap();
        assert_eq!(cfg.api_key,"doubao-test-key");
        assert_eq!(cfg.base_url,llm_preset("doubao").unwrap().base_url);
        assert_eq!(cfg.model,llm_preset("doubao").unwrap().model);
        assert_eq!(settings.llm_provider,"deepseek");
        assert_eq!(settings.llm_api_key,"other-provider-key");
        settings.llm_provider="doubao".into();settings.llm_model="my-doubao-endpoint".into();settings.llm_api_key="active-doubao-key".into();
        assert_eq!(news_search_config_from(&settings).unwrap().model,"my-doubao-endpoint");
        assert_eq!(news_search_config_from(&settings).unwrap().api_key,"active-doubao-key");
        settings.llm_web_search=Some(false);assert!(news_search_config_from(&settings).is_err());
    }
    #[test]
    fn global_volume_migrates_once_and_ignores_future_pet_changes() {
        let dir = tempfile::tempdir().unwrap();
        let pet = dir.path().join("custom-appearances/pet-a");
        std::fs::create_dir_all(&pet).unwrap();
        std::fs::write(pet.join("persona-voice.json"), r#"{"voice":{"volume":0.3},"persona":{"private":"never exported"}}"#).unwrap();
        assert_eq!(super::read_volume_at(dir.path()).unwrap(), 0.3);
        std::fs::write(pet.join("persona-voice.json"), r#"{"voice":{"volume":1.5}}"#).unwrap();
        assert_eq!(super::read_volume_at(dir.path()).unwrap(), 0.3);
        super::write_volume_at(dir.path(), 0.7).unwrap();
        assert_eq!(super::read_volume_at(dir.path()).unwrap(), 0.7);
        let saved = std::fs::read_to_string(dir.path().join(super::VOLUME_FILE)).unwrap();
        assert!(!saved.contains("private") && !saved.contains("pet-a"));
        for invalid in [f32::NAN, f32::INFINITY, -1.0, 0.0, 1.6] {
            assert!(super::write_volume_at(dir.path(), invalid).is_err());
        }
        assert_eq!(super::read_volume_at(dir.path()).unwrap(), 0.7);
    }

    #[test]
    fn global_volume_defaults_and_corruption_is_not_silently_full_volume() {
        let dir = tempfile::tempdir().unwrap();
        assert_eq!(super::read_volume_at(dir.path()).unwrap(), 0.8);
        std::fs::write(dir.path().join(super::VOLUME_FILE), "broken").unwrap();
        assert!(super::read_volume_at(dir.path()).is_err());
        super::write_volume_at(dir.path(), 0.3).unwrap();
        assert_eq!(super::read_volume_at(dir.path()).unwrap(), 0.3);
    }
    use super::*;

    #[test]
    fn provider_changes_drop_old_secrets_even_when_input_is_blank_or_masked() {
        let providers = || LLM_PRESETS.iter().map(|preset| preset.id).chain(["custom"]);
        for from in providers() {
            for to in providers() {
                for incoming in [None, Some(""), Some("  "), Some("tes…mask"), Some("••••")] {
                    let stored = VoiceChatSettings { llm_provider: from.into(), llm_api_key:"old-test-key".into(),
                        llm_model:"saved-model".into(), ..Default::default() };
                    let merged = merge_settings(stored, VoiceChatSettingsInput { llm_provider:Some(to.into()),
                        llm_api_key:incoming.map(str::to_owned), ..Default::default() });
                    assert_eq!(merged.llm_api_key, if from == to {"old-test-key"} else {""}, "{from} -> {to}");
                    assert_eq!(merged.llm_model, if from == to {"saved-model"} else {""});
                }
            }
        }
    }

    #[test]
    fn switching_and_saving_a_new_key_preserves_each_provider_in_its_own_slot() {
        let dir = tempfile::tempdir().unwrap(); let path = dir.path().join(FILE_NAME);
        let previous = VoiceChatSettings {llm_provider:"doubao".into(),llm_api_key:"old-test-key".into(),..Default::default()};
        let next = merge_settings(previous, VoiceChatSettingsInput {llm_provider:Some("mimo".into()),
            llm_api_key:Some("  new-test-key  ".into()), ..Default::default()});
        write_file(&path, &next).unwrap();
        let mut saved = read_file(&path).unwrap().unwrap();
        apply_llm_defaults(&mut saved);
        assert_eq!(saved.llm_provider,"mimo"); assert_eq!(saved.llm_api_key,"new-test-key");
        assert_eq!(saved.llm_model,"mimo-v2.6-flash");
        assert_eq!(saved.llm_api_keys.get("doubao").map(String::as_str),Some("old-test-key"));
        let restored = merge_settings(saved,VoiceChatSettingsInput {llm_provider:Some("doubao".into()),..Default::default()});
        assert_eq!(restored.llm_api_key,"old-test-key");
        assert_eq!(restored.llm_api_keys.get("mimo").map(String::as_str),Some("new-test-key"));
    }

    #[test]
    fn custom_endpoint_changes_require_a_new_key_but_model_changes_do_not() {
        let previous = VoiceChatSettings {llm_provider:"custom".into(),llm_base_url:"https://example.invalid/v1".into(),
            llm_api_key:"old-test-key".into(),..Default::default()};
        for endpoint in ["https://another.invalid/v1", "https://example.invalid/v2", ""] {
            let merged = merge_settings(previous.clone(), VoiceChatSettingsInput {llm_base_url:Some(endpoint.into()),..Default::default()});
            assert!(merged.llm_api_key.is_empty());
        }
        let same = merge_settings(previous.clone(), VoiceChatSettingsInput {
            llm_provider:Some("custom".into()),llm_base_url:Some(" https://example.invalid/v1/ ".into()),
            llm_model:Some("other-model".into()),..Default::default()});
        assert_eq!(same.llm_api_key,"old-test-key"); assert_eq!(same.llm_model,"other-model");
        let replaced = merge_settings(previous, VoiceChatSettingsInput {llm_base_url:Some("https://new.invalid".into()),
            llm_api_key:Some("new-test-key".into()), ..Default::default()});
        assert_eq!(replaced.llm_api_key,"new-test-key");
    }

    #[test]
    fn switching_provider_never_borrows_a_key() {
        let previous = VoiceChatSettings {llm_provider:"deepseek".into(),llm_api_key:"deepseek-test-key".into(),..Default::default()};
        let next = merge_settings(previous,VoiceChatSettingsInput {llm_provider:Some("doubao".into()),..Default::default()});
        assert!(next.llm_api_key.is_empty());
        let back = merge_settings(next,VoiceChatSettingsInput {llm_provider:Some("deepseek".into()),..Default::default()});
        assert_eq!(back.llm_api_key,"deepseek-test-key");
    }

    #[test]
    fn each_provider_and_custom_endpoint_restores_its_saved_key_after_restart() {
        let dir = tempfile::tempdir().unwrap(); let path = dir.path().join(FILE_NAME);
        let cases = [("deepseek", "", "ds-test-key"), ("doubao", "", "db-test-key"),
            ("mimo", "", "mi-test-key"), ("custom", "https://one.invalid/v1", "one-test-key"),
            ("custom", "https://two.invalid/v1", "two-test-key")];
        let mut settings = VoiceChatSettings::default();
        for (provider, endpoint, key) in cases {
            settings = merge_settings(settings,VoiceChatSettingsInput {llm_provider:Some(provider.into()),
                llm_base_url:Some(endpoint.into()),llm_model:Some(format!("model-{key}")),llm_api_key:Some(key.into()),..Default::default()});
            write_file(&path,&settings).unwrap(); settings=read_file(&path).unwrap().unwrap();
        }
        for (provider, endpoint, key) in cases.into_iter().rev() {
            settings = merge_settings(settings,VoiceChatSettingsInput {llm_provider:Some(provider.into()),
                llm_base_url:Some(endpoint.into()),..Default::default()});
            assert_eq!(settings.llm_api_key,key);
            assert_eq!(settings.llm_model,format!("model-{key}"));
            let masks = credential_masks(&settings);
            assert_eq!(masks.len(),5);
            for (_, _, raw) in cases { assert!(!serde_json::to_string(&masks).unwrap().contains(raw)); }
            write_file(&path,&settings).unwrap(); settings=read_file(&path).unwrap().unwrap();
        }
        settings = merge_settings(settings,VoiceChatSettingsInput {llm_provider:Some("custom".into()),..Default::default()});
        assert_eq!(settings.llm_base_url,"https://one.invalid/v1");
        assert_eq!(settings.llm_api_key,"one-test-key");
    }

    #[test]
    fn legacy_single_key_is_migrated_only_to_its_recorded_provider() {
        let mut legacy: VoiceChatSettings = serde_json::from_str(r#"{"llmProvider":"mimo","llmApiKey":"legacy-mimo-key"}"#).unwrap();
        remember_active_key(&mut legacy);
        assert_eq!(legacy.llm_api_keys.len(),1);
        assert_eq!(legacy.llm_api_keys.get("mimo").map(String::as_str),Some("legacy-mimo-key"));
        let masks = credential_masks(&legacy);
        assert!(masks.contains_key("mimo")); assert!(!masks.contains_key("doubao")); assert!(!masks.contains_key("deepseek"));
    }

    #[test]
    fn public_model_defaults_never_create_or_overwrite_user_keys() {
        for provider in ["doubao", "deepseek", "mimo", "custom", "unknown"] {
            let mut fresh = VoiceChatSettings { llm_provider:provider.into(), ..Default::default() };
            apply_llm_defaults(&mut fresh);
            assert!(fresh.llm_api_key.is_empty());
            fresh.llm_api_key="fixture-user-key".into();
            apply_llm_defaults(&mut fresh);
            assert_eq!(fresh.llm_api_key,"fixture-user-key");
        }
    }

    #[test]
    fn shared_speech_migration_preserves_existing_asr_and_llm() {
        let dir = tempfile::tempdir().unwrap(); let path = dir.path().join(FILE_NAME);
        let legacy = VoiceChatSettings { tts_api_key: "legacy-test-key".into(), tts_provider: "volcengine".into(), tts_resource_id: "old".into(), llm_api_key:"llm-test-key".into(), ..Default::default() };
        write_file(&path, &legacy).unwrap();
        migrate_shared_speech_key_at(&path, |provider, _, key| {
            assert_eq!((provider, key), ("volcengine", "legacy-test-key"));
            Ok(true)
        }).unwrap();
        let migrated = read_file(&path).unwrap().unwrap();
        assert!(migrated.tts_api_key.is_empty());
        assert_eq!(migrated.llm_api_key, legacy.llm_api_key);
        assert_eq!(migrated.tts_resource_id, TTS_DEFAULT_RESOURCE_ID);
        #[cfg(unix)] { use std::os::unix::fs::PermissionsExt; assert_eq!(fs::metadata(&path).unwrap().permissions().mode() & 0o777, 0o600); }
    }

    #[test]
    fn tts_only_migration_copies_before_clearing_and_can_retry() {
        let dir = tempfile::tempdir().unwrap(); let path = dir.path().join(FILE_NAME);
        let legacy = VoiceChatSettings { tts_api_key: "legacy-test-key".into(), ..Default::default() };
        write_file(&path, &legacy).unwrap();
        assert!(migrate_shared_speech_key_at(&path, |_, _, _| Err("credential store unavailable".into())).is_err());
        assert_eq!(read_file(&path).unwrap().unwrap().tts_api_key, legacy.tts_api_key);
        migrate_shared_speech_key_at(&path, |_, _, _| Ok(false)).unwrap();
        assert_eq!(read_file(&path).unwrap().unwrap().tts_api_key, legacy.tts_api_key);
        let mut copied = String::new();
        migrate_shared_speech_key_at(&path, |_, _, key| { copied = key.to_string(); Ok(true) }).unwrap();
        assert_eq!(copied, legacy.tts_api_key);
        assert!(read_file(&path).unwrap().unwrap().tts_api_key.is_empty());
        migrate_shared_speech_key_at(&path, |_, _, _| panic!("migration must be idempotent")).unwrap();
    }

    #[test]
    fn presets_fill_endpoint_and_model_and_unknown_providers_fall_back_to_doubao() {
        assert_eq!(normalize_provider("Doubao"), "doubao");
        assert_eq!(normalize_provider("nope"), "doubao");
        assert_eq!(normalize_provider("custom"), "custom");
        let preset = llm_preset("mimo").unwrap();
        assert_eq!(preset.base_url, "https://api.xiaomimimo.com/v1");
        assert_eq!(preset.model, "mimo-v2.6-flash");
        let token_plan = llm_preset("mimo-token-plan-cn").unwrap();
        assert_eq!(token_plan.base_url, "https://token-plan-cn.xiaomimimo.com/v1");
        assert_eq!(token_plan.label, "MiMo Token Plan");
    }

    #[test]
    fn editable_preset_models_survive_persistence_and_defaults_only_fill_blanks() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join(FILE_NAME);
        for preset in LLM_PRESETS {
            let mut settings = VoiceChatSettings {
                llm_provider: preset.id.into(), llm_model: "  account-model-id  ".into(),
                llm_base_url: "https://untrusted.invalid".into(), ..Default::default()
            };
            write_file(&path, &settings).unwrap();
            settings = read_file(&path).unwrap().unwrap();
            apply_llm_defaults(&mut settings);
            assert_eq!(settings.llm_model, "account-model-id");
            assert_eq!(settings.llm_base_url, preset.base_url);
            apply_llm_defaults(&mut settings);
            assert_eq!(settings.llm_model, "account-model-id");
            settings.llm_model = "  ".into();
            apply_llm_defaults(&mut settings);
            assert_eq!(settings.llm_model, preset.model);
        }
        assert_eq!(llm_preset("deepseek").unwrap().model, "deepseek-flash");
        assert_eq!(llm_preset("doubao").unwrap().model, "doubao-seed-2-1-lite-260915");
        let mut custom = VoiceChatSettings { llm_provider: "custom".into(),
            llm_base_url: "https://example.invalid/v1".into(), llm_model: "my-model".into(), ..Default::default() };
        apply_llm_defaults(&mut custom);
        assert_eq!(custom.llm_model, "my-model");
        assert_eq!(custom.llm_base_url, "https://example.invalid/v1");
    }

    #[test]
    fn masks_keep_only_head_and_tail() {
        assert_eq!(mask_secret(""), "");
        assert_eq!(mask_secret("short"), "••••");
        assert_eq!(mask_secret("sk-1234567890abcdef"), "sk-…cdef");
    }

    #[test]
    fn fresh_install_uses_doubao_but_saved_choices_and_legacy_keys_stay_scoped() {
        let mut fresh = VoiceChatSettings::default();
        apply_llm_defaults(&mut fresh);
        assert_eq!(fresh.llm_provider, "doubao");
        assert_eq!(fresh.llm_model, llm_preset("doubao").unwrap().model);
        assert!(fresh.llm_api_key.is_empty());
        let dir=tempfile::tempdir().unwrap(); let path=dir.path().join(FILE_NAME);
        let legacy=VoiceChatSettings{llm_api_key:"legacy-test-key".into(),..Default::default()};
        write_file(&path,&legacy).unwrap();
        let mut loaded=read_file(&path).unwrap().unwrap(); apply_llm_defaults(&mut loaded);
        assert_eq!(loaded.llm_provider,"deepseek");
        let switched=merge_settings(loaded,VoiceChatSettingsInput{llm_provider:Some("doubao".into()),..Default::default()});
        assert!(switched.llm_api_key.is_empty());
        for provider in ["deepseek","mimo","custom"] {
            let mut saved=VoiceChatSettings{llm_provider:provider.into(),llm_model:"my-model".into(),..Default::default()};
            apply_llm_defaults(&mut saved); assert_eq!(saved.llm_provider,provider); assert_eq!(saved.llm_model,"my-model");
        }
    }

    #[test]
    fn search_opt_out_persists_and_old_mimo_preset_migrates() {
        let mut settings=VoiceChatSettings{llm_provider:"mimo".into(),llm_model:"mimo-v2.6".into(),..Default::default()};
        apply_llm_defaults(&mut settings); assert_eq!(settings.llm_model,"mimo-v2.6-flash");
        assert!(settings.llm_web_search.unwrap_or(true));
        settings=merge_settings(settings,VoiceChatSettingsInput{llm_web_search:Some(false),..Default::default()});
        let decoded:VoiceChatSettings=serde_json::from_value(serde_json::to_value(&settings).unwrap()).unwrap();
        assert_eq!(decoded.llm_web_search,Some(false));
    }

    #[test]
    fn saving_a_masked_secret_keeps_the_stored_key() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join(FILE_NAME);
        write_file(
            &path,
            &VoiceChatSettings { llm_api_key: "sk-realkey123456".into(), ..Default::default() },
        )
        .unwrap();
        let mut stored = read_file(&path).unwrap().unwrap();
        let masked = mask_secret(&stored.llm_api_key);
        stored = merge_settings(stored, VoiceChatSettingsInput {llm_api_key:Some(masked),..Default::default()});
        assert_eq!(stored.llm_api_key, "sk-realkey123456");
    }
}

#[cfg(all(test, target_os = "macos"))]
#[path = "../test-fixtures/custom_speech_providers.rs"]
mod custom_provider_acceptance;
