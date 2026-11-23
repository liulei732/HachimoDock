// Child processes give each cold read fresh production OnceLocks and an isolated config directory.
use super::*;
use serde_json::{json, Value};
use std::{process::Command, time::Duration};

const CASE: &str =
    "voice_chat_settings::custom_provider_acceptance::independent_providers_over_loopback";
const PCM: &[u8] = include_bytes!("speech-probe.s16le");

#[tokio::test]
async fn independent_providers_over_loopback() {
    let Some(directory) = std::env::var_os("PET_CUSTOM_SPEECH_FIXTURE") else {
        let script =
            Path::new(env!("CARGO_MANIFEST_DIR")).join("test-fixtures/custom_speech_providers.py");
        let status = Command::new("python3")
            .env("PYTHONDONTWRITEBYTECODE", "1")
            .arg(script)
            .arg(std::env::current_exe().unwrap())
            .arg(CASE)
            .status()
            .unwrap();
        assert!(status.success(), "local speech provider acceptance failed");
        return;
    };
    let dir = PathBuf::from(directory).canonicalize().unwrap();
    let tmp = dir.parent().unwrap();
    assert_eq!(
        tmp.parent().unwrap(),
        std::env::temp_dir().canonicalize().unwrap()
    );
    assert!(tmp
        .file_name()
        .unwrap()
        .to_string_lossy()
        .starts_with("pet-speech-custom-"));
    assert!(dir
        .file_name()
        .unwrap()
        .to_string_lossy()
        .starts_with("case-"));
    configure_storage_dir(dir.clone()).unwrap();
    volcengine_asr::configure_storage_dir(dir.clone()).unwrap();
    crate::llm_network::configure_storage_dir(dir.clone()).unwrap();
    let plan: Value = serde_json::from_slice(&fs::read(tmp.join("step.json")).unwrap()).unwrap();
    match plan["action"].as_str().unwrap() {
        "init" => {
            crate::llm_network::save_llm_network_settings(
                serde_json::from_value(json!({
                    "mode":"direct", "caFile":tmp.join("ca.pem")
                }))
                .unwrap(),
            )
            .unwrap();
            if let Some(legacy) = plan["legacy"].as_object() {
                // Serialize literal old layouts rather than seeding via the current save implementation.
                for (field, file, purpose) in [
                    ("voice", FILE_NAME, "voice-chat"),
                    ("asr", "volcengine-device-asr.json", "speech-asr"),
                ] {
                    if let Some(old) = legacy.get(field) {
                        crate::secure_config::write(
                            &dir.join(file),
                            purpose,
                            &serde_json::to_vec(old).unwrap(),
                        )
                        .unwrap();
                    }
                }
            }
        }
        "save" | "reject" => {
            let stages = custom_snapshot().await;
            let which = plan["input"]["stage"].as_str().unwrap();
            let other = if which == "asr" { "tts" } else { "asr" };
            let before = fs::read(dir.join(FILE_NAME)).ok();
            let result = crate::save_custom_speech_stage(
                serde_json::from_value(plan["input"].clone()).unwrap(),
            )
            .await;
            if plan["action"] == "reject" {
                assert!(
                    result.is_err(),
                    "cross-endpoint credential reuse was accepted"
                );
                assert!(
                    before == fs::read(dir.join(FILE_NAME)).ok(),
                    "rejected change altered storage"
                );
            } else {
                let receipt = serde_json::to_value(result.unwrap()).unwrap();
                assert_eq!(receipt, custom_snapshot().await);
                assert_eq!(
                    receipt[other], stages[other],
                    "single-side save changed other stage"
                );
                assert_eq!(receipt[which]["baseUrl"], plan["input"]["baseUrl"]);
                assert_eq!(receipt[which]["model"], plan["input"]["model"]);
                assert_eq!(receipt[which]["configured"], true);
                assert_eq!(receipt[which]["active"], true);
            }
        }
        "preset" => {
            let result = crate::save_speech_settings(
                serde_json::from_value(json!({
                    "provider":"volcengine", "apiKey":"fixture-secret-preset-only"
                }))
                .unwrap(),
            )
            .await
            .unwrap();
            public(&serde_json::to_value(result).unwrap());
        }
        "probe" => {
            let before = custom_snapshot().await;
            let stage = plan["stage"].as_str().unwrap();
            assert_eq!(before[stage]["configured"], true);
            assert_eq!(before[stage]["active"], false);
            let bytes = fs::read(dir.join(FILE_NAME)).unwrap();
            let result =
                crate::test_custom_speech_stage(serde_json::from_value(json!(stage)).unwrap())
                    .await
                    .unwrap();
            let receipt = serde_json::to_value(result).unwrap();
            public(&receipt);
            assert_eq!(receipt["stage"], stage);
            assert!(receipt["latencyMs"].is_number());
            assert_eq!(custom_snapshot().await, before);
            assert!(
                bytes == fs::read(dir.join(FILE_NAME)).unwrap(),
                "probe changed durable settings"
            );
        }
        "run" => {
            let asr =
                serde_json::to_value(crate::load_device_asr_settings().await.unwrap()).unwrap();
            let both = custom_snapshot().await;
            assert_eq!(both["asr"]["baseUrl"], plan["asrUrl"]);
            assert_eq!(both["tts"]["baseUrl"], plan["ttsUrl"]);
            for stage in ["asr", "tts"] {
                assert_eq!(both[stage]["active"], true);
                assert_eq!(both[stage]["configured"], true);
            }
            let tts = serde_json::to_value(
                crate::realtime_chat::load_voice_chat_settings()
                    .await
                    .unwrap(),
            )
            .unwrap();
            public(&asr);
            public(&tts);
            assert_eq!(asr["baseUrl"], plan["asrUrl"]);
            assert_eq!(asr["model"], plan["asrModel"]);
            assert_eq!(tts["ttsBaseUrl"], plan["ttsUrl"]);
            assert_eq!(tts["ttsModel"], plan["ttsModel"]);
            assert_eq!(tts["ttsVoice"], plan["voice"]);
            let (tx, mut rx) = tokio::sync::mpsc::unbounded_channel();
            let asr = volcengine_asr::StreamingSpeechRecognizer::start(move |event| {
                let _ = tx.send(event);
            })
            .unwrap();
            asr.push_pcm(PCM).unwrap();
            asr.finish().unwrap();
            let text = tokio::time::timeout(Duration::from_secs(10), async {
                loop {
                    match rx.recv().await.unwrap() {
                        volcengine_asr::StreamingSpeechEvent::Final { text, .. } => break text,
                        volcengine_asr::StreamingSpeechEvent::Error(e) => panic!("{e}"),
                        _ => {}
                    }
                }
            })
            .await
            .unwrap();
            assert_eq!(text, "Two providers.");
            let mut pcm = Vec::new();
            let mut client =
                crate::doubao_tts::DoubaoTtsClient::new(tts_config("", "", 1.0).unwrap());
            let count = client
                .synthesize("Two providers.", |chunk| pcm.extend_from_slice(chunk))
                .await
                .unwrap();
            assert_eq!(count, 400);
            assert_eq!(pcm, [0x34, 0x12].repeat(200));
        }
        other => panic!("unknown fixture stage {other}"),
    }
    for file in [FILE_NAME, "volcengine-device-asr.json"] {
        if let Ok(raw) = fs::read(dir.join(file)) {
            assert!(
                !raw.windows(b"fixture-secret-".len())
                    .any(|w| w == b"fixture-secret-"),
                "plaintext credential in settings file"
            );
        }
    }
}

fn public(value: &Value) {
    // The app may display the first/last few characters of a key, but never a complete fixture key.
    assert!(
        !value.to_string().contains("fixture-secret-"),
        "unmasked credential in public IPC response"
    );
}

async fn custom_snapshot() -> Value {
    let status = serde_json::to_value(crate::load_custom_speech_settings().await.unwrap()).unwrap();
    public(&status);
    assert_eq!(status.as_object().unwrap().len(), 2);
    for stage in ["asr", "tts"] {
        let fields = status[stage].as_object().unwrap();
        assert_eq!(fields.len(), 5);
        for name in ["baseUrl", "model", "voice", "configured", "active"] {
            assert!(fields.contains_key(name));
        }
    }
    status
}
