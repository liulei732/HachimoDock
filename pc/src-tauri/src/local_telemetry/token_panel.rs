//! The Codex live-data source exports only screen-ready scalar projections.
use super::token_history::{Breakdown, Usage};
use chrono::{Days, Local};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};

pub const OPERATIONS: &[&str] = &[
    "refresh",
    "previous",
    "next",
    "page_up",
    "page_down",
    "quota",
    "overview",
    "detail",
    "cycle_period",
    "hour",
    "today",
    "day",
    "week",
    "month",
];
pub const PERIOD_ACTIONS: &[&str] = &["hour", "day", "week", "month", "today"];
const TODAY: u8 = 4;
const LABEL_GLYPHS: &str = "0123456789/KMGB. —TQE%";
const BAR_START: u32 = 0xe000;
const LABEL_START: u32 = 0xe040;
const BAR_STEPS: u64 = 32;
const MIX_START: u32 = 0xe060;
const MIX_WIDTH: u64 = 544;
const MIX_CELL: u64 = 8; // Eight two-pixel steps per font cell.
// Match the pane's Latin atlas. These characters also have a large metric glyph.
const SMALL_METRIC_CHARS: &str = "0123456789.KMBTQE%";
const ALIGNED_CHARS: &str = concat!(
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 @._-+/:%'·!~=&?",
    "待获取未登录查询中更新过期失败其他识别模型正在统计本机暂无记"
);

pub(super) fn screen_text(text: &str, max_bytes: usize) -> String {
    encode_label(text, max_bytes, SMALL_METRIC_CHARS, 0x100)
}

pub(super) fn aligned_text(text: &str, max_bytes: usize) -> String {
    encode_label(text, max_bytes, ALIGNED_CHARS, 0x180)
}

fn encode_label(text: &str, max_bytes: usize, chars: &str, start: u32) -> String {
    let mut result = String::new();
    for ch in text.chars() {
        let glyph = chars.chars().position(|candidate| candidate == ch)
            .and_then(|index| char::from_u32(start + index as u32))
            .unwrap_or(ch);
        if result.len() + glyph.len_utf8() > max_bytes {
            break;
        }
        result.push(glyph);
    }
    result
}

fn stack_levels(segments: [u64; 3], scale: u64, max_height: u64) -> [u64; 3] {
    let nonempty = segments.iter().filter(|&&count| count > 0).count() as u64;
    let total = segments.iter().map(|&count| count as u128).sum::<u128>();
    if total == 0 {
        return [0; 3];
    }
    let height = (total * max_height as u128)
        .div_ceil(scale.max(1) as u128)
        .clamp(nonempty as u128, max_height as u128) as u64;
    let mut count = 0u128;
    let mut visible = 0;
    segments.map(|segment| {
        count += segment as u128;
        visible += u64::from(segment > 0);
        // Give nonempty segments one display step, including small outputs
        // that used to round to the same height as the cumulative input.
        visible + (count * (height - nonempty) as u128 / total) as u64
    })
}

#[derive(Clone, Copy, Debug, Deserialize, Serialize)]
#[serde(default, deny_unknown_fields)]
pub struct View {
    page: u8,
    period: u8,
}

impl Default for View {
    fn default() -> Self {
        Self { page: 0, period: 2 }
    }
}

impl View {
    pub fn page(self) -> u8 {
        self.page.min(2)
    }

    pub fn apply(&mut self, operation: Option<&str>, include_today: bool) {
        self.page = self.page();
        if self.period > TODAY || (self.period == TODAY && !include_today) {
            self.period = 2;
        }
        match operation.unwrap_or("") {
            "page_up" => self.page = (self.page + 2) % 3,
            "page_down" => self.page = (self.page + 1) % 3,
            "quota" => self.page = 0,
            "overview" => self.page = 1,
            "detail" => self.page = 2,
            "cycle_period" if self.page == 2 => {
                self.period = match self.period {
                    0 if include_today => TODAY,
                    TODAY => 1,
                    period => (period + 1) % 4,
                };
            }
            "hour" if self.page == 2 => self.period = 0,
            "today" if self.page == 2 && include_today => self.period = TODAY,
            "day" if self.page == 2 => self.period = 1,
            "week" if self.page == 2 => self.period = 2,
            "month" if self.page == 2 => self.period = 3,
            _ => {}
        }
    }

