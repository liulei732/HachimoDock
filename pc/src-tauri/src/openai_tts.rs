//! OpenAI audio endpoints and MiMo-compatible Chat Completions TTS.
//! Synthesizes text to 16 kHz mono 16-bit PCM and feeds it through the same
//! on_pcm callback pipeline as the built-in provider.
use crate::model_issue::{self, TTS};

use base64::{engine::general_purpose::STANDARD, Engine};
use serde_json::json;

use crate::doubao_tts::DoubaoTtsConfig;
use crate::voice_chat_settings::custom_speech_endpoint;

const TARGET_RATE: u32 = 16_000;
const MAX_WAV_BYTES: usize = 8 * 1024 * 1024;

pub async fn synthesize_paced<F, Fut>(
    cfg: &DoubaoTtsConfig,
    text: &str,
    mut on_pcm: F,
) -> Result<usize, String>
where
    F: FnMut(&[u8]) -> Fut, Fut: std::future::Future<Output=Result<(), String>>,
{
    let model = cfg.tts_model.trim();
    if model.is_empty() {
        return Err("自定义语音合成未配置模型名称".into());
    }
    let endpoint = custom_speech_endpoint(&cfg.tts_base_url)?;
    let root = &endpoint.api_root;
    let client = model_issue::client(TTS, std::time::Duration::from_secs(60))?;
    let voice = cfg.tts_voice.trim();
    let request = if endpoint.uses_chat_completions {
        let mut messages = Vec::new();
        let mut audio = json!({"format": "wav"});
        if model == "mimo-v2.5-tts-voicedesign" {
            if voice.is_empty() || (cfg.tts_provider == "mimo" && voice == crate::voice_chat_settings::MIMO_VOICE) {
                return Err("请在合成音色中填写音色设计描述".into());
            }
            messages.push(json!({"role": "user", "content": voice}));
        } else if !voice.is_empty() {
            audio["voice"] = json!(voice);
        }
        messages.push(json!({"role": "assistant", "content": text}));
        client.post(format!("{root}/chat/completions")).json(&json!({
            "model": model,
            "messages": messages,
            "audio": audio,
            "stream": false,
        }))
    } else {
        client.post(format!("{root}/audio/speech")).json(&json!({
            "model": model,
            "input": text,
            "voice": voice,
            "response_format": "wav",
        }))
    };
    let request = endpoint.authorize(request, &cfg.api_key);
    let resp = request.send().await.map_err(|error| model_issue::network(TTS,&error))?;
    if !resp.status().is_success() {
        return Err(model_issue::http(TTS,resp.status().as_u16(),false));
    }
    let json_error = resp.headers().get("content-type").and_then(|v| v.to_str().ok()).is_some_and(|v| v.starts_with("application/json"));
    if !endpoint.uses_chat_completions && !json_error {
        return play_wav_response(resp, &mut on_pcm).await;
    }
    let json_bytes = model_issue::read_response(TTS,resp, 12 * 1024 * 1024).await?;
    let reply: serde_json::Value = serde_json::from_slice(&json_bytes)
        .map_err(|_| "语音合成响应格式无效")?;
    model_issue::check_reply(TTS,&reply)?;
    let data = reply["choices"][0]["message"]["audio"]["data"].as_str()
        .ok_or("语音合成响应缺少音频")?;
    let encoded = data.strip_prefix("data:audio/wav;base64,").unwrap_or(data);
    let bytes = STANDARD.decode(encoded).map_err(|_| "语音合成的音频编码无效")?;
    if bytes.len() > MAX_WAV_BYTES { return Err("语音合成音频超过大小上限".into()); }
    let pcm =
        parse_wav_to_pcm16mono16k(&bytes).map_err(|e| format!("解析语音合成音频失败: {e}"))?;
    let mut emitted = 0usize;
    for chunk in pcm.chunks(8000) {
        on_pcm(chunk).await?;
        emitted += chunk.len();
    }
    Ok(emitted)
}

