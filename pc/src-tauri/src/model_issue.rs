//! Public model failures. Only local hints, typed status numbers and enumerated
//! business codes cross the UI boundary. Never interpolate a provider message.
use serde_json::Value;

pub(crate) const ASR: &str = "[ASR]";
pub(crate) const TTS: &str = "[TTS]";
pub(crate) const LLM: &str = "[LLM]";
pub(crate) const CREDENTIALS: &str = "[CREDENTIALS]";

pub(crate) fn local(source: &str, message: String) -> String {
    if message.starts_with(source) || message.starts_with(CREDENTIALS) {
        message
    } else {
        format!("{source} {message}")
    }
}

pub(crate) fn http(source: &str, status: u16, websocket: bool) -> String {
    let hint = match status {
        401 => "鉴权失败，请核对当前服务的 API Key",
        403 => "权限不足，请检查服务授权和资源权限",
        404 => "请核对接口路径、模型或资源 ID",
        407 => "代理需要身份验证，请检查本机网络代理配置",
        402 | 429 => "请检查服务额度、余额和请求频率",
        400 | 422 => "请检查模型、资源及请求参数",
        408 | 504 => "服务请求超时，请稍后重试",
        500..=599 => "服务暂时不可用，请稍后重试",
        _ => "请检查服务配置及接口兼容性",
    };
    let protocol = if websocket { "WebSocket HTTP" } else { "HTTP" };
    format!("{source} {protocol} {status} {hint}")
}

pub(crate) fn client(
    source: &str,
    timeout: std::time::Duration,
) -> Result<reqwest::Client, String> {
    crate::llm_network::client(timeout).map_err(|e| format!("{source} [NETWORK] {e}"))
}

pub(crate) fn network(source: &str, error: &reqwest::Error) -> String {
    if error.is_builder() {
        return format!("{source} 请求配置无效，请检查接口地址及 API Key 是否包含无效字符");
    }
    format!(
        "{source} [NETWORK] {}",
        crate::llm_network::connection_error(error)
    )
}

pub(crate) fn websocket(source: &str, error: tokio_tungstenite::tungstenite::Error) -> String {
    use tokio_tungstenite::tungstenite::{error::ProtocolError, Error};
    match error {
        Error::Http(reply) => http(source, reply.status().as_u16(), true),
        Error::AlreadyClosed
        | Error::ConnectionClosed
        | Error::Protocol(ProtocolError::ResetWithoutClosingHandshake) => closed(source, None),
        Error::Io(e) if e.kind() == std::io::ErrorKind::TimedOut => {
            format!("{source} [NETWORK] WebSocket 超时，请检查网络")
        }
        Error::Io(_) => format!("{source} [NETWORK] WebSocket 连接中断，请检查网络"),
        Error::Tls(_) => format!("{source} [NETWORK] TLS 连接失败，请检查证书及网络设置"),
        Error::Capacity(_) | Error::WriteBufferFull(_) => {
            format!("{source} WebSocket 帧超过容量限制")
        }
        _ => format!("{source} WebSocket 协议无效，请检查服务和接口版本"),
    }
}

pub(crate) fn closed(source: &str, code: Option<u16>) -> String {
    match code {
        Some(code) => format!("{source} WS {code} 服务在操作完成前关闭连接，请检查服务权限或重试"),
        None => format!("{source} [NETWORK] WebSocket 连接已关闭，请重试"),
    }
}

pub(crate) fn business(source: &str, code: &Value) -> String {
    let number = code.as_i64().or_else(|| code.as_str()?.parse().ok());
    let numeric = match number {
        Some(
            n @ (400 | 401 | 402 | 403 | 404 | 429 | 500 | 502 | 503 | 504 | 45000000 | 45000001
            | 45000002 | 45000081 | 55000000 | 55000031),
        ) => Some(n),
        _ => None,
    };
    let symbol = match code.as_str() {
        Some(
            s @ ("invalid_api_key"
            | "permission_denied"
            | "model_not_found"
            | "insufficient_quota"
            | "rate_limit_exceeded"
            | "invalid_request_error"),
        ) => s,
        _ => "unknown",
    };
    let public = numeric
        .map(|n| n.to_string())
        .unwrap_or_else(|| symbol.into());
    let hint = match public.as_str() {
        "45000081" => "语音服务等待音频超时，请重新说一次",
        "401" | "invalid_api_key" => "请核对当前服务的 API Key",
        "403" | "permission_denied" => "请检查模型或资源授权",
        "429" | "402" | "insufficient_quota" | "rate_limit_exceeded" => {
            "请检查额度、余额及限流设置"
        }
        "404" | "model_not_found" => "请检查模型名称及可用权限",
        _ => "服务未完成请求，请检查模型和资源配置或稍后重试",
    };
    format!("{source} code={public} {hint}")
}

pub(crate) fn check_reply(source: &str, reply: &Value) -> Result<(), String> {
    if let Some(error) = reply.get("error").filter(|e| !e.is_null()) {
        return Err(business(
            source,
            error
                .get("code")
                .filter(|value| !value.is_null())
                .or_else(|| error.get("type"))
                .unwrap_or(&Value::Null),
        ));
    }
    for code in [
        reply.get("code"),
        reply.get("status_code"),
        reply.pointer("/base_resp/status_code"),
    ]
    .into_iter()
    .flatten()
    {
        if code != 0 && code != "0" && code != 200 && code != "200" {
            return Err(business(source, code));
        }
    }
    Ok(())
}

pub(crate) async fn read_response(
    source: &str,
    mut response: reqwest::Response,
    limit: usize,
) -> Result<Vec<u8>, String> {
    if !response.status().is_success() {
        return Err(http(source, response.status().as_u16(), false));
    }
    let mut data = Vec::new();
    while let Some(chunk) = response.chunk().await.map_err(|e| network(source, &e))? {
        if chunk.len() > limit.saturating_sub(data.len()) {
            return Err(format!("{source} 服务响应超过大小上限"));
        }
        data.extend_from_slice(&chunk);
    }
    Ok(data)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[tokio::test]
    async fn model_issue_invalid_header_is_configuration_not_connectivity() {
        let error = reqwest::Client::new()
            .get("http://127.0.0.1:1/private-path")
            .bearer_auth("private-key\ninvalid")
            .send()
            .await
            .unwrap_err();
        assert!(error.is_builder());
        let message = network(LLM, &error);
        assert!(message.starts_with("[LLM] 请求配置无效"));
        assert!(!message.contains("private") && !message.contains("[NETWORK]"));
    }

    #[tokio::test]
    async fn model_issue_network_truncation_keeps_source_and_drops_url_and_received_text() {
        use tokio::io::{AsyncReadExt, AsyncWriteExt};
        let client = reqwest::Client::builder().no_proxy().build().unwrap();
        for source in [ASR, TTS, LLM] {
            let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
            let url = format!(
                "http://{}/private-path?key=private-key",
                listener.local_addr().unwrap()
            );
            let server = tokio::spawn(async move {
                let (mut socket, _) = listener.accept().await.unwrap();
                let mut request = [0; 2048];
                socket.read(&mut request).await.unwrap();
                socket.write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\nConnection: close\r\n\r\nprivate-body").await.unwrap();
            });
            let response = client.get(url).send().await.unwrap();
            let error = read_response(source, response, 1024).await.unwrap_err();
            assert!(error.starts_with(&format!("{source} [NETWORK] ")));
            assert!(!error.contains("private") && !error.contains("127.0.0.1"));
            server.await.unwrap();
        }
    }
}