    pub(super) fn project(
        self,
        quota: &Value,
        usage: Option<&Usage>,
        busy: bool,
        now: i64,
    ) -> Value {
        let status = if busy && usage.is_none() {
            "正在统计"
        } else {
            "本机暂无记录"
        };
        match self.page() {
            0 => json!({"mode":0,
                "n0":quota["primary"]["remaining"],"n1":quota["secondary"]["remaining"],
                "n2":quota["primary"]["timeRemainingPct"],"n3":quota["secondary"]["timeRemainingPct"],
                "s0":quota["primary"]["detail"],"s1":quota["secondary"]["detail"],
                "refined":{"n2":quota["primary"]["timeRemainingPct"],"n3":quota["secondary"]["timeRemainingPct"],"alignedHeadline":"",
                    "metricText":quota["secondary"]["detail"],"alignedMetricText":quota["secondary"]["detail"]}}),
            1 => overview(usage, status, now),
            _ => {
                let data = usage.and_then(|usage| {
                    if self.period != TODAY {
                        return usage.periods.get(self.period as usize);
                    }
                    let today = chrono::DateTime::from_timestamp_millis(now)?
                        .with_timezone(&Local)
                        .date_naive();
                    let (date, breakdown) = usage.days.last()?;
                    (*date == today).then_some(breakdown)
                });
                details(data, self.period, status)
            }
        }
    }
}

fn compact(n: u64) -> String {
    if n < 1_000 {
        return n.to_string();
    }
    for (divisor, unit) in [
        (1_000_000_000_000_000_000, "E"),
        (1_000_000_000_000_000, "Q"),
        (1_000_000_000_000, "T"),
        (1_000_000_000, "B"),
        (1_000_000, "M"),
        (1_000, "K"),
    ] {
        if n >= divisor {
            let scaled = n as f64 / divisor as f64;
            return if scaled < 9.995 {
                format!("{scaled:.2}{unit}")
            } else if scaled < 99.95 {
                format!("{scaled:.1}{unit}")
            } else {
                format!("{scaled:.0}{unit}")
            };
        }
    }
    unreachable!()
}

fn metric_glyphs(text: &str) -> String {
    text.chars().map(label_glyph).collect()
}

fn label_strip(labels: impl Iterator<Item = String>) -> String {
    labels
        .flat_map(|label| {
            let n = label.chars().count().min(7);
            let left = (7 - n) / 2;
            let mut padded = " ".repeat(left);
            padded.extend(label.chars().take(n));
            padded.extend(std::iter::repeat_n(' ', 7 - n - left));
            padded.chars().map(label_glyph).collect::<Vec<_>>()
        })
        .collect()
}

fn label_glyph(ch: char) -> char {
    let index = LABEL_GLYPHS.chars().position(|c| c == ch).unwrap_or(16);
    char::from_u32(LABEL_START + index as u32).unwrap()
}

// Six character cells make exactly one line in each right-aligned number
// column. Fixed pitch avoids mixing model-name widths into number placement.
fn number_column(numbers: impl IntoIterator<Item = String>) -> String {
    numbers
        .into_iter()
        .flat_map(|value| {
            debug_assert!(value.chars().count() <= 6);
            format!("{value:>6}")
                .chars()
                .map(label_glyph)
                .collect::<Vec<_>>()
        })
        .collect()
}

fn proportional_levels(segments: [u64; 3], scale: u64) -> [u64; 3] {
    let scale = scale.max(1) as u128;
    let mut cumulative = 0u128;
    segments.map(|segment| {
        cumulative += segment as u128;
        ((cumulative * BAR_STEPS as u128 + scale / 2) / scale).min(BAR_STEPS as u128) as u64
    })
}

