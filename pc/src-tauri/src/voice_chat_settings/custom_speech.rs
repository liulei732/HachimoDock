//! Independent custom speech stages in the existing encrypted voice settings record.
use super::*;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Deserialize, Serialize)]
#[serde(rename_all = "lowercase")]
pub(crate) enum Stage {
    Asr,
    Tts,
}

impl Stage {
    pub(crate) fn source(self) -> &'static str {
        match self {
            Self::Asr => model_issue::ASR,
            Self::Tts => model_issue::TTS,
        }
    }
    pub(crate) fn same_destination(self, first: &str, second: &str) -> bool {
        self.destination(first)
            .ok()
            .zip(self.destination(second).ok())
            .is_some_and(|(a, b)| a == b)
    }

    fn destination(self, url: &str) -> Result<String, String> {
        let route = custom_speech_endpoint(url)?;
        let path = if route.uses_chat_completions {
            "chat/completions"
        } else {
            match self {
                Self::Asr => "audio/transcriptions",
                Self::Tts => "audio/speech",
            }
        };
        Ok(format!("{}/{path}", route.api_root))
    }
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub(crate) struct Input {
    pub(crate) stage: Stage,
    base_url: String,
    model: String,
    voice: Option<String>,
    api_key: Option<String>,
}

#[derive(Default, Clone, Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase")]
pub(super) struct BoundKey {
    destination: String,
    #[serde(deserialize_with = "deserialize_saved_key")]
    api_key: String,
}

fn usable(value: &str) -> Option<&str> {
    Some(value.trim()).filter(|key| !key.is_empty() && !credential_placeholder(key))
}

impl BoundKey {
    fn new(stage: Stage, url: &str, key: &str) -> Result<Self, String> {
        Ok(Self {
            destination: stage.destination(url)?,
            api_key: key.into(),
        })
    }
    pub(super) fn for_tts(&self, url: &str) -> Option<String> {
        (self.destination == Stage::Tts.destination(url).ok()?)
            .then(|| usable(&self.api_key))
            .flatten()
            .map(str::to_owned)
    }
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct PublicStage {
    base_url: String,
    model: String,
    voice: String,
    configured: bool,
    active: bool,
}

#[derive(Serialize)]
pub(crate) struct Snapshot {
    asr: PublicStage,
    tts: PublicStage,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) struct Probe {
    stage: Stage,
    latency_ms: u128,
}

struct Profile<'a> {
    url: &'a str,
    model: &'a str,
    voice: &'a str,
    key: Option<String>,
    active: bool,
}

impl Profile<'_> {
    fn public(&self, stage: Stage) -> PublicStage {
        PublicStage {
            base_url: self.url.into(),
            model: self.model.into(),
            voice: self.voice.into(),
            active: self.active,
            configured: self.key.is_some()
                && !self.model.trim().is_empty()
                && stage.destination(self.url).is_ok(),
        }
    }
}

fn asr_profile(profiles: &StoredSpeechSettings) -> Profile<'_> {
    let (url, model, key) = profiles.custom_asr().unwrap_or_default();
    Profile {
        url,
        model,
        voice: "",
        key: usable(key).map(str::to_owned),
        active: profiles.active_provider() == "openai",
    }
}

fn tts_profile(settings: &VoiceChatSettings) -> Profile<'_> {
    let url = &settings.tts_base_url;
    let active = settings.speech_provider() == "openai";
    let key = match &settings.custom_tts_credential {
        Some(bound) => bound.for_tts(url),
        None => (if active {
            usable(&settings.tts_api_key)
        } else {
            None
        })
        .map(str::to_owned)
        .or_else(|| {
            settings
                .speech_profiles
                .as_ref()?
                .tts_key_for_scope("openai", url)
        }),
    };
    Profile {
        url,
        model: &settings.tts_model,
        voice: &settings.tts_voice,
        key,
        active,
    }
}

fn read_profiles(path: &Path) -> Result<VoiceChatSettings, String> {
    let mut settings = read_file(path)?.unwrap_or_default();
    if settings.speech_profiles.is_none() {
        settings.speech_profiles = Some(volcengine_asr::read_legacy_config()?.unwrap_or_default());
    }
    Ok(settings)
}

