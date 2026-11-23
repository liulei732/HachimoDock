//! Local telemetry, todo, shared location and authorized provider snapshots.
use crate::{
    lan_agent::widget_data::{Route, Target},
    widget_data::{DataRow, DataSnapshot},
};
use std::{
    sync::{Mutex, OnceLock},
    time::{Duration, Instant},
};
static COMPUTER: OnceLock<Mutex<Option<(Instant, DataSnapshot)>>> = OnceLock::new();
const COMPUTER_STATUS_TTL_MS: u32 = 30_000;
fn cache() -> &'static Mutex<Option<(Instant, DataSnapshot)>> {
    COMPUTER.get_or_init(Default::default)
}
pub fn computer_status() -> DataSnapshot {
    match cache().lock().ok().and_then(|s| s.clone()) {
        Some((at, mut data)) => {
            if at.elapsed() > Duration::from_millis(COMPUTER_STATUS_TTL_MS.into()) {
                data.status = "error".into();
                data.message = "电脑状态已过期，等待恢复采样".into();
            }
            data
        }
        None => snapshot(vec![], "empty", "正在采样电脑状态"),
    }
}
#[tauri::command]
pub fn local_widget_snapshot(source: String) -> Result<DataSnapshot, String> {
    match source.as_str() {
        "computer.status" => Ok(computer_status()),
        "stocks.watchlist" => crate::stock_quotes::preview_snapshot(),
        "location.current" => Ok(crate::shared_location::snapshot()),
        "todos.upcoming" => Ok(crate::todos::snapshot()),
        other if crate::auth_provider::is_source(other) => crate::auth_provider::snapshot(other),
        _ => Err("不支持的数据源".into()),
    }
}
fn snapshot(rows: Vec<DataRow>, status: &str, message: &str) -> DataSnapshot {
    DataSnapshot {
        schema: 1,
        date: chrono::Local::now().format("%Y-%m-%d").to_string(),
        source: "computer.status".into(),
        ttl_ms: COMPUTER_STATUS_TTL_MS,
        status: status.into(),
        message: message.into(),
        rows,
    }
}
fn rate(bytes: u64, seconds: f64) -> String {
    if seconds <= 0.0 || !seconds.is_finite() {
        return "—".into();
    }
    let kib = bytes as f64 / seconds / 1024.0;
    if kib >= 1024.0 {
        format!("{:.1} MiB/s", kib / 1024.0)
    } else {
        format!("{kib:.1} KiB/s")
    }
}
fn row(id: &str, label: &str, value: String, detail: String, meta: &str) -> DataRow {
    DataRow {
        id: id.into(),
        label: label.into(),
        value: crate::widget_data::bounded_text(&value, 24),
        detail: crate::widget_data::bounded_text(&detail, 24),
        meta: meta.into(),
        tone: 0,
    }
}
struct Sampler {
    system: sysinfo::System,
    networks: sysinfo::Networks,
    sampled: Instant,
    peak_cpu: f32,
}
impl Sampler {
    fn new() -> Self {
        let mut system = sysinfo::System::new();
        system.refresh_cpu_usage();
        Self {
            system,
            networks: sysinfo::Networks::new_with_refreshed_list(),
            sampled: Instant::now(),
            peak_cpu: 0.0,
        }
    }
    fn sample(&mut self) -> DataSnapshot {
        let elapsed = self.sampled.elapsed().as_secs_f64();
        self.sampled = Instant::now();
        self.system.refresh_cpu_usage();
        self.system.refresh_memory();
        self.networks.refresh(true);
        let down = self
            .networks
            .values()
            .fold(0u64, |n, i| n.saturating_add(i.received()));
        let up = self
            .networks
            .values()
            .fold(0u64, |n, i| n.saturating_add(i.transmitted()));
        let total = self.system.total_memory();
        let used = self.system.used_memory();
        let memory = if total > 0 {
            format!("{:.0}%", used as f64 / total as f64 * 100.0)
        } else {
            "—".into()
        };
        let cpu = self.system.global_cpu_usage();
        self.peak_cpu = self.peak_cpu.max(cpu);
        let uptime = sysinfo::System::uptime();
        let gib = |bytes: u64| format!("{:.1} GiB", bytes as f64 / 1073741824.0);
        let rows = vec![
            row(
                "cpu",
                "CPU",
                format!("{cpu:.0}%"),
                String::new(),
                "处理器总占用",
            ),
            row(
                "memory",
                "内存",
                memory,
                format!(
                    "{:.1}/{:.1} GiB",
                    used as f64 / 1073741824.0,
                    total as f64 / 1073741824.0
                ),
                "已用 / 总量",
            ),
            row(
                "down",
                "网络接收",
                rate(down, elapsed),
                String::new(),
                "全部网络接口合计",
            ),
            row(
                "up",
                "网络发送",
                rate(up, elapsed),
                String::new(),
                "含虚拟接口，非测速",
            ),
            row(
                "uptime",
                "开机时长",
                format!(
                    "{}h {}m",
                    uptime / 3600,
                    uptime % 3600 / 60
                ),
                String::new(),
                "电脑启动至今",
            ),
            row("available", "可用内存",
                if total > 0 { gib(self.system.available_memory()) } else { "—".into() },
                String::new(), "系统可供分配的内存"),
            row("swap", "交换空间", gib(self.system.used_swap()),
                gib(self.system.total_swap()), "已用 / 总量"),
            row("peak_cpu", "CPU 峰值", format!("{:.0}%", self.peak_cpu),
                String::new(), "客户端本次运行的采样峰值"),
        ];
        snapshot(rows, "ok", "")
    }
}
fn todo_due(last: Option<&(Route, Instant, serde_json::Value)>, route: &Route,
           now: Instant, payload: &serde_json::Value, ttl_ms: u64) -> bool {
    last.is_none_or(|(old_route, sent, old)| {
        old_route != route || old != payload || now.duration_since(*sent) >= Duration::from_millis(ttl_ms / 2)
    })
}