fn mix_strip(fresh: u64, total: u64, input_percent: u64) -> String {
    let denominator = total.max(1) as u128;
    let max_steps = MIX_WIDTH / 2;
    let input_limit = MIX_WIDTH * input_percent / 100 / 2;
    let steps = ((fresh as u128 * max_steps as u128 + denominator / 2) / denominator) as u64;
    let steps = steps.min(input_limit);
    let glyph = |n| char::from_u32(MIX_START + (n - 1) as u32).unwrap();
    let mut strip: String =
        std::iter::repeat_n(glyph(MIX_CELL), (steps / MIX_CELL) as usize).collect();
    let remainder = steps % MIX_CELL;
    if remainder > 0 {
        strip.push(glyph(remainder));
    }
    strip
}

fn overview(usage: Option<&Usage>, status: &str, now: i64) -> Value {
    let today = chrono::DateTime::from_timestamp_millis(now)
        .unwrap()
        .with_timezone(&Local)
        .date_naive();
    let dates = std::array::from_fn::<_, 7, _>(|i| today - Days::new(6 - i as u64));
    let days = dates.map(|date| {
        usage.and_then(|u| {
            u.days
                .iter()
                .find(|(d, usage)| *d == date && usage.recorded)
                .map(|(_, usage)| usage)
        })
    });
    let total = days
        .iter()
        .flatten()
        .fold(0u64, |sum, day| sum.saturating_add(day.counts.total()));
    let peak = days
        .iter()
        .flatten()
        .map(|d| d.counts.total())
        .max()
        .unwrap_or_default();
    let step = 10u64.pow(peak.max(1).ilog10());
    let scale = peak.div_ceil(step).saturating_mul(step).max(1);
    let strip = |layer, refined| {
        days.iter()
            .map(|d| {
                let level = d.map_or(0, |d| {
                    let counts = d.counts;
                    let segments = [counts.input - counts.cached, counts.cached, counts.output];
                    let levels = if refined {
                        proportional_levels(segments, scale)
                    } else {
                        stack_levels(segments, scale, BAR_STEPS)
                    };
                    levels[layer]
                });
                char::from_u32(BAR_START + level as u32).unwrap()
            })
            .collect::<String>()
    };
    let dates = label_strip(dates.iter().map(|d| d.format("%m/%d").to_string()));
    let amounts = label_strip(days.iter().map(|d| {
        d.map(|d| compact(d.counts.total()))
            .unwrap_or_else(|| "—".into())
    }));
    let recorded = days.iter().flatten().count();
    let axes = if peak > 0 {
        number_column([compact(scale), compact(scale / 2), "0".to_owned()])
    } else {
        String::new()
    };
    let today_value = days[6]
        .map(|day| compact(day.counts.total()))
        .unwrap_or_else(|| "—".into());
    let peak_value = if recorded > 0 {
        compact(peak)
    } else {
        "—".into()
    };
    let [output, cached, fresh] = [strip(2, true), strip(1, true), strip(0, true)];
    json!({"mode":1,"n0":0,"n1":1,"n2":0,"n3":0,
        "s0":if recorded > 0 {compact(total)} else {"—".into()},
        "s1":today_value,
        "eyebrow":strip(2,false),"headline":strip(1,false),"metricValue":strip(0,false),
        "metricLabel":if total>0 {format!("本机统计 · {}/7 天有记录",days.iter().flatten().count())} else {status.into()},
        "metricUnit":if peak>0 {compact(scale)} else {String::new()},
        "badge":peak_value,
        "note":dates,"footer":amounts,
        "refined":{
            "eyebrow":output,"headline":cached,"metricValue":fresh,
            "screenEyebrow":output,"screenHeadline":cached,"screenMetricValue":fresh,
            "alignedEyebrow":output,"alignedHeadline":cached,"alignedMetricValue":fresh,
            "metricLabel":axes,"metricUnit":if recorded>0 {format!("本机记录 {recorded}/7 天")} else {status.into()},
            "caption":if recorded>0 {format!("近7天 · 本机 {recorded}/7 天")} else {format!("近7天 · {status}")},
            "metricText":metric_glyphs(&today_value),"metricBadge":metric_glyphs(&peak_value),
            "alignedMetricText":number_column([today_value.clone(),peak_value.clone()]),
            "note":dates,"footer":amounts,"n2":0,"n3":0
        },
        "status":if total>0 {format!("本机统计 · {}/7天有记录",days.iter().flatten().count())} else {status.into()}
    })
}

