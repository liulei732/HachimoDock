//! The conversation stays on the configured model's official endpoint.
use super::*;

pub(crate) fn route(cfg: &LlmConfig) -> Option<Provider> {
    let kind = provider(cfg)?;
    (cfg.search.mode != Mode::DeepSeek && !cfg.api_key.trim().is_empty()).then_some(kind)
}

fn native_request(
    client: &reqwest::Client,
    cfg: &LlmConfig,
    kind: Provider,
    messages: Value,
) -> (Protocol, reqwest::RequestBuilder) {
    let (url, body) = match kind {
        Provider::DeepSeek if cfg.model == "deepseek-v4-pro" => (
            "https://api.deepseek.com/v1/responses".into(),
            json!({"model":cfg.model,"input":messages,"tools":[{"type":"web_search"}],"stream":true}),
        ),
        Provider::DeepSeek => (
            cfg.completions_url(),
            json!({"model":cfg.model,"messages":messages,"tools":[{"type":"deepseek_web_search"}],
                "thinking":{"type":"disabled"},"stream":true}),
        ),
        Provider::Mimo | Provider::Doubao => {
            let (url, mut body) = request(kind, cfg, "");
            // Dedicated evidence requests require search. In realtime turns
            // the model decides whether to use its server-side tool.
            body["tool_choice"] = "auto".into();
            if kind == Provider::Mimo {
                body["tools"][0]["force_search"] = false.into();
                body["messages"] = messages;
            } else {
                body["input"] = messages;
            }
            body["stream"] = true.into();
            (url.into(), body)
        }
    };
    let protocol = if body.get("input").is_some() {
        Protocol::Responses
    } else {
        Protocol::Chat
    };
    (
        protocol,
        authorize(client.post(&url).json(&body), &url, cfg.api_key.trim()),
    )
}

#[derive(Clone, Copy, PartialEq)]
enum Protocol {
    Chat,
    Responses,
}

const UNFINISHED: &str = "联网回答接收中断，未收到完整结束标记";
const NO_SOURCES: &str = "联网回答缺少可核验的公开来源";
const NO_TEXT: &str = "联网服务已结束，但没有返回回答正文";

#[derive(Default)]
struct Output {
    sources: Vec<Source>,
    text: String,
    verified_text_end: usize,
    tool_markup: bool,
    delivered: usize,
    choice_stopped: bool,
    complete: bool,
}

impl Output {
    // Some models emit an XML tool invocation as ordinary assistant text.
    // Hold a partial opening tag across chunks so it never reaches subtitles or TTS.
    fn append_text(&mut self, delta: &str) {
        if self.tool_markup {
            return;
        }
        self.text.push_str(delta);
        const MARKERS: [&str; 3] = ["<tool_call", "<function=", "<parameter="];
        let pending = &self.text[self.verified_text_end..];
        for (offset, _) in pending.match_indices('<') {
            let tail = &pending[offset..];
            if MARKERS.iter().any(|marker| tail.starts_with(marker)) {
                self.verified_text_end += offset;
                self.text.truncate(self.verified_text_end);
                self.tool_markup = true;
                return;
            }
            if MARKERS.iter().any(|marker| marker.starts_with(tail)) {
                self.verified_text_end += offset;
                return;
            }
        }
        self.verified_text_end = self.text.len();
    }
    fn completed_snapshot(&mut self, items: &[Value]) -> Result<(), String> {
        // The completed output is a snapshot, not one more delta. Reapply
        // markup filtering before reconciling it with text already spoken.
        let mut snapshot = Output::default();
        for item in items {
            if item["type"] == "message" && item["role"] == "assistant" {
                for part in item["content"].as_array().into_iter().flatten() {
                    if part["type"] == "output_text" {
                        if let Some(text) = part["text"].as_str() {
                            snapshot.append_text(text);
                        }
                    }
                }
            }
        }
        if !snapshot.text[..snapshot.verified_text_end].starts_with(&self.text[..self.delivered]) {
            return Err("联网回答完成快照与已播报正文不一致".into());
        }
        self.text = snapshot.text;
        self.verified_text_end = snapshot.verified_text_end;
        self.tool_markup = snapshot.tool_markup;
        Ok(())
    }

