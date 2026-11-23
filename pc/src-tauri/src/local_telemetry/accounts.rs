//! Read-only Codex app-server usage, using isolated login directories.
//! No credential is returned to a component, device, log, or distributable package.
use super::*;
use base64::Engine;
use sha2::{Digest, Sha256};
use std::{
    collections::BTreeMap,
    io::Write,
    sync::{Mutex, OnceLock},
    time::{Duration, Instant},
};
#[path = "app_server.rs"]
mod app_server;
#[path = "account_manager.rs"]
pub mod manager;
#[path = "account_settings.rs"]
pub mod settings;
static AUTH_WRITE: Mutex<()> = Mutex::new(());

pub fn root() -> Option<PathBuf> {
    std::env::var_os("HACHIMO_CODEX_ACCOUNTS_HOME")
        .map(PathBuf::from)
        .or_else(|| {
            std::env::var_os("HOME")
                .or_else(|| std::env::var_os("USERPROFILE"))
                .map(|h| PathBuf::from(h).join(".claw-pet/codex-accounts"))
        })
}
struct Identity {
    id: String,
    email: String,
    key: String,
}
fn identity(home: &Path) -> Option<Identity> {
    if !directory(home) {
        return None;
    }
    let path = home.join("auth.json");
    let metadata = fs::symlink_metadata(&path).ok()?;
    if !metadata.is_file() || metadata.file_type().is_symlink() || metadata.len() > 65536 {
        return None;
    }
    let auth: Value = serde_json::from_slice(&fs::read(path).ok()?).ok()?;
    if auth["auth_mode"] != "chatgpt" {
        return None;
    }
    let id = auth["tokens"]["account_id"].as_str()?.to_string();
    let token = auth["tokens"]["id_token"].as_str()?;
    let bytes = base64::engine::general_purpose::URL_SAFE_NO_PAD
        .decode(token.split('.').nth(1)?)
        .ok()?;
    let claims: Value = serde_json::from_slice(&bytes).ok()?;
    if claims["https://api.openai.com/auth"]["chatgpt_account_id"].as_str() != Some(id.as_str()) {
        return None;
    }
    let email = claims["email"].as_str()?.to_string();
    if id.is_empty() || email.len() > 63 || email.chars().any(char::is_control) {
        return None;
    }
    // Track credential revisions separately from the stable account cache owner.
    let key = format!(
        "{:x}",
        Sha256::digest(serde_json::to_vec(&auth["tokens"]).ok()?)
    );
    Some(Identity { id, email, key })
}
struct Entry {
    attempt: Instant,
    busy: bool,
    value: Option<(i64, Value)>,
    error: Option<String>,
    credential_revision: String,
    request_id: u64,
}
fn cache_key(home: &Path, account: &Identity) -> String {
    // A token refresh changes credentials, not the owner of the last good quota.
    // Different account IDs/emails in a reused slot always get separate entries.
    let owner = format!("{}\0{}", account.id, account.email);
    format!("{}:{:x}", home.display(), Sha256::digest(owner.as_bytes()))
}
fn new_entry(revision: &str) -> Entry {
    Entry {
        attempt: Instant::now() - Duration::from_secs(3600),
        busy: false,
        value: None,
        error: None,
        credential_revision: revision.into(),
        request_id: 0,
    }
}
pub(super) fn retire_account(home: &Path) {
    let prefix = format!("{}:", home.display());
    cache()
        .lock()
        .unwrap_or_else(|p| p.into_inner())
        .retain(|key, _| !key.starts_with(&prefix));
}
fn next_request_id() -> u64 {
    use std::sync::atomic::{AtomicU64, Ordering};
    static NEXT: AtomicU64 = AtomicU64::new(1);
    NEXT.fetch_add(1, Ordering::Relaxed)
}
static CACHE: OnceLock<Mutex<BTreeMap<String, Entry>>> = OnceLock::new();
fn cache() -> &'static Mutex<BTreeMap<String, Entry>> {
    CACHE.get_or_init(|| Mutex::new(BTreeMap::new()))
}
#[cfg(test)]
fn normalize(result: &Value, expected: &str) -> Result<Value, String> {
    if result["account_id"].as_str() != Some(expected) {
        return Err("账号身份不匹配".into());
    }
    let limits = result
        .get("rate_limit")
        .filter(|v| v.is_object())
        .ok_or("未返回 Codex 额度")?;
    // Membership comes from this same account-checked usage snapshot, never a
    // login claim or a previously selected account's cache. Bound raw metadata.
    let plan = result.get("plan_type").and_then(Value::as_str).map(|p| {
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
    let mut out = json!({"limit_id":"codex","plan_type":plan});
    for (source, target) in [
        ("primary_window", "primary"),
        ("secondary_window", "secondary"),
    ] {
        let w = &limits[source];
        let seconds = w["limit_window_seconds"].as_i64();
        let minutes = seconds.filter(|s| *s > 0 && *s % 60 == 0).map(|s| s / 60);
        out[target] = json!({"used_percent":w["used_percent"],"window_minutes":minutes,"resets_at":w["reset_at"]});
    }
    Ok(out)
}
fn fetch(home: &Path, expected: &str) -> Result<(Value, String), String> {
    let _write = AUTH_WRITE.lock().map_err(|_| "登录状态暂不可用")?;
    settings::protected_directory(home)?;
    let account = identity(home)
        .filter(|i| i.id == expected)
        .ok_or("需重新登录")?;
    let value = app_server::read(home, &account.email)?;
    // Codex owns refresh-token rotation. Recheck the stored account before
    // accepting a response; a concurrent credential replacement cannot inherit it.
    let after = identity(home)
        .filter(|i| i.id == expected && i.email == account.email)
        .ok_or("账号已变更，请重新查询")?;
    Ok((value, after.key))
}
fn plan_label(plan: Option<&str>) -> &'static str {
    // Match upstream KnownPlan::display_name(), protocol/src/auth.rs:
    // https://github.com/openai/codex/blob/main/codex-rs/protocol/src/auth.rs
    // In particular the live "prolite" entitlement is named "Pro", while
    // "pro" and "promax" are separate tiers, never aliases for that badge.
    match plan {
        Some("plus") => "Plus",
        Some("prolite") => "Pro",
        Some("pro") => "Pro (More)",
        Some("promax") => "Pro (Max)",
        Some(_) => "未知",
        None => "待获取",
    }
}
fn identity_caption(slot: u8, email: &str, status: &str) -> String {
    identity_caption_count(slot, 2, email, status)
}
fn identity_caption_count(index: u8, count: u8, email: &str, status: &str) -> String {
    let prefix = format!("{index}/{count} · ");
    let suffix = format!(" · {status}");
    if prefix.len() + email.len() + suffix.len() <= 63 {
        return format!("{prefix}{email}{suffix}");
    }
    // Preserve the selected slot and status for a long UTF-8 email. Membership
    // has its own badge; never cut an address in the middle of a codepoint.
    let budget = 63 - prefix.len() - suffix.len() - 2;
    let mut end = 0;
    for (i, c) in email.char_indices() {
        if i + c.len_utf8() > budget {
            break;
        }
        end = i + c.len_utf8();
    }
    format!("{prefix}{}..{suffix}", &email[..end])
}
fn render(slot: u8, identity: Option<&Identity>, entry: Option<&Entry>, now: i64) -> Value {
    let mut out = unavailable("等待账号登录");
    out["accountIndex"] = slot.into();
    out["email"] = identity
        .map(|i| i.email.as_str())
        .unwrap_or("未登录")
        .into();
    out["plan_type"] = Value::Null;
    let mut status = if identity.is_some() {
        "待查询"
    } else {
        "未登录"
    }
    .to_string();
    if let Some(e) = entry {
        if let Some((at, limits)) = &e.value {
            out["plan_type"] = limits["plan_type"].clone();
            out["primary"] = window(limits, 300, *at, now);
            out["secondary"] = window(limits, 10080, *at, now);
            out["observedAt"] = (*at).into();
            let updated = chrono::DateTime::from_timestamp_millis(*at)
                .unwrap()
                .with_timezone(&chrono::Local)
                .format("%H:%M:%S")
                .to_string();
            let stale = now - *at > STALE_MS || e.error.is_some();
            status = format!("{} {updated}", if stale { "过期" } else { "更新" });
            if stale {
                for key in ["primary", "secondary"] {
                    if out[key]["remaining"] != -1 {
                        out[key]["countdown"] =
                            format!("过期·{}", out[key]["countdown"].as_str().unwrap_or("")).into();
                    }
                }
            }
        }
        if e.busy {
            status = "查询中".into();
        } else if let Some(error) = &e.error {
            status = if e.value.is_some() {
                format!("{error} · {status}")
            } else {
                error.clone()
            };
        }
    }
    out["status"] = status.clone().into();
    let email = out["email"].as_str().unwrap_or("未登录");
    let short = if status.starts_with("更新") || status.starts_with("过期") {
        status.chars().take(8).collect::<String>()
    } else if status.starts_with("查询中") {
        "查询中".into()
    } else if status == "待查询" {
        "待获取".into()
    } else if status.contains("登录") {
        "未登录".into()
    } else {
        "查询失败".into()
    };
    let plan = plan_label(out["plan_type"].as_str());
    let caption = if identity.is_some() {
        identity_caption(slot, email, &short)
    } else {
        format!("{slot}/2 · 未登录")
    };
    // Reuse the existing two strings: account drives the prominent plan badge,
    // identity drives the selected slot, email and freshness line. No ninth var.
    out["plan"] = plan.into();
    out["account"] = plan.into();
    out["identity"] = caption.into();
    for key in ["primary", "secondary"] {
        out[key]["detail"] = format!(
            "{} · {}",
            out[key]["reset"].as_str().unwrap_or("待获取"),
            out[key]["countdown"].as_str().unwrap_or("待获取")
        )
        .into();
    }
    out
}
pub fn snapshot(slot: u8, now: i64, refresh: bool) -> Value {
    snapshot_with(slot, now, refresh, fetch)
}
fn snapshot_with<F>(slot: u8, now: i64, refresh: bool, fetcher: F) -> Value
where
    F: FnOnce(&Path, &str) -> Result<(Value, String), String> + Send + 'static,
{
    #[cfg(windows)]
    {
        let _ = (slot, refresh);
        return render(1, None, None, now);
    }
    let slot = slot.clamp(1, settings::MAX_ACCOUNTS);
    let Some(root) = root() else {
        return render(slot, None, None, now);
    };
    let home = root.join(slot.to_string());
    let Some(account) = identity(&home) else {
        return render(slot, None, None, now);
    };
    let key = cache_key(&home, &account);
    let mut entries = cache().lock().unwrap_or_else(|p| p.into_inner());
    if !entries.contains_key(&key) && entries.len() >= 8 {
        // Never evict an in-flight task.
        let removable = entries
            .iter()
            .find(|(_, e)| !e.busy)
            .map(|(k, _)| k.clone());
        if let Some(old) = removable {
            entries.remove(&old);
        } else {
            return render(slot, Some(&account), None, now);
        }
    }
    let e = entries
        .entry(key.clone())
        .or_insert_with(|| new_entry(&account.key));
    if !e.busy && e.credential_revision != account.key {
        // Recheck new credentials without throwing away this same account's
        // dated display value. Explicit removal/reauth retires the entry.
        e.credential_revision = account.key.clone();
        e.attempt = Instant::now() - Duration::from_secs(3600);
    }
    if !e.busy && e.attempt.elapsed() >= Duration::from_secs(if refresh { 15 } else { 60 }) {
        e.busy = true;
        e.attempt = Instant::now();
        e.request_id = next_request_id();
        let request_id = e.request_id;
        let expected = account.id.clone();
        let email = account.email.clone();
        std::thread::spawn(move || {
            let result = fetcher(&home, &expected);
            let at = chrono::Utc::now().timestamp_millis();
            let mut entries = cache().lock().unwrap_or_else(|p| p.into_inner());
            // A retired request cannot update a recreated entry for the same slot.
            if let Some(e) = entries.get_mut(&key).filter(|e| e.request_id == request_id) {
                e.busy = false;
                match result {
                    Ok((v, revision))
                        if identity(&home).is_some_and(|i| {
                            i.id == expected && i.email == email && i.key == revision
                        }) =>
                    {
                        e.value = Some((at, v));
                        e.error = None;
                        e.credential_revision = revision;
                    }
                    Ok(_) => e.error = Some("账号已变更，请重新查询".into()),
                    Err(error) => e.error = Some(error),
                }
            }
        });
    }
    render(slot, Some(&account), Some(e), now)
}
/// Display-only selection is shared by the PC manager and the device feed.
pub fn managed_snapshot(slot: &mut u8, now: i64, operation: Option<&str>) -> Result<Value, String> {
    let root = root().ok_or("账号目录不可用")?;
    let settings = settings::load(&root)?;
    let direction = match operation {
        Some("next") => 1,
        Some("previous") => -1,
        _ => 0,
    };
    let shown = settings.displayed();
    let Some(next) = settings.selected(*slot, direction) else {
        let mut out = render(1, None, None, now);
        out["accountIndex"] = 0.into();
        out["accountCount"] = 0.into();
        out["identity"] = "0/0 · 未登录".into();
        out["message"] = "没有勾选要显示的账号，请在 PC 组件详情中选择".into();
        return Ok(out);
    };
    *slot = next;
    let mut out = snapshot(next, now, operation == Some("refresh"));
    let index = shown.iter().position(|s| *s == next).unwrap() as u8 + 1;
    out["accountIndex"] = index.into();
    out["accountCount"] = (shown.len() as u8).into();
    let identity = out["identity"]
        .as_str()
        .unwrap_or("")
        .split_once(" · ")
        .map(|(_, v)| v)
        .unwrap_or("未登录");
    out["identity"] = format!("{index}/{} · {identity}", shown.len()).into();
    Ok(out)
}
/// Read existing cache without starting network work or returning credentials.
fn cached(slot: u8, now: i64) -> Value {
    let Some(root) = root() else {
        return render(slot, None, None, now);
    };
    let home = root.join(slot.to_string());
    let account = identity(&home);
    let entries = cache().lock().unwrap_or_else(|p| p.into_inner());
    let entry = account
        .as_ref()
        .and_then(|i| entries.get(&cache_key(&home, i)));
    render(slot, account.as_ref(), entry, now)
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn rejects_cross_account_and_missing_identity() {
        assert!(normalize(&json!({"account_id":"other","rate_limit":{}}), "expected").is_err());
        assert!(normalize(&json!({"rate_limit":{}}), "expected").is_err());
    }
    #[test]
    fn converts_official_windows_and_preserves_zero() {
        let v=normalize(&json!({"account_id":"ok","rate_limit":{"primary_window":{"used_percent":100,"limit_window_seconds":18000,"reset_at":1794819600},"secondary_window":{"used_percent":0,"limit_window_seconds":604800,"reset_at":1794902400}}}),"ok").unwrap();
        assert_eq!(
            window(&v, 300, 1794816000000, 1794816000000)["remaining"],
            0
        );
        assert_eq!(
            window(&v, 10080, 1794816000000, 1794816000000)["remaining"],
            100
        );
    }
    #[test]
    fn never_uses_another_accounts_cache_for_missing_login() {
        let out = render(2, None, None, 1794816000000);
        assert_eq!(out["primary"]["remaining"], -1);
        assert_eq!(out["email"], "未登录");
        assert_eq!(out["accountIndex"], 2);
        assert_eq!(out["account"], "待获取");
        assert_eq!(out["identity"], "2/2 · 未登录");
    }
    #[test]
    fn membership_is_taken_from_the_verified_usage_snapshot() {
        for (raw, label) in [
            ("plus", "Plus"),
            ("prolite", "Pro"),
            ("pro", "Pro (More)"),
            ("promax", "Pro (Max)"),
            ("future_plan", "未知"),
        ] {
            let v = normalize(
                &json!({"account_id":"ok","plan_type":raw,"rate_limit":{}}),
                "ok",
            )
            .unwrap();
            assert_eq!(v["plan_type"], raw);
            assert_eq!(plan_label(v["plan_type"].as_str()), label);
        }
        let missing = normalize(&json!({"account_id":"ok","rate_limit":{}}), "ok").unwrap();
        assert_eq!(missing["plan_type"], Value::Null);
        assert_eq!(plan_label(None), "待获取");
        for invalid in ["Plus", "plus\nsecret", "", &"a".repeat(33)] {
            let v = normalize(
                &json!({"account_id":"ok","plan_type":invalid,"rate_limit":{}}),
                "ok",
            )
            .unwrap();
            assert_eq!(v["plan_type"], "unknown");
            assert_eq!(plan_label(v["plan_type"].as_str()), "未知");
        }
    }
    #[test]
    fn selected_account_keeps_its_own_plan_and_marks_stale_data() {
        let now = 1794816000000;
        let first = Identity {
            id: "first".into(),
            email: "one@example.com".into(),
            key: "a".into(),
        };
        let second = Identity {
            id: "second".into(),
            email: "two@example.com".into(),
            key: "b".into(),
        };
        let entry = |plan| Entry {
            attempt: Instant::now(),
            busy: false,
            value: Some((now, json!({"plan_type":plan}))),
            error: None,
            credential_revision: String::new(),
            request_id: 0,
        };
        // Actual observed entitlement values for the two independently authorized
        // accounts are plus / prolite; the upstream display names are Plus / Pro.
        let a = render(1, Some(&first), Some(&entry("plus")), now);
        let b = render(2, Some(&second), Some(&entry("prolite")), now);
        assert_eq!(a["account"], "Plus");
        assert_eq!(b["account"], "Pro");
        assert!(a["identity"]
            .as_str()
            .unwrap()
            .starts_with("1/2 · one@example.com · 更新"));
        assert!(b["identity"]
            .as_str()
            .unwrap()
            .starts_with("2/2 · two@example.com · 更新"));
        let pending = render(2, Some(&second), None, now);
        assert_eq!(pending["plan"], "待获取");
        assert!(!pending["identity"].as_str().unwrap().contains("Plus"));
        let stale = render(1, Some(&first), Some(&entry("plus")), now + STALE_MS + 1);
        assert_eq!(stale["account"], "Plus");
        assert!(stale["identity"].as_str().unwrap().contains(" · 过期"));
    }
    #[test]
    fn prolite_pro_and_promax_are_distinct_official_tiers() {
        let now = 1794816000000;
        let id = Identity {
            id: "ok".into(),
            email: "member@example.com".into(),
            key: "k".into(),
        };
        for (raw, display) in [
            ("prolite", "Pro"),
            ("pro", "Pro (More)"),
            ("promax", "Pro (Max)"),
        ] {
            let value = normalize(
                &json!({"account_id":"ok","plan_type":raw,"rate_limit":{}}),
                "ok",
            )
            .unwrap();
            let entry = Entry {
                attempt: Instant::now(),
                busy: false,
                value: Some((now, value)),
                error: None,
                credential_revision: String::new(),
                request_id: 0,
            };
            let out = render(2, Some(&id), Some(&entry), now);
            assert_eq!(out["plan_type"], raw);
            assert_eq!(out["account"], display);
        }
    }
    #[test]
    fn identity_caption_is_bounded_without_cutting_utf8() {
        for email in ["a".repeat(63), "账".repeat(21)] {
            let text = identity_caption(2, &email, "更新 00:00");
            assert!(text.len() <= 63);
            assert!(text.starts_with("2/2 · "));
            assert!(text.ends_with(" · 更新 00:00"));
            assert!(text.contains(".."));
        }
    }
}