fn model_label(name: &str) -> String {
    if name.is_empty() {
        return String::new();
    }
    // The surface supplies the same baseline for every model label and uses
    // separate small digits so the large metric font cannot override them.
    let mut label = String::from(char::from_u32(BAR_START).unwrap());
    for ch in name.chars() {
        let display = match ch {
            '0'..='9' => char::from_u32(0xC0 + u32::from(ch) - u32::from('0')).unwrap(),
            '.' => 'Ê',
            'p' => 'Ë',
            'q' => 'Ì',
            'y' => 'Í',
            _ => ch,
        };
        if label.len() + display.len_utf8() > 63 {
            break;
        }
        label.push(display);
    }
    label
}

fn details(data: Option<&Breakdown>, period: u8, status: &str) -> Value {
    // Today shares the hourly layout. Keep firmware modes 6 and 7 reserved
    // for pending and offline rendering, and report the selected period separately.
    let mode = if period == TODAY { 2 } else { 2 + period };
    let period_action = PERIOD_ACTIONS[period as usize];
    let period_text = ["1小时", "24小时", "7天", "30天", "今日"][period as usize];
    let caption = format!("{period_text} · 本机");
    let Some(data) = data.filter(|d| d.recorded) else {
        return json!({"mode":mode,"period":period_action,"n0":0,"n1":0,"n2":0,"n3":-1,"s0":"—","s1":"—",
            "eyebrow":"—","metricValue":"—","metricLabel":"—","metricUnit":"—","badge":"—",
            "headline":status,"note":"","footer":"","status":status,
            "refined":{"eyebrow":model_label(status),"headline":"","metricValue":"",
                "screenEyebrow":screen_text(status, 63),"screenHeadline":"","screenMetricValue":"",
                "alignedEyebrow":aligned_text(status, 63),"alignedHeadline":"","alignedMetricValue":"",
                "metricLabel":number_column(std::iter::repeat_n("—".to_owned(),5)),
                "metricUnit":caption,"caption":caption,"metricText":metric_glyphs("—"),
                "alignedMetricText":metric_glyphs("—"),
                "note":"","footer":"","n2":0,"n3":-1}});
    };
    let counts = data.counts;
    let total = counts.total();
    let ratio = |n: u64, denominator: u64| {
        ((n as u128 * 100 + denominator as u128 / 2) / denominator.max(1) as u128) as u64
    };
    let mut models: Vec<_> = data
        .models
        .iter()
        .filter(|(name, _)| !name.is_empty())
        .collect();
    models.sort_by(|(a_name, a), (b_name, b)| b.cmp(a).then_with(|| a_name.cmp(b_name)));
    let mut names: Vec<(&str, u64)> = models
        .iter()
        .take(2)
        .map(|(name, count)| (name.as_str(), **count))
        .collect();
    let others = models
        .iter()
        .skip(2)
        .fold(0u64, |sum, (_, n)| sum.saturating_add(**n));
    let unknown = data.models.get("").copied().unwrap_or(0);
    if others > 0 || unknown > 0 {
        names.push((
            if others > 0 {
                "其他 / 未识别"
            } else {
                "未识别模型"
            },
            others.saturating_add(unknown),
        ));
    }
    let label = |i: usize| {
        names
            .get(i)
            .map(|(name, count)| {
                let shortened: String = name.chars().take(25).collect();
                format!("{shortened}  {}", compact(*count))
            })
            .unwrap_or_default()
    };
    let bar = |i: usize| {
        names
            .get(i)
            .map(|(_, count)| ratio(*count, total))
            .unwrap_or_default()
    };
    let [fresh_mix, input_mix, _] = stack_levels(
        [counts.input - counts.cached, counts.cached, counts.output],
        total,
        100,
    );
    let input_percent = ratio(counts.input, total);
    let amounts = number_column(names.iter().map(|(_, count)| compact(*count)));
    let name = |i: usize| names.get(i).map_or_else(String::new, |(name, _)| model_label(name));
    let screen_name = |i: usize| names.get(i).map_or_else(String::new, |(name, _)| screen_text(name, 63));
    let aligned_name = |i: usize| names.get(i).map_or_else(String::new, |(name, _)| aligned_text(name, 63));
    let cache_ratio = if counts.input > 0 {
        format!("{}%", ratio(counts.cached, counts.input))
    } else {
        "—".into()
    };
    json!({"mode":mode,"period":period_action,"n0":bar(0),"n1":bar(1),
    "n2":fresh_mix,"n3":if total == 0 { -1 } else { input_mix as i64 },
    "s0":compact(total),"s1":cache_ratio,
    "eyebrow":compact(counts.input),"metricValue":compact(counts.input-counts.cached),
    "metricLabel":compact(counts.cached),"metricUnit":compact(counts.output),"badge":compact(counts.reasoning),
    "headline":label(0),"note":label(1),"footer":label(2),"status":"本机统计",
    "refined":{
        "eyebrow":name(0),"headline":name(1),"metricValue":name(2),
        "screenEyebrow":screen_name(0),"screenHeadline":screen_name(1),"screenMetricValue":screen_name(2),
        "alignedEyebrow":aligned_name(0),"alignedHeadline":aligned_name(1),"alignedMetricValue":aligned_name(2),
        "metricLabel":number_column([
            compact(counts.input),compact(counts.input-counts.cached),
            compact(counts.cached),compact(counts.output),compact(counts.reasoning)
        ]),
        "metricUnit":caption,"caption":caption,"metricText":metric_glyphs(&cache_ratio),"note":amounts,
        "alignedMetricText":metric_glyphs(&cache_ratio),
        "footer":mix_strip(counts.input-counts.cached,total,input_percent),
        "n2":bar(2),"n3":if total==0 {-1} else {input_percent as i64}
    }})
}

