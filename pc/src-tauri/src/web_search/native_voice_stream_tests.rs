use super::*;

const DONE: &str = "data: [DONE]\r\n\r\n";

fn frame(value: Value) -> String {
    format!("data: {value}\r\n\r\n")
}

fn citation() -> Value {
    json!({"type":"url_citation","url":"https://example.org/forecast"})
}

fn chat_text(text: &str) -> String {
    frame(json!({"choices":[{"index":0,"delta":{"content":text,"reasoning_content":"内部推理"}}]}))
}

fn stop() -> String {
    frame(json!({"choices":[{"index":0,"delta":{},"finish_reason":"stop"}]}))
}

fn completed(text: &str, annotations: Value) -> String {
    frame(
        json!({"type":"response.completed","response":{"status":"completed","output":[
            {"type":"reasoning","summary":[{"text":"内部推理"}]},
            {"type":"web_search_call","summary":"工具摘要"},
            {"type":"message","role":"assistant","content":[
                {"type":"output_text","text":text,"annotations":annotations}
            ]}
        ]}}),
    )
}

// Exercise the production reader across arbitrary HTTP and UTF-8 boundaries.
async fn replay(
    protocol: Protocol,
    wire: String,
    width: usize,
) -> (Result<SearchResult, String>, String) {
    let mut spoken = String::new();
    let chunks = futures_util::stream::iter(wire.as_bytes().chunks(width).map(Ok::<_, ()>));
    let result = read_answer(chunks, Provider::DeepSeek, protocol, |text| {
        spoken.push_str(text)
    })
    .await;
    (result, spoken)
}

#[tokio::test]
async fn chat_consumes_late_citations_and_usage_after_stop_across_chunks() {
    for annotations in [
        json!({"annotations":[citation()]}),
        json!({"server_tool_calls":[{"calls":[{"sources":[{"type":"url","url":"https://example.org/forecast"}]}]}]}),
    ] {
        let late = frame(json!({"choices":[{"index":0,"delta":annotations}]}));
        let usage = frame(json!({"choices":[],"usage":{"total_tokens":15}}));
        let wire = [chat_text("晴。"), stop(), late, usage, DONE.into()].concat();
        for width in [1, 13, wire.len()] {
            let (result, spoken) = replay(Protocol::Chat, wire.clone(), width).await;
            let result = result.unwrap();
            assert_eq!(spoken, "晴。");
            assert_eq!(result.summary, spoken);
            assert_eq!(result.sources.len(), 1);
        }
    }
}

#[test]
fn speech_waits_for_a_public_source_then_streams_before_completion() {
    let mut out = Output::default();
    let mut speech = String::new();
    for delta in [
        json!({"content":"今天","annotations":[{"type":"url_citation","url":"http://localhost/weather"}]}),
        json!({"reasoning_content":"搜索中"}),
    ] {
        out.event(
            Protocol::Chat,
            &json!({"choices":[{"delta":delta}]}),
            &mut |s| speech += s,
        )
        .unwrap();
    }
    assert!(speech.is_empty());
    out.event(
        Protocol::Chat,
        &json!({"choices":[{"delta":{"annotations":[citation()]}}]}),
        &mut |s| speech += s,
    )
    .unwrap();
    assert_eq!(speech, "今天");
    out.event(
        Protocol::Chat,
        &json!({"choices":[{"delta":{"content":"晴。"}}]}),
        &mut |s| speech += s,
    )
    .unwrap();
    assert_eq!(speech, "今天晴。");
    assert!(!out.complete);
}

#[tokio::test]
async fn responses_snapshot_supplies_final_text_without_repeating_deltas_or_tool_markup() {
    for prefix in ["", "今天", "今天晴。"] {
        let wire = [
            frame(json!({"type":"response.output_text.annotation.added","annotation":citation()})),
            frame(json!({"type":"response.output_text.delta","delta":prefix})),
            completed("今天晴。<tool_call>隐藏", json!([citation()])),
        ]
        .concat();
        let (result, spoken) = replay(Protocol::Responses, wire, 1).await;
        let result = result.unwrap();
        assert_eq!(result.summary, "今天晴。");
        assert_eq!(spoken, result.summary);
        assert_eq!(result.sources.len(), 1);
    }
    let (result, spoken) = replay(
        Protocol::Responses,
        completed("只有快照。", json!([citation()])),
        7,
    )
    .await;
    assert_eq!(result.unwrap().summary, "只有快照。");
    assert_eq!(spoken, "只有快照。");
}