fn snapshot(settings: &VoiceChatSettings) -> Snapshot {
    Snapshot {
        asr: asr_profile(
            settings
                .speech_profiles
                .as_ref()
                .expect("materialized speech profiles"),
        )
        .public(Stage::Asr),
        tts: tts_profile(settings).public(Stage::Tts),
    }
}

pub(crate) fn load() -> Result<Snapshot, String> {
    let _guard = SETTINGS_MUTATION.lock().map_err(|_| "语音配置繁忙")?;
    Ok(snapshot(&read_profiles(&settings_path()?)?))
}

fn select_key(
    stage: Stage,
    url: &str,
    supplied: Option<&str>,
    prior: Profile<'_>,
) -> Result<BoundKey, String> {
    let destination = stage.destination(url)?;
    let key = supplied
        .and_then(usable)
        .map(str::to_owned)
        .or_else(|| {
            if stage.destination(prior.url).ok().as_ref() == Some(&destination) {
                prior.key
            } else {
                None
            }
        })
        .ok_or("此阶段的服务地址没有已保存的 API Key，请填写后保存")?;
    Ok(BoundKey {
        destination,
        api_key: key,
    })
}

fn freeze_legacy(settings: &mut VoiceChatSettings) -> Result<(), String> {
    if settings.custom_tts_credential.is_some() {
        return Ok(());
    }
    let prior = tts_profile(settings);
    let bound = match prior.key {
        Some(key) => BoundKey::new(Stage::Tts, prior.url, &key)?,
        None => BoundKey::default(),
    };
    if settings.speech_provider() == "openai" {
        settings.tts_api_key.clear();
    }
    materialize_speech(settings, volcengine_asr::read_legacy_config)?.retain_legacy_mimo();
    settings.custom_tts_credential = Some(bound);
    settings.speech_preset_checked = true;
    Ok(())
}

pub(crate) fn save(input: Input) -> Result<Snapshot, String> {
    save_at(&settings_path()?, input)
}

fn save_at(path: &Path, input: Input) -> Result<Snapshot, String> {
    let _guard = SETTINGS_MUTATION.lock().map_err(|_| "语音配置繁忙")?;
    let mut settings = read_profiles(path)?;
    let url = input.base_url.trim().trim_end_matches('/');
    let model = input.model.trim();
    if model.is_empty() {
        return Err("请填写此阶段的模型".into());
    }
    freeze_legacy(&mut settings)?;
    match input.stage {
        Stage::Asr => {
            let profiles = settings
                .speech_profiles
                .as_mut()
                .expect("materialized speech profiles");
            let bound = select_key(
                Stage::Asr,
                url,
                input.api_key.as_deref(),
                asr_profile(profiles),
            )?;
            profiles.pin_selection();
            profiles.apply(DeviceAsrSettingsInput {
                provider: Some("openai".into()),
                base_url: Some(url.into()),
                model: Some(model.into()),
                api_key: Some(bound.api_key),
                resource_id: None,
            })?;
        }
        Stage::Tts => {
            let bound = select_key(
                Stage::Tts,
                url,
                input.api_key.as_deref(),
                tts_profile(&settings),
            )?;
            // Voice can contain provider-specific design text; only carry it within its route.
            let same_destination = Stage::Tts.destination(&settings.tts_base_url).ok().as_ref()
                == Some(&bound.destination);
            if let Some(voice) = input.voice {
                settings.tts_voice = voice.trim().into();
            } else if !same_destination {
                settings.tts_voice.clear();
            }
            settings.custom_tts_credential = Some(bound);
            settings.tts_provider = "openai".into();
            settings.tts_base_url = url.into();
            settings.tts_model = model.into();
        }
    }
    write_file(path, &settings)?;
    Ok(snapshot(&settings))
}

