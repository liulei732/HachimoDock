use super::*;
use tokio::io::{AsyncBufReadExt, AsyncReadExt, AsyncWriteExt, BufReader};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::oneshot;

async fn request(listener: &TcpListener, path: &str) -> TcpStream {
    let mut read = BufReader::new(timeout(Duration::from_secs(5), listener.accept()).await.unwrap().unwrap().0);
    let mut line = String::new();
    read.read_line(&mut line).await.unwrap();
    assert_eq!(line, format!("POST /v1/{path} HTTP/1.1\r\n"));
    let mut len = None;
    loop {
        line.clear();
        assert!(read.read_line(&mut line).await.unwrap() > 0);
        if line == "\r\n" { break; }
        if let Some(value) = line.to_ascii_lowercase().strip_prefix("content-length:") {
            len = Some(value.trim().parse::<usize>().unwrap());
        }
    }
    let len = len.unwrap();
    assert!((1..512_000).contains(&len));
    read.read_exact(&mut vec![0; len]).await.unwrap();
    read.into_inner()
}

fn configure_stage(stage: &str, addr: std::net::SocketAddr) {
    let input = serde_json::from_value(json!({
        "stage":stage, "baseUrl":format!("http://{addr}/v1"), "apiKey":"offline-fixture",
        "model":"fixture-model", "voice":"fixture-voice"
    })).unwrap();
    voice_chat_settings::custom_speech::save(input).unwrap();
}

async fn slow_asr() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    configure_stage("asr", listener.local_addr().unwrap());
    let server = tokio::spawn(async move {
        let mut stream = request(&listener, "audio/transcriptions").await;
        tokio::time::sleep(Duration::from_secs(10)).await;
        stream.write_all(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 15\r\nConnection: close\r\n\r\n{\"text\":\"done\"}").await.unwrap();
    });
    let (send, mut events) = mpsc::unbounded_channel();
    let rec = StreamingSpeechRecognizer::start(move |event| { let _ = send.send(event); }).unwrap();
    let budget = rec.final_wait_budget();
    assert_eq!(budget, Duration::from_secs(61));
    rec.push_pcm(&[1; FRAME_BYTES]).unwrap();
    rec.finish().unwrap();
    let (mic, mut rx) = mpsc::channel(128);
    let capture = tokio::spawn(async move {
        // A burst larger than the former 576-frame cap while the final is outstanding.
        for _ in 0..650 { mic.send(Input::Pcm(vec![0; FRAME_BYTES], Some(false))).await.unwrap(); }
        mic
    });
    let mut pending = VecDeque::new();
    let text = wait_for_asr_final(&rec, &mut events, String::new(), &mut rx, &mut pending, true).await.unwrap();
    assert_eq!(text, "done");
    assert_eq!(pending.len(), 650);
    drop(capture.await.unwrap());
    server.await.unwrap();
}