/// Parse an RIFF/WAV byte stream into 16 kHz mono 16-bit little-endian PCM.
// A canonical mono 16 kHz PCM WAV can be played straight from the response.
// Other WAV layouts retain the full parser, channel conversion and resampler.
fn direct_pcm_length(header: &[u8]) -> Option<usize> {
    if header.get(..4)? != b"RIFF" || header.get(8..16)? != b"WAVEfmt " ||
        header.get(16..24)? != [16,0,0,0,1,0,1,0] || header.get(24..28)? != TARGET_RATE.to_le_bytes() ||
        header.get(32..36)? != [2,0,16,0] || header.get(36..40)? != b"data" { return None; }
    let n = u32::from_le_bytes(header.get(40..44)?.try_into().ok()?) as usize;
    (n != 0 && n % 2 == 0 && n <= MAX_WAV_BYTES - 44 &&
        u32::from_le_bytes(header[4..8].try_into().ok()?) as usize == n + 36).then_some(n)
}

async fn play_wav_response<F, Fut>(mut response: reqwest::Response, on_pcm: &mut F) -> Result<usize, String>
where F: FnMut(&[u8]) -> Fut, Fut: std::future::Future<Output=Result<(), String>> {
    let mut buffer = Vec::new();
    let mut expected = None;
    let mut total = 0usize;
    let mut emitted = 0usize;
    while let Some(chunk) = response.chunk().await.map_err(|e| model_issue::network(TTS,&e))? {
        if chunk.len() > MAX_WAV_BYTES - total { return Err("语音服务响应超过大小上限".into()); }
        let needs_header = total < 44;
        total += chunk.len();
        buffer.extend_from_slice(&chunk);
        if needs_header && buffer.len() >= 44 {
            expected = direct_pcm_length(&buffer);
            if expected.is_some() { buffer.drain(..44); }
        }
        if let Some(length) = expected {
            if emitted + buffer.len() > length { return Err("WAV 长度与响应不一致".into()); }
            let complete = buffer.len() & !1;
            for part in buffer[..complete].chunks(8000) { on_pcm(part).await?; }
            emitted += complete;
            buffer.drain(..complete);
        }
    }
    if let Some(length) = expected {
        return if emitted == length { Ok(emitted) } else { Err("WAV 数据块不完整".into()) };
    }
    let pcm = parse_wav_to_pcm16mono16k(&buffer)?;
    for part in pcm.chunks(8000) { on_pcm(part).await?; }
    Ok(pcm.len())
}