    fn event(
        &mut self,
        protocol: Protocol,
        event: &Value,
        emit: &mut impl FnMut(&str),
    ) -> Result<(), String> {
        if !event["error"].is_null()
            || matches!(
                event["type"].as_str(),
                Some("error" | "response.failed" | "response.incomplete")
            )
        {
            return Err("联网服务未完整生成回答".into());
        }
        match protocol {
            Protocol::Chat => {
                let choice = &event["choices"][0];
                if let Some(reason) = choice["finish_reason"].as_str() {
                    if reason != "stop" {
                        return Err("联网服务未完整生成回答".into());
                    }
                    self.choice_stopped = true;
                }
                if let Some(delta) = choice["delta"]["content"].as_str() {
                    self.append_text(delta);
                }
            }
            Protocol::Responses => match event["type"].as_str() {
                Some("response.output_text.delta") => {
                    if let Some(delta) = event["delta"].as_str() {
                        self.append_text(delta);
                    }
                }
                Some("response.completed") => {
                    let response = &event["response"];
                    if response["status"] != "completed" || !response["error"].is_null() {
                        return Err("联网服务未完整生成回答".into());
                    }
                    if let Some(items) = response["output"].as_array() {
                        self.completed_snapshot(items)?;
                    }
                    self.complete = true;
                }
                _ => {}
            },
        }
        collect_sources(event, 0, &mut self.sources);
        if !self.sources.is_empty() && self.verified_text_end > self.delivered {
            emit(&self.text[self.delivered..self.verified_text_end]);
            self.delivered = self.verified_text_end;
        }
        Ok(())
    }

    fn final_metadata(&self, kind: Provider) -> Value {
        // Use the realtime logger's allowlisted fields. Provider is derived
        // from the selected adapter, never from a model, host or response.
        let provider = match kind {
            Provider::Mimo => "mimo",
            Provider::Doubao => "doubao",
            Provider::DeepSeek => "deepseek",
        };
        json!({
            "stage": provider,
            "state": if self.complete { "complete" } else { "unfinished" },
            "count": self.sources.len(),
            "chars": self.text[..self.verified_text_end].trim().chars().count(),
        })
    }

    fn finish(
        mut self,
        kind: Provider,
        received: Result<(), String>,
    ) -> Result<SearchResult, String> {
        crate::realtime_chat_log::record("", "native_search_final", self.final_metadata(kind));
        received?;
        if !self.complete {
            return Err(UNFINISHED.into());
        }
        self.text.truncate(self.verified_text_end);
        if self.text.trim().is_empty() {
            return Err(NO_TEXT.into());
        }
        if self.sources.is_empty() {
            return Err(NO_SOURCES.into());
        }
        Ok(SearchResult {
            provider: kind.label().into(),
            summary: self.text,
            sources: self.sources,
            retrieved_at: chrono::Utc::now().to_rfc3339(),
        })
    }
}

async fn read_answer<B: AsRef<[u8]>, E>(
    stream: impl futures_util::Stream<Item = Result<B, E>> + Unpin,
    kind: Provider,
    protocol: Protocol,
    mut emit: impl FnMut(&str),
) -> Result<SearchResult, String> {
    let mut output = Output::default();
    let received = read_events(stream, protocol, &mut output, &mut emit).await;
    output.finish(kind, received)
}