#[cfg(test)]
mod tests {
    use super::{
        aligned_text, compact, details, metric_glyphs, mix_strip, model_label, number_column,
        proportional_levels, screen_text, stack_levels, ALIGNED_CHARS,
    };

    #[test]
    fn stacked_chart_keeps_small_nonzero_segments_visible() {
        assert_eq!(stack_levels([0, 0, 0], 100, 32), [0, 0, 0]);
        assert_eq!(stack_levels([0, 99, 1], 100, 32), [0, 30, 32]);
        assert_eq!(stack_levels([1, 997, 2], 1000, 100), [1, 98, 100]);
        assert_eq!(stack_levels([1, 1, 1], 10_000, 32), [1, 2, 3]);
    }

    #[test]
    fn refined_chart_preserves_total_height_and_zero() {
        assert_eq!(proportional_levels([1, 1, 1], 10_000), [0, 0, 0]);
        assert_eq!(proportional_levels([100, 199, 1], 400), [8, 24, 24]);
        assert_eq!(proportional_levels([0, 0, 400], 400), [0, 0, 32]);
    }

    #[test]
    fn screen_columns_and_mix_fit_the_existing_display_envelope() {
        assert_eq!(
            number_column(["0".into(), compact(u64::MAX), "—".into()]).len(),
            54
        );
        assert_eq!(mix_strip(0, 1, 100), "");
        assert_eq!(mix_strip(100, 100, 100).chars().count(), 34);
        assert_eq!(mix_strip(50, 100, 50).chars().count(), 17);
    }