fn parse_wav_to_pcm16mono16k(bytes: &[u8]) -> Result<Vec<u8>, String> {
    if bytes.len() < 44 || &bytes[0..4] != b"RIFF" || &bytes[8..12] != b"WAVE" {
        return Err("响应不是 WAV 音频".into());
    }
    let mut format = None;
    let mut offset = 12usize;
    let mut data: Option<&[u8]> = None;
    while offset + 8 <= bytes.len() {
        let id = &bytes[offset..offset + 4];
        let size = u32::from_le_bytes([
            bytes[offset + 4],
            bytes[offset + 5],
            bytes[offset + 6],
            bytes[offset + 7],
        ]) as usize;
        let payload = offset + 8;
        if payload.saturating_add(size) > bytes.len() {
            return Err("WAV 数据块不完整".into());
        }
        let chunk = &bytes[payload..payload.saturating_add(size)];
        if id == b"fmt " {
            if chunk.len() < 16 || u16::from_le_bytes([chunk[0], chunk[1]]) != 1 {
                return Err("WAV 必须使用 PCM 编码".into());
            }
            format = Some((
                u16::from_le_bytes([chunk[2], chunk[3]]),
                u32::from_le_bytes([chunk[4], chunk[5], chunk[6], chunk[7]]),
                u16::from_le_bytes([chunk[14], chunk[15]]),
            ));
        } else if id == b"data" {
            data = Some(chunk);
        }
        offset = payload.saturating_add(size).saturating_add(size % 2);
    }
    let data = data.ok_or("WAV 缺少 data 块")?;
    let (channels, sample_rate, bits) = format.ok_or("WAV 缺少 fmt 块")?;
    if bits != 16 {
        return Err(format!("不支持的位深: {bits}"));
    }
    if !(8_000..=96_000).contains(&sample_rate) || !matches!(channels, 1 | 2) {
        return Err("WAV 采样率异常".into());
    }
    if data.len() % (2 * usize::from(channels)) != 0 {
        return Err("WAV 包含不完整的采样".into());
    }
    // Mono-ize (average channels), then linear-resample to 16 kHz.
    let samples: Vec<i16> = if channels == 1 {
        data.chunks_exact(2)
            .map(|b| i16::from_le_bytes([b[0], b[1]]))
            .collect()
    } else {
        let ch = channels as usize;
        let frames = data.len() / (2 * ch);
        (0..frames)
            .map(|i| {
                let sum: i32 = (0..ch)
                    .map(|c| {
                        i16::from_le_bytes([data[(i * ch + c) * 2], data[(i * ch + c) * 2 + 1]])
                            as i32
                    })
                    .sum();
                (sum / ch as i32) as i16
            })
            .collect()
    };
    if samples.is_empty() {
        return Err("音频内容为空".into());
    }
    let out = resample_linear(&samples, sample_rate, TARGET_RATE);
    if out.is_empty() {
        return Err("音频内容为空".into());
    }
    let mut v = Vec::with_capacity(out.len() * 2);
    for s in out {
        v.extend_from_slice(&s.to_le_bytes());
    }
    Ok(v)
}