// Frame by SSE blank lines, independently of HTTP chunk and UTF-8 boundaries.
async fn read_events<B: AsRef<[u8]>, E>(
    mut stream: impl futures_util::Stream<Item = Result<B, E>> + Unpin,
    protocol: Protocol,
    output: &mut Output,
    emit: &mut impl FnMut(&str),
) -> Result<(), String> {
    let mut line = Vec::new();
    let mut data = Vec::new();
    let mut total = 0;
    while let Some(chunk) = stream.next().await {
        let chunk = chunk.map_err(|_| UNFINISHED)?;
        let bytes = chunk.as_ref();
        total += bytes.len();
        if total > MAX_RESPONSE {
            return Err("联网回答超过大小限制".into());
        }
        for piece in bytes.split_inclusive(|&b| b == b'\n') {
            let Some(end) = piece.strip_suffix(b"\n") else {
                line.extend_from_slice(piece);
                continue;
            };
            line.extend_from_slice(end);
            let field = line.strip_suffix(b"\r").unwrap_or(&line);
            if let Some(value) = field.strip_prefix(b"data:") {
                data.extend_from_slice(value.strip_prefix(b" ").unwrap_or(value));
                data.push(b'\n');
            } else if field.is_empty() && !data.is_empty() {
                let payload = std::str::from_utf8(&data)
                    .map_err(|_| "联网回答编码无效")?
                    .trim();
                if payload == "[DONE]" {
                    // Chat stop finishes one choice. Citation/usage frames can
                    // still arrive before the transport sentinel.
                    output.complete = protocol == Protocol::Chat && output.choice_stopped;
                    return Ok(());
                }
                if !payload.is_empty() {
                    let event =
                        serde_json::from_str(payload).map_err(|_| "联网回答事件格式异常")?;
                    output.event(protocol, &event, emit)?;
                    if output.complete {
                        return Ok(());
                    }
                }
                data.clear();
            }
            line.clear();
        }
    }
    Ok(())
}

pub(crate) async fn answer(
    cfg: &LlmConfig,
    kind: Provider,
    query: &str,
    messages: Value,
    emit: impl FnMut(&str),
) -> Result<SearchResult, String> {
    check_query(cfg, query)?;
    let client = crate::llm_network::client(DEADLINE)?;
    let (protocol, post) = native_request(&client, cfg, kind, messages);
    let response = post
        .send()
        .await
        .map_err(|e| crate::llm_network::connection_error(&e))?;
    if !response.status().is_success() {
        return Err(format!(
            "{}原生联网请求失败（HTTP {}），请检查模型、联网工具权限与额度",
            kind.label(),
            response.status().as_u16()
        ));
    }
    read_answer(response.bytes_stream(), kind, protocol, emit).await
}

#[cfg(test)]
pub(super) mod tests {
    use super::*;

    /// Exercise the production request builder, SSE decoder and acceptance rules exactly once.
    pub(in crate::web_search) async fn weather_receipt(cfg: &LlmConfig, kind: Provider, messages: Value) -> Value {
        let client = crate::llm_network::client(DEADLINE).expect("client unavailable");
        let (protocol, post) = native_request(&client, cfg, kind, messages);
        let start = std::time::Instant::now();
        let mut receipt = json!({"provider":kind.label(),"protocol":if protocol == Protocol::Chat {"chat"} else {"responses"},
            "http":null,"media":null,"publicSources":null,"hasText":null,"complete":null,"accepted":false});
        match post.send().await {
            Ok(response) => {
                let mime = response.headers().get(reqwest::header::CONTENT_TYPE).and_then(|h| h.to_str().ok())
                    .unwrap_or("").split(';').next().unwrap_or("").trim();
                receipt["http"] = response.status().as_u16().into();
                receipt["media"] = if mime.eq_ignore_ascii_case("text/event-stream") { "sse" }
                    else if mime.eq_ignore_ascii_case("application/json") { "json" } else { "other" }.into();
                if response.status().is_success() {
                    let mut output = Output::default();
                    let mut first_sentence_ms = None;
                    let result = read_events(response.bytes_stream(),protocol,&mut output,&mut |delta| {
                        if first_sentence_ms.is_none() && delta.contains(['。','！','？','!','?']) {
                            first_sentence_ms = Some(start.elapsed().as_millis() as u64);
                        }
                    }).await;
                    receipt["firstVerifiedSentenceMs"] = json!(first_sentence_ms);
                    receipt["publicSources"] = output.sources.len().into();
                    receipt["hasText"] = (!output.text[..output.verified_text_end].trim().is_empty()).into();
                    receipt["complete"] = output.complete.into();
                    receipt["choiceStop"] = output.choice_stopped.into();
                    receipt["toolMarkup"] = output.tool_markup.into();
                    receipt["streamReadOk"] = result.is_ok().into();
                    receipt["accepted"] = output.finish(kind,result).is_ok().into();
                } else if receipt["media"] == "json" {
                    // Classify API rejections without logging response text or identifiers.
                    let mut body = Vec::new();
                    let mut response = response;
                    while let Ok(Some(chunk)) = response.chunk().await {
                        if body.len() + chunk.len() > 16 * 1024 { break; }
                        body.extend_from_slice(&chunk);
                    }
                    if let Ok(value) = serde_json::from_slice::<Value>(&body) {
                        let msg = value["error"]["message"].as_str().unwrap_or("").to_ascii_lowercase();
                        receipt["errorMentionsModel"] = msg.contains("model").into();
                        receipt["errorMentionsTool"] = msg.contains("tool").into();
                        receipt["errorMentionsPermission"] = (msg.contains("permission") || msg.contains("access")).into();
                    }
                }
            }
            Err(e) => receipt["transport"] = if e.is_timeout() {"timeout"} else if e.is_connect() {"connect_failed"} else {"send_failed"}.into(),
        }
        receipt["elapsedMs"] = (start.elapsed().as_millis() as u64).into();
        receipt
    }

