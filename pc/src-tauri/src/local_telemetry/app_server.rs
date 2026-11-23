//! Read-only quota RPC through the documented Codex app-server API.
//! No threads/turns, raw HTTP endpoints, caller-selected programs or token RPCs.
use serde_json::{json, Value};
use std::{
    io::{BufRead, BufReader, Read, Write},
    path::Path,
    process::{Child, Command, Stdio},
    sync::mpsc,
    time::{Duration, Instant},
};

struct Process(Child);
impl Drop for Process {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}

/// Parse only the Codex bucket. Missing windows remain missing, never 100%.
pub(super) fn normalize(account: &Value, response: &Value, email: &str) -> Result<Value, String> {
    if account["type"] != "chatgpt" || account["email"].as_str() != Some(email) {
        return Err("账号身份不匹配".into());
    }
    let limits = if let Some(map) = response.get("rateLimitsByLimitId").filter(|v| !v.is_null()) {
        map.get("codex").ok_or("未返回 Codex 额度")?
    } else {
        &response["rateLimits"]
    };
    if !limits.is_object()
        || limits
            .get("limitId")
            .is_some_and(|v| !v.is_null() && v != "codex")
    {
        return Err("未返回 Codex 额度".into());
    }
    let plan = account.get("planType").and_then(Value::as_str).map(|p| {
        if !p.is_empty()
            && p.len() <= 32
            && p.bytes()
                .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_')
        {
            p
        } else {
            "unknown"
        }
    });
    let mut result = json!({"limit_id":"codex", "plan_type":plan});
    for key in ["primary", "secondary"] {
        let w = &limits[key];
        result[key] = json!({"used_percent":w["usedPercent"],
            "window_minutes":w["windowDurationMins"], "resets_at":w["resetsAt"]});
    }
    Ok(result)
}

fn write(child: &mut Child, message: Value) -> Result<(), String> {
    let input = child.stdin.as_mut().ok_or("额度工具已退出")?;
    input
        .write_all(
            serde_json::to_string(&message)
                .map_err(|_| "额度请求无效")?
                .as_bytes(),
        )
        .and_then(|_| input.write_all(b"\n"))
        .and_then(|_| input.flush())
        .map_err(|_| "额度工具已退出".into())
}

fn receive(rx: &mpsc::Receiver<Value>, id: u64, deadline: Instant) -> Result<Value, String> {
    for _ in 0..128 {
        let remaining = deadline
            .checked_duration_since(Instant::now())
            .ok_or("额度查询超时")?;
        let value = rx
            .recv_timeout(remaining)
            .map_err(|_| "额度工具无响应，请检查 Codex 版本")?;
        if value["id"] == id {
            // Upstream errors may contain sensitive diagnostics; never forward them.
            if value.get("error").is_some() {
                return Err("额度查询失败，请检查授权或 Codex 版本".into());
            }
            return value.get("result").cloned().ok_or("额度响应无效".into());
        }
    }
    Err("额度工具输出过多".into())
}

pub(super) fn read(home: &Path, email: &str) -> Result<Value, String> {
    let binary = super::manager::executable()?;
    let mut command = Command::new(binary);
    command
        .args(["app-server", "-c", "cli_auth_credentials_store=\"file\""])
        .env("CODEX_HOME", home)
        .env_remove("OPENAI_API_KEY")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(0x08000000);
    }
    let mut child = Process(command.spawn().map_err(|_| "无法启动 Codex 额度工具")?);
    let output = child.0.stdout.take().ok_or("额度工具无输出")?;
    let (tx, rx) = mpsc::sync_channel(8);
    std::thread::spawn(move || {
        let mut reader = BufReader::new(output);
        // Bound both line length and total messages, including notifications.
        for _ in 0..128 {
            let mut line = Vec::new();
            if !matches!(Read::take(&mut reader, 65537).read_until(b'\n', &mut line), Ok(n) if n>0)
                || line.len() > 65536
            {
                break;
            }
            let Ok(value) = serde_json::from_slice(&line) else {
                continue;
            };
            if tx.send(value).is_err() {
                break;
            }
        }
    });
    let deadline = Instant::now() + Duration::from_secs(20);
    write(
        &mut child.0,
        json!({"id":1,"method":"initialize","params":{
        "clientInfo":{"name":"hachimodock-quota","version":env!("CARGO_PKG_VERSION")},
        "capabilities":{"experimentalApi":false}}}),
    )?;
    receive(&rx, 1, deadline)?;
    write(&mut child.0, json!({"method":"initialized"}))?;
    write(
        &mut child.0,
        json!({"id":2,"method":"account/read","params":{"refreshToken":false}}),
    )?;
    let account = receive(&rx, 2, deadline)?;
    if account["account"]["email"].as_str() != Some(email) {
        return Err("账号身份不匹配".into());
    }
    write(
        &mut child.0,
        json!({"id":3,"method":"account/rateLimits/read"}),
    )?;
    let response = receive(&rx, 3, deadline)?;
    normalize(&account["account"], &response, email)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn picks_codex_bucket_and_preserves_zero_missing_and_swapped_windows() {
        let account = json!({"type":"chatgpt","email":"member@example.com","planType":"plus"});
        let v = normalize(
            &account,
            &json!({"rateLimits":{"limitId":"other"},
            "rateLimitsByLimitId":{"codex":{"limitId":"codex","primary":null,
            "secondary":{"usedPercent":100,"windowDurationMins":10080,"resetsAt":1000}}}}),
            "member@example.com",
        )
        .unwrap();
        assert_eq!(v["primary"]["used_percent"], Value::Null);
        assert_eq!(v["secondary"]["used_percent"], 100);
        assert_eq!(v["secondary"]["window_minutes"], 10080);
        assert_eq!(v["plan_type"], "plus");
        assert!(normalize(
            &account,
            &json!({"rateLimitsByLimitId":{"other":{}},"rateLimits":{}}),
            "member@example.com"
        )
        .is_err());
    }
    #[test]
    fn refuses_wrong_account_or_different_quota_bucket() {
        let account = json!({"type":"chatgpt","email":"one@example.com"});
        assert!(normalize(&account, &json!({"rateLimits":{}}), "two@example.com").is_err());
        assert!(normalize(
            &account,
            &json!({"rateLimits":{"limitId":"other"}}),
            "one@example.com"
        )
        .is_err());
        assert!(normalize(
            &json!({"type":"apiKey"}),
            &json!({"rateLimits":{}}),
            "one@example.com"
        )
        .is_err());
    }
    #[test]
    fn upstream_errors_are_redacted_and_notifications_are_bounded() {
        let (tx, rx) = mpsc::channel();
        tx.send(json!({"method":"account/updated"})).unwrap();
        tx.send(json!({"id":2,"error":{"message":"private-token-must-not-escape"}}))
            .unwrap();
        let e = receive(&rx, 2, Instant::now() + Duration::from_secs(1)).unwrap_err();
        assert!(!e.contains("private-token"));
    }
}