    #[test]
    fn all_period_captions_fit_the_device_field() {
        for period in 0..=super::TODAY {
            let detail = details(None, period, "本机暂无记录");
            let caption = detail["refined"]["metricUnit"].as_str().unwrap();
            assert!(caption.len() <= 31, "{caption}");
            assert!(caption.ends_with(" · 本机"));
        }
    }

    #[test]
    fn today_uses_the_current_local_calendar_day_and_keeps_the_legacy_cycle() {
        use super::{Breakdown, Usage, View};
        use super::super::token_history::Tokens;
        use chrono::{Days, Local, TimeZone};
        use serde_json::json;

        let now = Local.with_ymd_and_hms(2026, 11, 20, 1, 30, 0).unwrap();
        let with_input = |input| Breakdown {
            counts: Tokens { input, ..Default::default() },
            recorded: true,
            ..Default::default()
        };
        let usage = Usage {
            days: std::array::from_fn(|i| (now.date_naive() - Days::new((6 - i) as u64), with_input(20))),
            periods: [with_input(5), with_input(90), with_input(130), with_input(200)],
        };
        let mut view = View::default();
        view.apply(Some("detail"), true);
        view.apply(Some("hour"), true);
        view.apply(Some("cycle_period"), true);
        let project = |view: View, at| view.project(&json!({}), Some(&usage), false, at);
        let day = project(view, now.timestamp_millis());
        assert_eq!(day["period"], "today");
        assert_eq!(day["s0"], "20");
        assert_eq!(project(view, now.timestamp_millis() + 86_400_000)["s0"], "—");
        view.apply(Some("cycle_period"), true);
        assert_eq!(project(view, now.timestamp_millis())["s0"], "90");
        view.apply(Some("hour"), false);
        view.apply(Some("cycle_period"), false);
        assert_eq!(project(view, now.timestamp_millis())["period"], "day");
    }

    #[test]
    fn model_labels_use_small_digits_and_fit_the_shortest_field() {
        assert_eq!(model_label("gpt-6.2-sol"), "\u{e000}gËt-ÆÊÂ-sol");
        assert_eq!(model_label("未识别模型"), "\u{e000}未识别模型");
        assert_eq!(model_label(""), "");
        assert!(model_label(&"1".repeat(32)).len() <= 63);
    }

    #[test]
    fn model_and_account_labels_share_the_small_metric_glyphs() {
        assert_eq!(screen_text("gpt-6.2-sol", 63), "gpt-ĆĊĂ-sol");
        assert_eq!(screen_text("gpy+2@mail.test", 127), "gpy+Ă@mailĊtest");
        assert_eq!(screen_text("查询中", 127), "查询中");
        assert_eq!(screen_text("g2中", 4), "gĂ");
    }

    #[test]
    fn mixed_labels_encode_every_visible_character() {
        for text in ["1/2 · gpy.agent+2@mail.test · 更新 02:15", "查询失败", "未识别模型",
                     "其他 / 未识别", "正在统计", "本机暂无记录"] {
            let glyphs = aligned_text(text, 127);
            assert_eq!(glyphs.chars().count(), text.chars().count());
            assert!(glyphs.chars().all(|c| (0x180..0x180 + ALIGNED_CHARS.chars().count() as u32).contains(&u32::from(c))));
        }
    }

    #[test]
    fn middle_size_values_fit_the_peak_field_when_rounding_crosses_a_digit() {
        for n in [0, 9_994_999, 9_999_999, 99_949_999, 99_999_999, u64::MAX] {
            let label = compact(n);
            assert!(metric_glyphs(&label).len() <= 15, "{label}");
        }
        assert_eq!(metric_glyphs("100%").len(), 12);
    }
}