fn resample_linear(src: &[i16], from: u32, to: u32) -> Vec<i16> {
    if from == to {
        return src.to_vec();
    }
    let ratio = from as f64 / to as f64;
    let n = (src.len() as f64 / ratio) as usize;
    (0..n)
        .map(|i| {
            let pos = i as f64 * ratio;
            let lo = pos.floor() as usize;
            let hi = (lo + 1).min(src.len() - 1);
            let frac = pos - lo as f64;
            let a = f64::from(src[lo]);
            let b = f64::from(src[hi]);
            (a + (b - a) * frac).round() as i16
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn model_issue_tts_rejects_business_json_on_wav_and_chat_endpoints_without_audio() {
        for suffix in ["/v1","/v1/chat/completions"] {
            let reply=json!({"base_resp":{"status_code":403,"status_msg":"private-key https://private.invalid"}});
            let (origin,server)=crate::voice_chat_settings::mock_speech_response("application/json",reply.to_string().into_bytes()).await;
            let mut cfg=DoubaoTtsConfig::new("private-key","","",1.0,"");
            cfg.tts_model="fixture".into();cfg.tts_base_url=format!("{origin}{suffix}");
            let error=synthesize_paced(&cfg,"private text",|_| std::future::ready(Err("unexpected PCM".into()))).await.unwrap_err();
            assert_eq!(error,model_issue::business(TTS,&json!(403)));server.await.unwrap();
        }
    }

    #[tokio::test]
    async fn custom_tts_decodes_wav_from_both_protocols() {
        use crate::voice_chat_settings::mock_speech_response;

        let wav = b"RIFF\x26\0\0\0WAVEfmt \x10\0\0\0\x01\0\x01\0\x80>\0\0\0}\0\0\x02\0\x10\0data\x02\0\0\0\x01\0";
        let chat_reply = json!({"choices": [{"message": {"audio": {"data": STANDARD.encode(wav)}}}]});
        for (suffix, route, content_type, response) in [
            ("/speech/v1", "/speech/v1/audio/speech", "audio/wav", wav.to_vec()),
            ("/speech/v1/chat/completions", "/speech/v1/chat/completions", "application/json", chat_reply.to_string().into_bytes()),
        ] {
            let (origin, request) = mock_speech_response(content_type, response).await;
            let mut cfg = DoubaoTtsConfig::new("speech-fixture", "", "", 1.0, "");
            cfg.tts_base_url = format!("{origin}{suffix}");
            cfg.tts_model = "fixture-tts".into();
            cfg.tts_voice = "fixture-voice".into();
            let mut pcm = Vec::new();
            assert_eq!(synthesize_paced(&cfg, "语音测试", |chunk| { pcm.extend_from_slice(chunk); std::future::ready(Ok(())) }).await.unwrap(), 2);
            assert_eq!(pcm, [1, 0]);
            let (headers, body) = request.await.unwrap();
            assert!(headers.starts_with(&format!("POST {route} HTTP/1.1")));
            assert!(headers.lines().filter_map(|line| line.split_once(':')).any(|(name, value)| {
                name.eq_ignore_ascii_case("authorization") && value.trim() == "Bearer speech-fixture"
            }));
            let payload: serde_json::Value = serde_json::from_slice(&body).unwrap();
            assert_eq!(payload["model"], "fixture-tts");
            if suffix.ends_with("/chat/completions") {
                assert_eq!(payload["messages"][0], json!({"role": "assistant", "content": "语音测试"}));
                assert_eq!(payload["audio"], json!({"format": "wav", "voice": "fixture-voice"}));
            } else {
                assert_eq!(payload["input"], "语音测试");
                assert_eq!(payload["response_format"], "wav");
            }
        }
    }

    #[tokio::test]
    async fn direct_pcm_starts_before_server_finishes_and_carries_split_sample() {
        use tokio::io::{AsyncReadExt, AsyncWriteExt};
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = listener.local_addr().unwrap();
        let (started, first_sample) = tokio::sync::oneshot::channel();
        let server = tokio::spawn(async move {
            let (mut stream, _) = listener.accept().await.unwrap();
            let mut request = [0; 2048];
            stream.read(&mut request).await.unwrap();
            stream.write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 48\r\nConnection: close\r\n\r\n").await.unwrap();
            stream.write_all(b"RIFF\x28\0\0\0WAVEfmt \x10\0\0\0\x01\0\x01\0\x80>\0\0\0}\0\0\x02\0\x10\0data\x04\0\0\0\x01\0\x02").await.unwrap();
            tokio::time::timeout(std::time::Duration::from_secs(5), first_sample).await.unwrap().unwrap();
            stream.write_all(&[0]).await.unwrap();
        });
        let response = reqwest::Client::builder().no_proxy().build().unwrap()
            .get(format!("http://{addr}")).send().await.unwrap();
        let mut started = Some(started);
        let mut samples = Vec::new();
        let n = play_wav_response(response, &mut |bytes| {
            samples.extend_from_slice(bytes);
            if let Some(signal) = started.take() { signal.send(()).unwrap(); }
            std::future::ready(Ok(()))
        }).await.unwrap();
        assert_eq!(n, 4);
        assert_eq!(samples, [1,0,2,0]);
        server.await.unwrap();
    }

    #[test]
    fn resamples_between_pcm_samples() {
        assert_eq!(
            resample_linear(&[0, 1000], 8000, 16000),
            [0, 500, 1000, 1000]
        );
    }

    #[test]
    fn rejects_non_pcm_format_and_partial_samples() {
        let mut wav = b"RIFF&\0\0\0WAVEfmt \x10\0\0\0\x01\0\x01\0\x80>\0\0\0}\0\0\x02\0\x10\0data\x02\0\0\0\x01\0".to_vec();
        assert_eq!(parse_wav_to_pcm16mono16k(&wav).unwrap(), [1, 0]);
        wav[20] = 3;
        assert!(parse_wav_to_pcm16mono16k(&wav).is_err());
        wav[20] = 1;
        wav.pop();
        assert!(parse_wav_to_pcm16mono16k(&wav).is_err());
    }
}