// Legacy IPC remains callable, including a whole-preset selection after independent saves.
pub(super) fn bind_asr_edit(
    profiles: &StoredSpeechSettings,
    input: &mut DeviceAsrSettingsInput,
) -> Result<(), String> {
    if input
        .provider
        .as_deref()
        .map(str::trim)
        .filter(|provider| !provider.is_empty())
        .unwrap_or(profiles.active_provider())
        == "openai"
    {
        let prior = asr_profile(profiles);
        let url = input.base_url.as_deref().unwrap_or(prior.url);
        input.api_key = Some(select_key(Stage::Asr, url, input.api_key.as_deref(), prior)?.api_key);
    }
    Ok(())
}

pub(super) fn bind_combined_keys(
    settings: &mut VoiceChatSettings,
    input: &mut SpeechSettingsInput,
) -> Result<(), String> {
    let url = input.asr.base_url.as_deref().unwrap_or_default();
    let tts = select_key(
        Stage::Tts,
        url,
        input.asr.api_key.as_deref(),
        tts_profile(settings),
    )?;
    bind_asr_edit(
        settings.speech_profiles.as_ref().ok_or("语音配置不完整")?,
        &mut input.asr,
    )?;
    settings.custom_tts_credential = Some(tts);
    Ok(())
}

pub(super) fn bind_legacy_tts_edit(
    settings: &mut VoiceChatSettings,
    input: &mut VoiceChatSettingsInput,
) -> Result<(), String> {
    if input
        .tts_provider
        .as_deref()
        .unwrap_or(settings.speech_provider())
        .trim()
        == "openai"
        && (input.tts_base_url.is_some()
            || input.tts_api_key.is_some()
            || input.tts_provider.is_some())
    {
        let url = input
            .tts_base_url
            .as_deref()
            .unwrap_or(&settings.tts_base_url);
        let bound = select_key(
            Stage::Tts,
            url,
            input.tts_api_key.as_deref(),
            tts_profile(settings),
        )?;
        input.tts_api_key = None;
        settings.custom_tts_credential = Some(bound);
    }
    Ok(())
}