#[tokio::test]
async fn metadata_only_completion_keeps_streamed_answer_and_conflicting_snapshot_fails() {
    let prefix = [
        frame(
            json!({"type":"response.output_item.done","item":{"type":"web_search_call","results":[
            {"type":"web_search_result","url":"https://example.org/forecast"}]}}),
        ),
        frame(json!({"type":"response.output_text.delta","delta":"阴。"})),
    ]
    .concat();
    let metadata =
        frame(json!({"type":"response.completed","response":{"status":"completed","usage":{}}}));
    let (result, spoken) = replay(Protocol::Responses, prefix.clone() + &metadata, 17).await;
    assert_eq!(result.unwrap().summary, spoken);
    assert_eq!(spoken, "阴。");
    let (result, spoken) = replay(
        Protocol::Responses,
        prefix + &completed("晴。", json!([])),
        9,
    )
    .await;
    assert!(result.unwrap_err().contains("不一致"));
    assert_eq!(spoken, "阴。");
}

#[tokio::test]
async fn missing_sources_and_missing_answer_report_distinct_errors_without_speech() {
    let private = json!({"type":"url_citation","url":"http://127.0.0.1/x"});
    for wire in [
        completed("晴。", json!([])),
        completed("正文 https://example.org", json!([private])),
    ] {
        let (result, spoken) = replay(Protocol::Responses, wire, 27).await;
        assert_eq!(result.unwrap_err(), NO_SOURCES);
        assert!(spoken.is_empty());
    }
    let (result, spoken) =
        replay(Protocol::Responses, completed(" ", json!([citation()])), 19).await;
    assert_eq!(result.unwrap_err(), NO_TEXT);
    assert!(spoken.trim().is_empty());
    let (result, spoken) = replay(Protocol::Chat, chat_text("晴。") + &stop() + DONE, 11).await;
    assert_eq!(result.unwrap_err(), NO_SOURCES);
    assert!(spoken.is_empty());
}

#[tokio::test]
async fn early_eof_transport_failure_and_done_without_a_model_end_are_incomplete() {
    let chat = frame(json!({"choices":[{"delta":{"content":"半句","annotations":[citation()]}}]}));
    for tail in [String::new(), stop(), DONE.into(), stop() + "data: [DO"] {
        let (result, _) = replay(Protocol::Chat, chat.clone() + &tail, 5).await;
        assert_eq!(result.unwrap_err(), UNFINISHED);
    }
    let (result, _) = replay(Protocol::Responses, DONE.into(), 8).await;
    assert_eq!(result.unwrap_err(), UNFINISHED);
    let failed = futures_util::stream::iter([Ok(chat), Ok(stop()), Err(())]);
    assert_eq!(
        read_answer(failed, Provider::Mimo, Protocol::Chat, |_| {})
            .await
            .unwrap_err(),
        UNFINISHED
    );
}

#[tokio::test]
async fn explicit_failure_after_stop_or_in_a_completed_envelope_is_not_success() {
    let error = frame(json!({"error":{"message":"private service detail"}}));
    for (protocol, wire) in [
        (Protocol::Chat, chat_text("晴。") + &stop() + &error),
        (
            Protocol::Chat,
            frame(json!({"choices":[{"finish_reason":"length"}]})),
        ),
        (
            Protocol::Responses,
            frame(json!({"type":"response.incomplete"})),
        ),
        (
            Protocol::Responses,
            frame(json!({"type":"response.failed"})),
        ),
        (
            Protocol::Responses,
            frame(json!({"type":"response.completed","response":{"status":"incomplete"}})),
        ),
    ] {
        let (result, _) = replay(protocol, wire, 21).await;
        assert_eq!(result.unwrap_err(), "联网服务未完整生成回答");
    }
}

#[tokio::test]
async fn sse_event_fields_comments_and_multiline_data_do_not_change_the_answer() {
    let wire = concat!(
        ": heartbeat\n\n",
        "event: response.completed\n",
        "data: {\"type\":\"response.completed\",\n",
        "data: \"response\":{\"status\":\"completed\",\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\n",
        "data: \"content\":[{\"type\":\"output_text\",\"text\":\"晴。\",\"annotations\":[",
        "{\"type\":\"url_citation\",\"url\":\"https://example.org/\"}]}]}]}}\n\n"
    );
    let (result, spoken) = replay(Protocol::Responses, wire.into(), 1).await;
    assert_eq!(result.unwrap().summary, "晴。");
    assert_eq!(spoken, "晴。");
}

#[test]
fn final_metadata_contains_only_fixed_provider_state_and_counts() {
    let mut out = Output::default();
    out.append_text(" 正文。<tool_call>private");
    collect_sources(&citation(), 0, &mut out.sources);
    for (kind, label) in [
        (Provider::Mimo, "mimo"),
        (Provider::Doubao, "doubao"),
        (Provider::DeepSeek, "deepseek"),
    ] {
        for complete in [false, true] {
            out.complete = complete;
            assert_eq!(
                out.final_metadata(kind),
                json!({
                    "stage":label, "state":if complete {"complete"} else {"unfinished"}, "count":1, "chars":3
                })
            );
        }
    }
    assert_eq!(
        Output::default().final_metadata(Provider::Mimo),
        json!({"stage":"mimo","state":"unfinished","count":0,"chars":0})
    );
}
