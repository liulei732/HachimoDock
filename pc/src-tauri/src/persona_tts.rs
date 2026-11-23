//! Resolve an appearance's optional voice only after the global synthesis route
//! and its credentials have been selected. Overrides cannot select a destination.
use crate::doubao_tts::DoubaoTtsConfig;
use crate::voice_chat_settings::{custom_speech_endpoint, official_mimo_endpoint, tts_config};
use serde::{Deserialize, Serialize};

#[derive(Clone, Debug, Default, Deserialize, Serialize, PartialEq)]
#[serde(default, deny_unknown_fields)]
pub(crate) struct VoiceOverrides {
    #[serde(skip_serializing_if = "Option::is_none")]
    mimo: Option<ModelVoice>,
    #[serde(skip_serializing_if = "Option::is_none")]
    openai: Option<CustomVoice>,
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq)]
#[serde(deny_unknown_fields)]
struct ModelVoice {
    model: String,
    voice: String,
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct CustomVoice {
    base_url: String,
    model: String,
    voice: String,
}

impl VoiceOverrides {
    pub(crate) fn is_empty(&self) -> bool { self.mimo.is_none() && self.openai.is_none() }

    pub(crate) fn validate(&self) -> Result<(), String> {
        let bounded = |model: &str, voice: &str| model.len() <= 256 && voice.len() <= 1024;
        if self.mimo.as_ref().is_some_and(|v| !bounded(&v.model, &v.voice))
            || self.openai.as_ref().is_some_and(|v| !bounded(&v.model, &v.voice)
                || v.base_url.len() > 2048 || custom_speech_endpoint(&v.base_url).is_err()) {
            return Err("形象音色模型、音色或自定义服务地址无效".into());
        }
        Ok(())
    }

    pub(crate) fn apply(&self, cfg: &mut DoubaoTtsConfig) -> Result<(), String> {
        self.validate()?;
        let voice = match cfg.tts_provider.as_str() {
            "mimo" if official_mimo_endpoint(&cfg.tts_base_url) => self.mimo.as_ref()
                .filter(|v| v.model.trim() == cfg.tts_model.trim()).map(|v| v.voice.trim()),
            "openai" => self.openai.as_ref().filter(|v| v.model.trim() == cfg.tts_model.trim())
                .filter(|v| crate::voice_chat_settings::custom_speech::Stage::Tts.same_destination(&v.base_url, &cfg.tts_base_url))
                .map(|v| v.voice.trim()),
            _ => None,
        };
        if let Some(voice) = voice.filter(|v| !v.is_empty()) { cfg.tts_voice = voice.into(); }
        Ok(())
    }

    pub(crate) fn config(&self, speaker: &str, clone_id: &str, speed: f32) -> Result<DoubaoTtsConfig, String> {
        let mut cfg = tts_config(speaker, clone_id, speed)?;
        self.apply(&mut cfg)?;
        Ok(cfg)
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    use crate::{component_voice, doubao_tts::DoubaoTtsClient, realtime_chat};
    use serde_json::{json, Value};

    async fn preview(slots: Value) {
        // This is the production preview up to the physical PC speaker.
        let input = serde_json::from_value(json!({"text":"试听","voice":{
            "volume":1.0,"ttsOverrides":slots,"speaker":"persona-speaker"}})).unwrap();
        assert_eq!(realtime_chat::preview_pcm(input).await.unwrap(), [0x34, 0x12].repeat(200));
    }

    pub(crate) async fn verify_appearance_routes(dir: &std::path::Path) {
        let slots = json!({"mimo":{"model":"edited-tts","voice":"pet-specific"}});
        preview(slots.clone()).await;
        component_voice::configure_preparation(dir.join("prepared-voice.json"));
        let pet = |id, overrides| serde_json::from_value(json!({"boardDeviceId":"test-board",
            "appearanceId":id,"speaker":"persona-speaker","cloneSpeakerId":"","speed":1.0,"ttsOverrides":overrides})).unwrap();
        for (id, overrides, voice) in [("one", slots.clone(), "pet-specific"), ("two", json!({}), "茉莉"), ("one", slots, "pet-specific")] {
            component_voice::component_speech_set_pet_voice(Some(pet(id, overrides))).unwrap();
            assert_eq!(component_voice::config("test-board").unwrap().tts_voice, voice);
            assert_eq!(component_voice::preparation_config("").unwrap().tts_voice, voice);
        }
        let mut client = DoubaoTtsClient::new(component_voice::config("test-board").unwrap());
        assert_eq!(client.synthesize("组件朗读", |_| {}).await.unwrap(), 400);
        crate::test_device_tts_settings().await.unwrap();
    }

    pub(crate) async fn verify_custom_appearance_routes() {
        let slots = json!({"openai":{"baseUrl":"https://one.invalid/v1/chat/completions",
            "model":"mimo-v2.5-tts-voicedesign","voice":"scoped-description"}});
        for (host, key) in [("one", "test-one"), ("two", "test-two"), ("one", "test-one")] {
            let input = json!({"stage":"tts","apiKey":key,"model":"mimo-v2.5-tts-voicedesign",
                "baseUrl":format!("https://{host}.invalid/v1/chat/completions"), "voice":"global-other"});
            crate::save_custom_speech_stage(serde_json::from_value(input).unwrap()).await.unwrap();
            assert_eq!(crate::volcengine_asr::settings_status().unwrap().provider, "mimo");
            preview(slots.clone()).await;
        }
    }

    #[test]
    fn voice_override_is_scoped_to_provider_model_and_synthesis_route() {
        let slots: VoiceOverrides = serde_json::from_value(serde_json::json!({
            "mimo":{"model":"design","voice":"小米音色描述"},
            "openai":{"baseUrl":"https://one.invalid/vendor/v1/chat/completions",
                "model":"design","voice":"自定义音色描述"}
        })).unwrap();
        for (provider, url, model, voice) in [
            ("mimo", "https://api.xiaomimimo.com/v1", "design", "小米音色描述"),
            ("mimo", "https://api.xiaomimimo.com/v1", "other", "global"),
            ("openai", "https://one.invalid/vendor/v1/chat/completions/", "design", "自定义音色描述"),
            ("openai", "https://two.invalid/vendor/v1/chat/completions", "design", "global"),
            ("openai", "https://one.invalid/other/v1/chat/completions", "design", "global"),
            ("openai", "https://one.invalid/vendor/v1", "design", "global"),
            ("openai", "https://one.invalid/vendor/v1/chat/completions", "other", "global"),
            ("volcengine", "", "", "global"),
        ] {
            let mut cfg = DoubaoTtsConfig::new("global-test-key", "speaker", "", 1.0, "");
            cfg.tts_provider = provider.into();
            cfg.tts_base_url = url.into();
            cfg.tts_model = model.into();
            cfg.tts_voice = "global".into();
            slots.apply(&mut cfg).unwrap();
            assert_eq!((cfg.tts_voice.as_str(), cfg.api_key.as_str(), cfg.tts_base_url.as_str()),
                (voice, "global-test-key", url));
            assert_eq!(cfg.speaker, "speaker");
        }
    }
}
