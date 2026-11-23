//! Bounded Codex account snapshots for the existing composition host.
//! No session history, package-selected paths or credentials leave this service.
use serde_json::{json, Value};
use std::{
    collections::BTreeMap,
    fs,
    io::Read,
    path::{Path, PathBuf},
    sync::OnceLock,
};
const STALE_MS: i64 = 300_000;
#[path = "local_telemetry/accounts.rs"]
pub mod accounts;
const PROVIDERS: &str = include_str!("../../skills/petui/scripts/local_telemetry_sources.json");

#[derive(serde::Deserialize)]
struct Provider<'a> {
    #[serde(borrow)]
    actions: Vec<&'a str>,
}

fn provider(source: &str) -> Option<&'static Provider<'static>> {
    static REGISTRY: OnceLock<BTreeMap<&'static str, Provider<'static>>> = OnceLock::new();
    REGISTRY.get_or_init(|| serde_json::from_str(PROVIDERS).expect("built-in telemetry registry"))
        .get(source)
}

pub fn registered(source: &str) -> bool {
    provider(source).is_some()
}
pub fn operations(source: &str) -> &'static [&'static str] {
    provider(source).map_or(&[], |provider| provider.actions.as_slice())
}
fn directory(path: &Path) -> bool {
    fs::symlink_metadata(path).is_ok_and(|m| m.is_dir() && !m.file_type().is_symlink())
}
fn unavailable(message: &str) -> Value {
    json!({"primary":unknown_window(),"secondary":unknown_window(),"badge":"等待数据","message":message,"observedAt":null})
}
fn unknown_window() -> Value {
    json!({"remaining":-1,"label":"—","reset":"待获取","timeRemainingPct":-1,"countdown":"等待新快照"})
}
fn countdown(seconds: i64) -> String {
    let minutes = (seconds + 59) / 60;
    if minutes >= 1440 {
        format!("{}天{}时", minutes / 1440, minutes % 1440 / 60)
    } else if minutes >= 60 {
        format!("{}时{}分", minutes / 60, minutes % 60)
    } else {
        format!("{minutes}分")
    }
}
fn window(limits: &Value, minutes: i64, at: i64, now: i64) -> Value {
    for key in ["primary", "secondary"] {
        let w = &limits[key];
        if w["window_minutes"].as_i64() != Some(minutes) {
            continue;
        }
        let Some(used) = w["used_percent"]
            .as_f64()
            .filter(|v| v.is_finite() && (0.0..=100.0).contains(v))
        else {
            continue;
        };
        let reset = w["resets_at"]
            .as_i64()
            .filter(|s| *s > at / 1000 && *s <= at / 1000 + minutes * 60 + 300);
        // Never infer a replenished allowance from a clock crossing the last reset time.
        if reset.is_some_and(|s| s <= now / 1000) {
            continue;
        }
        let remaining = (100.0 - used).floor() as i32;
        let label = format!("{remaining}");
        let seconds = reset.map(|s| s - now / 1000);
        let time_pct = seconds
            .map(|s| ((s * 100 + minutes * 60 - 1) / (minutes * 60)).clamp(0, 100))
            .unwrap_or(-1);
        let countdown = seconds.map(countdown).unwrap_or_else(|| "待获取".into());
        let reset = reset
            .and_then(|s| chrono::DateTime::from_timestamp(s, 0))
            .map(|d| {
                d.with_timezone(&chrono::Local)
                    .format(if minutes == 300 {
                        "%H:%M"
                    } else {
                        "%m/%d %H:%M"
                    })
                    .to_string()
            })
            .unwrap_or_else(|| "待获取".into());
        return json!({"remaining":remaining,"label":label,"reset":reset,"timeRemainingPct":time_pct,"countdown":countdown});
    }
    unknown_window()
}
pub fn snapshot(source: &str, selection: &mut u8, now: i64, action: Option<&str>) -> Result<Value, String> {
    let contract = provider(source).ok_or("未注册的本机数据源")?;
    if action.is_some_and(|action| !contract.actions.contains(&action)) {
        return Err("本机数据源不支持此动作".into());
    }
    match source {
        "codex.accounts" if !cfg!(target_os = "macos") => Err("Codex 额度账号管理需要 macOS".into()),
        "codex.accounts" => {
            if !(1..=accounts::settings::MAX_ACCOUNTS).contains(selection) {
                return Err("本机数据源的选择存档无效，原数据保留".into());
            }
            accounts::managed_snapshot(selection, now, action)
        },
        _ => Err("本机数据源的适配器不可用".into()),
    }
}