pub fn start(usb: crate::usb_serial::UsbSerialManager) {
    let providers_usb = usb.clone();
    // Provider HTTP deadlines must not stall the local sampler or expiry updates.
    std::thread::Builder::new()
        .name("widget-providers".into())
        .spawn(move || loop {
            std::thread::sleep(Duration::from_secs(2));
            let Some(target) = Target::current(&providers_usb) else {
                continue;
            };
            for source in crate::auth_provider::push_sources() {
                if let Ok(snapshot) = crate::auth_provider::snapshot(source) {
                    if let Ok(payload) = serde_json::to_value(snapshot) {
                        let _ = target.publish(&providers_usb, &payload);
                    }
                }
            }
        })
        .expect("start widget providers");
    // Sampling runs away from async audio/network workers and never enumerates processes.
    std::thread::Builder::new()
        .name("local-widget-sampler".into())
        .spawn(move || {
            let mut sampler = Sampler::new();
            let mut todo_sent = None;
            let mut location_sent: Option<(Route, Instant, String)> = None;
            loop {
                std::thread::sleep(Duration::from_secs(2));
                let sample = sampler.sample();
                if let Ok(mut data) = cache().lock() {
                    *data = Some((Instant::now(), sample.clone()));
                }
                let Some(target) = Target::current(&usb) else {
                    location_sent = None;
                    todo_sent = None;
                    continue;
                };
                if let Ok(payload) = serde_json::to_value(sample) {
                    let _ = target.publish(&usb, &payload);
                }
                let todos = crate::todos::snapshot();
                let ttl_ms = u64::from(todos.ttl_ms);
                if let Ok(payload) = serde_json::to_value(todos) {
                    if todo_due(todo_sent.as_ref(), &target.route, Instant::now(), &payload, ttl_ms) {
                        todo_sent = target.publish(&usb, &payload).ok()
                            .map(|()| (target.route.clone(), Instant::now(), payload));
                    }
                }
                if target.supports_expiry() {
                    let location = crate::shared_location::snapshot();
                    if let Ok(serialized) = serde_json::to_string(&location) {
                        let due = location_sent.as_ref().is_none_or(|(route, at, previous)| {
                            *route != target.route
                                || *previous != serialized
                                || at.elapsed() >= Duration::from_secs(20)
                        });
                        if !due {
                            continue;
                        }
                        if let Ok(mut payload) = serde_json::to_value(&location) {
                            payload["clearOnExpire"] = true.into();
                            if target.publish(&usb, &payload).is_ok() {
                                location_sent = Some((target.route, Instant::now(), serialized));
                            }
                        }
                    }
                }
            }
        })
        .expect("start local widget sampler");
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn unchanged_todos_renew_before_expiry_and_new_route_sends_immediately() {
        let route = Route::Usb {board:"test".into(), connection:1};
        let next = Route::Usb {board:"test".into(), connection:2};
        let now = Instant::now();
        let data = serde_json::json!({"status":"ok","rows":[]});
        let last = Some((route.clone(), now, data.clone()));
        assert!(todo_due(None, &route, now, &data, 30_000));
        assert!(!todo_due(last.as_ref(), &route, now + Duration::from_secs(14), &data, 30_000));
        assert!(todo_due(last.as_ref(), &route, now + Duration::from_secs(15), &data, 30_000));
        assert!(todo_due(last.as_ref(), &next, now, &data, 30_000));
        assert!(todo_due(last.as_ref(), &route, now, &serde_json::Value::Null, 30_000));

        let mut sent = None;
        let mut deliveries = 0;
        for tick in 0..=150 {
            let at = now + Duration::from_secs(tick * 2);
            if todo_due(sent.as_ref(), &route, at, &data, 30_000) {
                if let Some((_, previous, _)) = sent.as_ref() {
                    assert!(at.duration_since(*previous) < Duration::from_secs(30));
                }
                deliveries += 1;
                sent = Some((route.clone(), at, data.clone()));
            }
        }
        assert_eq!(deliveries, 19);
    }
    #[test]
    fn rate_uses_measured_interval() {
        assert_eq!(rate(2048, 2.0), "1.0 KiB/s");
        assert_eq!(rate(2097152, 2.0), "1.0 MiB/s");
        assert_eq!(rate(1, 0.0), "—");
    }
    #[test]
    fn local_sample_is_bounded_and_contains_no_identity() {
        let mut sampler = Sampler::new();
        let first = sampler.sample();
        let result = sampler.sample();
        let ids: Vec<_> = result.rows.iter().map(|r| r.id.as_str()).collect();
        assert_eq!(ids, ["cpu", "memory", "down", "up", "uptime", "available", "swap", "peak_cpu"]);
        let percent = |r: &DataRow| r.value.trim_end_matches('%').parse::<u32>().unwrap();
        assert_eq!(percent(&result.rows[7]), percent(&first.rows[0]).max(percent(&result.rows[0])));
        assert_eq!(result.rows[6].value, format!("{:.1} GiB", sampler.system.used_swap() as f64 / 1073741824.0));
        if sampler.system.total_memory() > 0 {
            assert_eq!(result.rows[5].value, format!("{:.1} GiB", sampler.system.available_memory() as f64 / 1073741824.0));
        }
        for r in result.rows {
            assert!(
                r.id.len() <= 23
                    && r.label.len() <= 36
                    && r.value.len() <= 24
                    && r.detail.len() <= 24
                    && r.meta.len() <= 63
            );
        }
    }
}