pub(crate) async fn probe(stage: Stage) -> Result<Probe, String> {
    let (url, model, voice, key) = tauri::async_runtime::spawn_blocking(move || {
        let _guard = SETTINGS_MUTATION.lock().map_err(|_| "语音配置繁忙")?;
        let settings = read_profiles(&settings_path()?)?;
        let p = match stage {
            Stage::Asr => asr_profile(
                settings
                    .speech_profiles
                    .as_ref()
                    .expect("materialized speech profiles"),
            ),
            Stage::Tts => tts_profile(&settings),
        };
        stage.destination(p.url)?;
        if p.model.trim().is_empty() {
            return Err(String::from("请先保存此阶段的模型"));
        }
        Ok((
            p.url.to_owned(),
            p.model.to_owned(),
            p.voice.to_owned(),
            p.key.ok_or("请先保存此阶段的 API Key")?,
        ))
    })
    .await
    .map_err(|_| "[CREDENTIALS] 读取语音配置任务中断")??;
    let started = std::time::Instant::now();
    match stage {
        Stage::Asr => {
            volcengine_asr::probe_custom_stage(url, model, key).await?;
        }
        Stage::Tts => {
            let settings = VoiceChatSettings {
                tts_provider: "openai".into(),
                tts_base_url: url,
                tts_model: model,
                tts_voice: voice,
                ..Default::default()
            };
            let config = tts_config_for(settings, &key, "zh_female_vv_uranus_bigtts", "", 1.0)?;
            crate::probe_device_tts(config).await?;
        }
    }
    Ok(Probe {
        stage,
        latency_ms: started.elapsed().as_millis(),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn seeded_record() -> (tempfile::TempDir, PathBuf) {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join(FILE_NAME);
        write_file(
            &path,
            &VoiceChatSettings {
                speech_profiles: Some(StoredSpeechSettings::default()),
                ..Default::default()
            },
        )
        .unwrap();
        (dir, path)
    }

    fn input(stage: Stage, url: &str, key: Option<&str>, voice: Option<&str>) -> Input {
        Input {
            stage,
            base_url: url.into(),
            model: "test-model".into(),
            api_key: key.map(str::to_owned),
            voice: voice.map(str::to_owned),
        }
    }

    #[test]
    fn tts_voice_only_follows_the_same_effective_request_url() {
        let (_dir, path) = seeded_record();
        let root = "https://one.invalid/v1";
        let audio = "https://one.invalid/v1/audio/speech";
        let chat = "https://one.invalid/v1/chat/completions";
        let other = "https://two.invalid/v1";
        for (url, key, voice, expected) in [
            (
                root,
                Some("fixture-a"),
                Some("original design"),
                "original design",
            ),
            (audio, Some("fixture-rotated"), None, "original design"),
            (chat, Some("fixture-chat"), None, ""),
            (chat, None, Some("chat design"), "chat design"),
            (other, Some("fixture-b"), None, ""),
            (
                root,
                Some("fixture-c"),
                Some("explicit design"),
                "explicit design",
            ),
            (audio, None, Some(""), ""),
        ] {
            let status = save_at(&path, input(Stage::Tts, url, key, voice)).unwrap();
            assert_eq!(status.tts.voice, expected);
            assert!(status.tts.active && status.tts.configured);
            assert!(!status.asr.active && !status.asr.configured);
            assert_eq!(read_file(&path).unwrap().unwrap().tts_voice, expected);
        }
    }

    #[test]
    fn distinct_legacy_keys_on_one_origin_report_independent_credentials() {
        let url = "https://legacy.invalid/v1";
        let mut profiles = StoredSpeechSettings::default();
        profiles
            .apply(
                serde_json::from_value(json!({
                    "provider":"openai","baseUrl":url,"model":"legacy-model","apiKey":"asr-fixture"
                }))
                .unwrap(),
            )
            .unwrap();
        let mut settings = VoiceChatSettings {
            speech_profiles: Some(profiles),
            tts_provider: "openai".into(),
            tts_base_url: url.into(),
            tts_api_key: "tts-fixture".into(),
            ..Default::default()
        };
        assert_eq!(
            resolve_tts_key(&settings).unwrap(),
            ("tts-fixture".into(), false)
        );
        settings.tts_api_key.clear();
        assert_eq!(
            resolve_tts_key(&settings).unwrap(),
            ("asr-fixture".into(), true)
        );
    }

    #[test]
    fn protocol_switch_requires_a_new_stage_key_in_all_split_save_paths() {
        let (_dir, path) = seeded_record();
        let root = "https://one.invalid/v1";
        let chat = "https://one.invalid/v1/chat/completions";
        save_at(&path, input(Stage::Asr, root, Some("fixture-a"), None)).unwrap();
        save_at(&path, input(Stage::Tts, root, Some("fixture-t"), None)).unwrap();
        let original = fs::read(&path).unwrap();
        for stage in [Stage::Asr, Stage::Tts] {
            assert!(save_at(&path, input(stage, chat, None, None)).is_err());
        }
        let combined = |url| {
            serde_json::from_value::<SpeechSettingsInput>(json!({
                "provider":"openai","baseUrl":url,"model":"updated-asr","ttsModel":"updated-tts"
            }))
            .unwrap()
        };
        for provider in [" openai ", " "] {
            let mut legacy_asr = combined(chat).asr;
            legacy_asr.provider = Some(provider.into());
            assert!(save_asr_at(Some(&path), legacy_asr).is_err());
        }
        assert!(save_voice_at(
            &path,
            VoiceChatSettingsInput {
                tts_provider: Some(" openai ".into()),
                tts_base_url: Some(chat.into()),
                ..Default::default()
            }
        )
        .is_err());
        assert!(save_speech_at(&path, combined(chat), || panic!(), write_file).is_err());
        assert_eq!(original, fs::read(&path).unwrap());
        save_speech_at(&path, combined(root), || panic!(), write_file).unwrap();
        let saved = read_file(&path).unwrap().unwrap();
        assert_eq!(tts_profile(&saved).key.as_deref(), Some("fixture-t"));
        assert_eq!(
            asr_profile(saved.speech_profiles.as_ref().unwrap())
                .key
                .as_deref(),
            Some("fixture-a")
        );
    }
}