async fn first_packet_and_preview_stop() {
    let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
    configure_stage("tts", listener.local_addr().unwrap());
    let config = voice_chat_settings::saved_tts_probe_config().unwrap();
    let (allow_audio, gate) = oneshot::channel();
    let (accepted, queued) = oneshot::channel();
    let (player, _rx) = Player::channel(Arc::new(Mutex::new(Default::default())));
    let server = async {
        let mut socket = request(&listener, "audio/speech").await;
        tokio::time::sleep(Duration::from_secs(3)).await;
        assert!(player.commands.lock().unwrap().is_empty());
        socket.write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 48\r\nConnection: close\r\n\r\n").await.unwrap();
        socket.write_all(b"RIFF\x28\0\0\0WAVEfmt \x10\0\0\0\x01\0\x01\0\x80>\0\0\0}\0\0\x02\0\x10\0data\x04\0\0\0\x01\0").await.unwrap();
        allow_audio.send(()).unwrap();
        timeout(Duration::from_millis(300), queued).await.unwrap().unwrap();
        {
            let commands = player.commands.lock().unwrap();
            assert!(matches!(&commands[0].1, PlayerCmd::Begin(_)));
            assert!(matches!(&commands[1].1, PlayerCmd::Subtitle(s) if s == "问候"));
            assert!(matches!(&commands[2].1, PlayerCmd::Pcm(p, _) if p == &[1,0]));
        }
        tokio::time::sleep(Duration::from_secs(2)).await;
        socket.write_all(&[2,0]).await.unwrap();
    };
    let mut tts = DoubaoTtsClient::new(config);
    let greeting = speak_one_logged(&mut tts, &player, "问候", "", async {
        gate.await.unwrap();
        player.send(PlayerCmd::Begin("greeting".into())).unwrap();
        Ok(())
    }, || accepted.send(()).unwrap());
    let (result, ()) = tokio::join!(greeting, server);
    result.unwrap();
    assert_eq!(player.commands.lock().unwrap().len(), 4);

    let id = reserve_persona_voice_preview();
    let preview = persona_voice_preview(PersonaVoicePreviewInput { text: "试听".into(), voice: Default::default() }, Some(id.clone()));
    let close = async {
        let mut socket = request(&listener, "audio/speech").await;
        cancel_persona_voice_preview(id.clone());
        let mut buf = [0; 1];
        assert_eq!(timeout(Duration::from_millis(300), socket.read(&mut buf)).await.unwrap().unwrap(), 0);
    };
    let (result, ()) = tokio::join!(preview, close);
    result.unwrap();
    let newer = reserve_persona_voice_preview();
    cancel_persona_voice_preview(id.clone());
    persona_voice_preview(PersonaVoicePreviewInput { text: "关闭的试听".into(), voice: Default::default() },
        Some(id)).await.unwrap();
    assert!(timeout(Duration::from_millis(50), listener.accept()).await.is_err());
    assert_eq!(preview_owner().borrow().as_ref(), Some(&newer));
    cancel_persona_voice_preview(newer);
}

#[test]
fn delayed_voice_and_scoped_forget_across_restart() {
    const TEST: &str = "realtime_chat::voice_timing_tests::delayed_voice_and_scoped_forget_across_restart";
    const FLAG: &str = "PET_TEST_VOICE_TIMING_DIR";
    let Some(dir) = std::env::var_os(FLAG) else {
        let dir = tempfile::tempdir().unwrap();
        for phase in ["setup", "cold"] {
            let result = std::process::Command::new(std::env::current_exe().unwrap())
                .args(["--exact", TEST, "--nocapture"]).env(FLAG, dir.path()).env("PET_TEST_VOICE_PHASE", phase)
                .env_remove("HACHIMO_ASR_PROVIDER").env_remove("VOLCENGINE_ASR_API_KEY").output().unwrap();
            assert!(result.status.success(), "{}{}", String::from_utf8_lossy(&result.stdout), String::from_utf8_lossy(&result.stderr));
        }
        return;
    };
    let dir = std::path::PathBuf::from(dir);
    voice_chat_settings::configure_storage_dir(dir.clone()).unwrap();
    crate::volcengine_asr::configure_storage_dir(dir.clone()).unwrap();
    crate::llm_network::configure_storage_dir(dir).unwrap();
    const CUSTOM: &str = "https://one.invalid/v1";
    if std::env::var("PET_TEST_VOICE_PHASE").unwrap() == "setup" {
        for (provider, url) in [("deepseek", ""), ("custom", CUSTOM), ("custom", "https://two.invalid/v1"), ("doubao", "")] {
            voice_chat_settings::save(serde_json::from_value(json!({"llmProvider":provider, "llmBaseUrl":url,
                "llmApiKey":"fixture-key","llmSearchApiKey":"fixture-search"})).unwrap()).unwrap();
        }
        voice_chat_settings::clear_secret("llm", Some(&format!("custom:{CUSTOM}"))).unwrap();
        voice_chat_settings::clear_secret("llm", Some("doubao")).unwrap();
        tauri::async_runtime::block_on(async { slow_asr().await; first_packet_and_preview_stop().await; });
    } else {
        let status = voice_chat_settings::status().unwrap();
        assert_eq!(status.llm_provider, "doubao");
        assert!(!status.llm_configured);
        assert!(status.llm_search_configured && status.tts_configured);
        assert!(crate::volcengine_asr::settings_status().unwrap().configured);
        assert!(status.llm_api_key_masks.contains_key("deepseek"));
        assert!(status.llm_api_key_masks.contains_key("custom:https://two.invalid/v1"));
        assert!(!status.llm_api_key_masks.contains_key(&format!("custom:{CUSTOM}")));
        assert!(!status.llm_api_key_masks.contains_key("doubao"));
    }
}