    #[test]
    fn native_models_get_the_conversation_and_only_their_server_tool() {
        let messages = json!([{"role":"system","content":"宠物人设"},{"role":"user","content":"上一轮"},{"role":"user","content":"追问"}]);
        for (id, field, tool) in [
            ("doubao", "input", "web_search"),
            ("mimo", "messages", "web_search"),
            ("deepseek", "messages", "deepseek_web_search"),
        ] {
            let preset = crate::voice_chat_settings::llm_preset(id).unwrap();
            let mut cfg = LlmConfig {
                base_url: preset.base_url.into(),
                model: preset.model.into(),
                web_search: true,
                api_key: "fixture".into(),
                search: Default::default(),
            };
            let kind = route(&cfg).unwrap();
            let (_, post) = native_request(&reqwest::Client::new(), &cfg, kind, messages.clone());
            let post = post.build().unwrap();
            assert_eq!(post.method(), reqwest::Method::POST);
            assert_ne!(post.url().as_str(), DEEPSEEK_SEARCH_URL);
            let body: Value =
                serde_json::from_slice(post.body().unwrap().as_bytes().unwrap()).unwrap();
            if kind != Provider::DeepSeek {
                assert_eq!(body["tool_choice"], "auto");
                let (_, dedicated) = request(kind, &cfg, "公开查询");
                if kind == Provider::Mimo {
                    assert_eq!(body["tools"][0]["force_search"], false);
                    assert_eq!(dedicated["tools"][0]["force_search"], true);
                } else {
                    assert_eq!(dedicated["tool_choice"]["type"], "web_search");
                }
            }
            assert_eq!(body[field], messages);
            assert_eq!(body["model"], cfg.model);
            assert_eq!(body["stream"], true);
            assert_eq!(body["tools"].as_array().unwrap().len(), 1);
            assert_eq!(body["tools"][0]["type"], tool);
            cfg.search.mode = Mode::DeepSeek;
            assert!(route(&cfg).is_none());
        }
    }

    #[test]
    fn split_model_tool_markup_never_reaches_speech_or_saved_answer() {
        let mut output = Output::default();
        let mut spoken = String::new();
        for part in [
            "船长，2 <",
            " 3。<",
            "tool_",
            "call><function=web_search>",
            "不应朗读",
        ] {
            output.event(Protocol::Chat, &json!({"choices":[{"delta":{"annotations":[{"type":"url_citation","url":"https://example.org/article"}],"content":part}}]}), &mut |s|spoken+=s).unwrap();
        }
        assert_eq!(spoken, "船长，2 < 3。");
        assert_eq!(output.text, spoken);
    }
}

#[cfg(test)]
#[path = "native_voice_stream_tests.rs"]
mod stream_tests;
