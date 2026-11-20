//! Local, account-attributed Codex usage. Only counters leave the desktop.
use chrono::{DateTime, Days, Local, NaiveDate, TimeZone, Utc};
use serde::{de::IgnoredAny, Deserialize};
use serde_json::Value;
use std::{
    collections::{BTreeMap, HashMap, HashSet},
    fs::{self, File},
    io::{self, BufRead, BufReader, Read, Seek, SeekFrom},
    path::{Path, PathBuf},
    sync::{Mutex, OnceLock},
    time::{Duration, Instant, SystemTime},
};

const HOUR: i64 = 3_600_000;
const DAY: i64 = 24 * HOUR;
pub const PERIODS: [i64; 4] = [HOUR, DAY, 7 * DAY, 30 * DAY];
const MAX_LINE_BYTES: usize = 256 * 1024;
const SCAN_BATCH_BYTES: usize = 64 * 1024 * 1024;

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct Owner {
    pub account: String,
    pub user: String,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct Tokens {
    pub input: u64,
    pub cached: u64,
    pub output: u64,
    pub reasoning: u64,
}

impl Tokens {
    fn read(value: &Value) -> Option<Self> {
        let tokens = Self {
            input: value["input_tokens"].as_u64()?,
            cached: value["cached_input_tokens"].as_u64()?,
            output: value["output_tokens"].as_u64()?,
            reasoning: value["reasoning_output_tokens"].as_u64()?,
        };
        (tokens.cached <= tokens.input
            && tokens.reasoning <= tokens.output
            && tokens.total() == value["total_tokens"].as_u64()?)
        .then_some(tokens)
    }

    pub fn total(self) -> u64 {
        self.input.saturating_add(self.output)
    }

    fn delta(self, previous: Self) -> Option<Self> {
        let delta = Self {
            input: self.input.checked_sub(previous.input)?,
            cached: self.cached.checked_sub(previous.cached)?,
            output: self.output.checked_sub(previous.output)?,
            reasoning: self.reasoning.checked_sub(previous.reasoning)?,
        };
        (delta.cached <= delta.input && delta.reasoning <= delta.output).then_some(delta)
    }

    fn add(&mut self, tokens: Self) {
        self.input = self.input.saturating_add(tokens.input);
        self.cached = self.cached.saturating_add(tokens.cached);
        self.output = self.output.saturating_add(tokens.output);
        self.reasoning = self.reasoning.saturating_add(tokens.reasoning);
    }
}

struct Sample {
    at: i64,
    total: Tokens,
    tokens: Tokens,
    model: Option<String>,
}

#[derive(Deserialize)]
struct Header {
    #[serde(rename = "type")]
    kind: String,
    payload: HeaderPayload,
}

#[derive(Deserialize)]
struct HeaderPayload {
    id: String,
    creator_account_id: String,
    creator_user_id: String,
    timestamp: String,
    account_scoped: bool,
    forked_from_id: Option<IgnoredAny>,
    history_base: Option<IgnoredAny>,
}

#[cfg(unix)]
type FileId = (u64, u64);
#[cfg(unix)]
fn file_id(metadata: &fs::Metadata) -> FileId {
    use std::os::unix::fs::MetadataExt;
    (metadata.dev(), metadata.ino())
}

#[cfg(not(unix))]
type FileId = Option<SystemTime>;
#[cfg(not(unix))]
fn file_id(metadata: &fs::Metadata) -> FileId {
    metadata.created().ok()
}

#[derive(Default)]
struct Session {
    id: String,
    owner: Option<Owner>,
    start: i64,
    offset: u64,
    file_id: Option<FileId>,
    observed_len: u64,
    modified: Option<SystemTime>,
    discarding_oversize_record: bool,
    last: Option<Tokens>,
    model: Option<String>,
    samples: Vec<Sample>,
}

fn valid_id(value: &str) -> bool {
    !value.is_empty()
        && value.len() <= 128
        && value
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, b'-' | b'_'))
}

fn model_name(value: &Value) -> Option<String> {
    let name = value.as_str()?;
    (!name.is_empty()
        && name.len() <= 32
        && name
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, b'-' | b'_' | b'.' | b' ')))
    .then(|| name.to_owned())
}

fn timestamp(value: &Value) -> Option<i64> {
    Some(
        DateTime::parse_from_rfc3339(value.as_str()?)
            .ok()?
            .timestamp_millis(),
    )
}

fn contains(line: &[u8], key: &[u8]) -> bool {
    memchr::memmem::find(line, key).is_some()
}

impl Session {
    fn read_header(&mut self, file: &mut File, len: u64, cutoff: i64) -> io::Result<()> {
        // Find a complete first line without buffering potentially large instructions.
        let mut reader = BufReader::new(file.take(len));
        let mut bytes = 0;
        loop {
            let chunk = reader.fill_buf()?;
            if chunk.is_empty() {
                return Ok(()); // A concurrent writer may still be appending the header.
            }
            if let Some(end) = memchr::memchr(b'\n', chunk) {
                bytes += end as u64 + 1;
                break;
            }
            let n = chunk.len();
            reader.consume(n);
            bytes += n as u64;
        }
        file.rewind()?;
        // Serde skips unknown fields, including instructions and tool definitions.
        let parsed = serde_json::from_reader::<_, Header>(BufReader::new(file.take(bytes)));
        let header = match parsed {
            Err(error) if error.is_io() => return Err(error.into()),
            other => other.ok(),
        };
        self.offset = bytes;
        if let Some(Header { kind, payload: h }) = header {
            if kind == "session_meta"
                && h.account_scoped
                && valid_id(&h.id)
                && valid_id(&h.creator_account_id)
                && valid_id(&h.creator_user_id)
            {
                let Ok(start) = DateTime::parse_from_rfc3339(&h.timestamp) else {
                    return Ok(());
                };
                self.start = start.timestamp_millis();
                self.id = h.id;
                self.owner = Some(Owner {
                    account: h.creator_account_id,
                    user: h.creator_user_id,
                });
                if h.forked_from_id.is_none() && h.history_base.is_none() && self.start >= cutoff {
                    self.last = Some(Tokens::default());
                }
            }
        }
        Ok(())
    }

    fn consume(&mut self, line: &[u8], cutoff: i64) {
        if !contains(line, b"\"token_count\"")
            && !contains(line, b"\"turn_context\"")
            && !contains(line, b"\"model_reroute\"")
        {
            return;
        }
        let Ok(entry) = serde_json::from_slice::<Value>(line) else {
            return;
        };
        let p = &entry["payload"];
        let Some(at) = timestamp(&entry["timestamp"]) else {
            return;
        };
        if entry["type"] == "turn_context" {
            self.model = model_name(&p["model"]);
        } else if p["type"] == "model_reroute" {
            self.model = model_name(&p["to_model"]);
        } else if entry["type"] == "event_msg" && p["type"] == "token_count" {
            let info = &p["info"];
            let Some(total) = Tokens::read(&info["total_token_usage"]) else {
                return;
            };
            let last = Tokens::read(&info["last_token_usage"]);
            let delta = self.last.and_then(|baseline| total.delta(baseline));
            self.last = Some(total);
            if at < self.start || at < cutoff {
                return;
            }
            // If a restored/forked session has no baseline yet, wait for a
            // second observation. Never add its inherited cumulative counters.
            let Some(tokens) = delta.filter(|v| v.total() > 0) else {
                return;
            };
            let model = (last == Some(tokens)).then(|| self.model.clone()).flatten();
            self.samples.push(Sample {
                at,
                total,
                tokens,
                model,
            });
        }
    }

    fn update(&mut self, path: &Path, cutoff: i64, scan_budget: &mut usize) -> io::Result<()> {
        let mut options = File::options();
        options.read(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            options.custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK);
        }
        if !fs::symlink_metadata(path)?.is_file() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "expected a session file",
            ));
        }
        let mut file = options.open(path)?;
        let metadata = file.metadata()?;
        if !metadata.is_file() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "expected a session file",
            ));
        }
        let id = file_id(&metadata);
        let modified = metadata.modified()?;
        let len = metadata.len();
        if self.file_id != Some(id)
            || len < self.observed_len
            || (self.modified.is_some()
                && self.modified != Some(modified)
                && len == self.observed_len)
        {
            *self = Self::default();
        }
        if self.modified == Some(modified) && self.observed_len == len {
            return Ok(());
        }
        self.file_id = Some(id);
        self.observed_len = len;
        self.modified = None;
        self.samples.retain(|sample| sample.at >= cutoff);
        if self.offset == 0 {
            self.read_header(&mut file, len, cutoff)?;
        }
        if self.owner.is_none() {
            self.modified = Some(modified);
            return Ok(());
        }
        file.seek(SeekFrom::Start(self.offset))?;
        // Files can grow while Codex is writing a turn. Read the observed
        // extent so a busy session cannot hold up the rest of the scan.
        let mut reader =
            BufReader::with_capacity(128 * 1024, file.take(metadata.len() - self.offset));
        let mut line = Vec::new();
        loop {
            if *scan_budget < MAX_LINE_BYTES {
                // Yield between batches without waiting for another UI poll.
                std::thread::sleep(Duration::from_millis(10));
                *scan_budget = SCAN_BATCH_BYTES;
            }
            line.clear();
            let bytes = reader
                .by_ref()
                .take(MAX_LINE_BYTES as u64)
                .read_until(b'\n', &mut line)?;
            *scan_budget -= bytes;
            if bytes == 0 {
                self.modified = Some(modified);
                break;
            }
            if self.discarding_oversize_record {
                self.discarding_oversize_record = line.last() != Some(&b'\n');
                self.offset += bytes as u64;
                continue;
            }
            if line.last() != Some(&b'\n') {
                if bytes < MAX_LINE_BYTES {
                    self.modified = Some(modified);
                    break;
                } // incomplete concurrent write
                self.discarding_oversize_record = true;
                self.offset += bytes as u64;
                continue;
            }
            self.consume(&line, cutoff);
            self.offset += bytes as u64;
        }
        Ok(())
    }
}

#[derive(Clone, Debug, Default)]
pub struct Breakdown {
    pub counts: Tokens,
    pub models: BTreeMap<String, u64>,
    pub recorded: bool,
}

impl Breakdown {
    fn add(&mut self, sample: &Sample) {
        self.recorded = true;
        self.counts.add(sample.tokens);
        let count = self
            .models
            .entry(sample.model.clone().unwrap_or_default())
            .or_default();
        *count = count.saturating_add(sample.tokens.total());
    }
}

#[derive(Clone, Debug)]
pub struct Usage {
    pub days: [(NaiveDate, Breakdown); 7],
    pub periods: [Breakdown; 4],
}

impl Usage {
    fn new(today: NaiveDate) -> Self {
        Self {
            days: std::array::from_fn(|i| (today - Days::new(6 - i as u64), Breakdown::default())),
            periods: std::array::from_fn(|_| Breakdown::default()),
        }
    }
}

fn scan_paths(
    dir: &Path,
    depth: u8,
    cutoff: SystemTime,
    files: &mut HashSet<PathBuf>,
) -> io::Result<()> {
    for entry in fs::read_dir(dir)? {
        let entry = entry?;
        let kind = entry.file_type()?;
        if kind.is_dir()
            && depth > 0
            && entry.file_name().to_str().is_some_and(|s| {
                s.len() == if depth == 3 { 4 } else { 2 } && s.bytes().all(|b| b.is_ascii_digit())
            })
        {
            scan_paths(&entry.path(), depth - 1, cutoff, files)?;
        } else if depth == 0
            && kind.is_file()
            && entry.path().extension().is_some_and(|e| e == "jsonl")
        {
            if entry.metadata()?.modified()? >= cutoff {
                files.insert(entry.path());
            }
        }
    }
    Ok(())
}

fn update_sessions(
    base: &Path,
    now: i64,
    sessions: &mut HashMap<PathBuf, Session>,
) -> io::Result<()> {
    let mut scan_budget = SCAN_BATCH_BYTES;
    let mut files = HashSet::new();
    let cutoff = SystemTime::now() - Duration::from_secs(31 * 86_400);
    for (name, depth) in [("sessions", 3), ("archived_sessions", 0)] {
        let dir = base.join(name);
        match fs::symlink_metadata(&dir) {
            Err(e) if e.kind() == io::ErrorKind::NotFound => continue,
            other => {
                other?;
            }
        }
        scan_paths(&dir, depth, cutoff, &mut files)?;
    }
    for path in &files {
        let session = sessions.entry(path.clone()).or_default();
        session.update(path, now - 31 * DAY, &mut scan_budget)?;
    }
    sessions.retain(|path, _| files.contains(path));
    Ok(())
}

fn aggregate(sessions: &HashMap<PathBuf, Session>, now: i64) -> HashMap<Owner, Usage> {
    let today = Utc
        .timestamp_millis_opt(now)
        .single()
        .unwrap()
        .with_timezone(&Local)
        .date_naive();
    let mut usage = HashMap::new();
    let mut seen = HashSet::new();
    for session in sessions.values() {
        let Some(owner) = &session.owner else {
            continue;
        };
        let usage: &mut Usage = usage
            .entry(owner.clone())
            .or_insert_with(|| Usage::new(today));
        for sample in &session.samples {
            if sample.at > now
                || !seen.insert((owner, session.id.as_str(), sample.at, sample.total))
            {
                continue;
            }
            for (period, window) in usage.periods.iter_mut().zip(PERIODS) {
                if sample.at > now - window {
                    period.add(sample);
                }
            }
            let Some(at) = DateTime::from_timestamp_millis(sample.at) else {
                continue;
            };
            let date = at.with_timezone(&Local).date_naive();
            if let Some((_, day)) = usage.days.iter_mut().find(|(d, _)| *d == date) {
                day.add(sample);
            }
        }
    }
    usage
}

#[derive(Default)]
struct History {
    busy: bool,
    last_launch: Option<Instant>,
    sessions: HashMap<PathBuf, Session>,
    usage: HashMap<Owner, Usage>,
}

static HISTORY: OnceLock<Mutex<History>> = OnceLock::new();

pub fn read(owner: &Owner, now: i64, refresh: bool) -> (Option<Usage>, bool) {
    let history = HISTORY.get_or_init(Default::default);
    let mut state = history.lock().unwrap_or_else(|p| p.into_inner());
    let interval = Duration::from_secs(if refresh { 5 } else { 30 });
    if !state.busy && state.last_launch.is_none_or(|at| at.elapsed() >= interval) {
        state.busy = true;
        state.last_launch = Some(Instant::now());
        std::thread::spawn(move || {
            let mut sessions = {
                let mut state = history.lock().unwrap_or_else(|p| p.into_inner());
                std::mem::take(&mut state.sessions)
            };
            let base = std::env::var_os("CODEX_HOME")
                .map(PathBuf::from)
                .or_else(|| {
                    std::env::var_os("HOME").map(|home| PathBuf::from(home).join(".codex"))
                });
            // A failed scan must not publish a smaller or partially updated total.
            let usage = base
                .filter(|base| update_sessions(base, now, &mut sessions).is_ok())
                .map(|_| aggregate(&sessions, Utc::now().timestamp_millis()));
            let mut state = history.lock().unwrap_or_else(|p| p.into_inner());
            state.sessions = sessions;
            if let Some(usage) = usage {
                state.usage = usage;
            }
            state.busy = false;
        });
    }
    (state.usage.get(owner).cloned(), state.busy)
}

#[cfg(test)]
mod tests {
    use super::*;

    use serde_json::json;
    use std::io::Write;

    fn event(at: i64, kind: &str, payload: Value) -> Value {
        json!({"timestamp": DateTime::from_timestamp_millis(at).unwrap().to_rfc3339(), "type":kind, "payload":payload})
    }

    fn usage(at: i64, total: u64, last: u64) -> Value {
        let tokens = |n| {
            json!({"input_tokens":n * 4,"cached_input_tokens":n * 2,
            "output_tokens":n,"reasoning_output_tokens":n,"total_tokens":n * 5})
        };
        event(
            at,
            "event_msg",
            json!({"type":"token_count",
            "info":{"last_token_usage":tokens(last),"total_token_usage":tokens(total)}}),
        )
    }

    #[test]
    fn counts_once_in_rolling_windows_and_only_for_the_exact_owner() {
        let now = 1_794_950_000_000;
        let log = tempfile::NamedTempFile::new().unwrap();
        let path = log.path();
        let start = now - 20 * DAY;
        let mut meta = event(
            start,
            "session_meta",
            json!({"id":"thread","creator_account_id":"a",
            "creator_user_id":"u","account_scoped":true,"model_provider":"custom"}),
        );
        meta["payload"]["timestamp"] = meta["timestamp"].clone();
        let context = event(start + 1, "turn_context", json!({"model":"gpt-test"}));
        let first = usage(now - 3 * HOUR, 6, 6);
        let second = usage(now - 30 * 60_000, 10, 4);
        let partial = usage(now - 5 * 60_000, 12, 2);
        let mut file = log.reopen().unwrap();
        for line in [meta, context, first, second.clone(), second] {
            writeln!(file, "{line}").unwrap();
        }
        write!(file, "{partial}").unwrap();
        let mut session = Session::default();
        let mut budget = SCAN_BATCH_BYTES;
        session.update(path, now - 31 * DAY, &mut budget).unwrap();
        assert_eq!(session.samples.len(), 2);
        writeln!(file).unwrap();
        session.update(path, now - 31 * DAY, &mut budget).unwrap();
        assert_eq!(session.samples.len(), 3);
        let mut copied = Session::default();
        copied.update(path, now - 31 * DAY, &mut budget).unwrap();
        let all = aggregate(
            &HashMap::from([(path.into(), session), ("copy".into(), copied)]),
            now,
        );
        let account = &all[&Owner {
            account: "a".into(),
            user: "u".into(),
        }];
        assert_eq!(
            account
                .periods
                .iter()
                .map(|period| period.counts.total())
                .collect::<Vec<_>>(),
            [30, 60, 60, 60]
        );
        assert!(!all.contains_key(&Owner {
            account: "a".into(),
            user: "other".into()
        }));
    }

    #[test]
    fn counts_all_sessions_and_long_logs_across_repeated_scans() {
        let home = tempfile::tempdir().unwrap();
        let active = home.path().join("sessions/2026/11/20");
        let archive = home.path().join("archived_sessions");
        fs::create_dir_all(&active).unwrap();
        fs::create_dir_all(&archive).unwrap();
        let now = Utc::now().timestamp_millis();
        let at = now - 60_000;
        let owner = Owner {
            account: "account".into(),
            user: "user".into(),
        };
        let header = |id: usize| {
            event(
                at,
                "session_meta",
                json!({
                    "timestamp": DateTime::from_timestamp_millis(at).unwrap().to_rfc3339(),
                    "id": format!("session-{id}"),
                    "creator_account_id": "account", "creator_user_id": "user", "account_scoped": true
                }),
            )
        };
        let first = usage(at, 1, 1);
        for id in 0..1024 {
            fs::write(
                active.join(format!("{id}.jsonl")),
                format!("{}\n{first}\n", header(id)),
            )
            .unwrap();
        }
        let path = archive.join("long.jsonl");
        let mut log = io::BufWriter::new(File::create(&path).unwrap());
        writeln!(log, "{}", header(1024)).unwrap();
        for n in 1..=24_001 {
            writeln!(log, "{}", usage(at, n, 1)).unwrap();
        }
        log.flush().unwrap();
        let mut sessions = HashMap::new();
        let expected = (1024 + 24_001) * 5;
        for total in [expected, expected, expected + 5] {
            if total > expected {
                writeln!(log, "{}", usage(at + 1, 24_002, 1)).unwrap();
                log.flush().unwrap();
            }
            update_sessions(home.path(), now, &mut sessions).unwrap();
            assert_eq!(sessions.len(), 1025);
            assert_eq!(
                aggregate(&sessions, now)[&owner].periods[3].counts.total(),
                total
            );
        }
    }

    fn session_log(owner: &str, n: u64, at: i64, padding: usize) -> String {
        let header = event(
            at,
            "session_meta",
            json!({
                "timestamp": DateTime::from_timestamp_millis(at).unwrap().to_rfc3339(),
                "id": "id", "creator_account_id": owner, "creator_user_id": "u",
                "account_scoped": true, "instructions": "x".repeat(padding)
            }),
        );
        format!("{}\n{}\n", header, usage(at, n, n))
    }
    #[test]
    fn replaced_log_is_attributed_to_new_owner() {
        let log = tempfile::tempdir().unwrap();
        let now = Utc::now().timestamp_millis();
        let p = log.path().join("log");
        fs::write(&p, session_log("a", 2, now, 0)).unwrap();
        let mut s = Session::default();
        let mut b = SCAN_BATCH_BYTES;
        s.update(&p, now - 31 * DAY, &mut b).unwrap();
        let replacement = log.path().join("replacement");
        fs::write(&replacement, session_log("b", 8, now, 0)).unwrap();
        fs::rename(replacement, &p).unwrap();
        s.update(&p, now - 31 * DAY, &mut b).unwrap();
        assert_eq!(s.owner.as_ref().unwrap().account, "b");
        assert_eq!(s.samples.len(), 1);
        assert_eq!(s.samples[0].tokens.total(), 40);
    }
    #[test]
    fn large_metadata_keeps_normal_following_counts() {
        let log = tempfile::NamedTempFile::new().unwrap();
        let now = Utc::now().timestamp_millis();
        let record = session_log("a", 2, now, MAX_LINE_BYTES);
        let split = record.find('\n').unwrap();
        fs::write(log.path(), &record[..split]).unwrap();
        let mut s = Session::default();
        let mut b = SCAN_BATCH_BYTES;
        s.update(log.path(), now - 31 * DAY, &mut b).unwrap();
        assert!(s.owner.is_none());
        assert_eq!(s.offset, 0);
        let mut writer = File::options().append(true).open(log.path()).unwrap();
        writer.write_all(record[split..].as_bytes()).unwrap();
        s.update(log.path(), now - 31 * DAY, &mut b).unwrap();
        assert_eq!(s.samples.len(), 1);
        assert_eq!(s.samples[0].tokens.total(), 10);
    }
    #[test]
    fn unreadable_session_root_keeps_last_complete_result() {
        let home = tempfile::tempdir().unwrap();
        let now = Utc::now().timestamp_millis();
        let archive = home.path().join("archived_sessions");
        fs::create_dir(&archive).unwrap();
        fs::write(archive.join("session.jsonl"), session_log("a", 2, now, 0)).unwrap();
        let mut sessions = HashMap::new();
        update_sessions(home.path(), now, &mut sessions).unwrap();
        assert_eq!(sessions.len(), 1);
        fs::rename(&archive, home.path().join("parked")).unwrap();
        fs::write(&archive, "temporarily not a directory").unwrap();
        assert!(update_sessions(home.path(), now, &mut sessions).is_err());
        assert_eq!(sessions.len(), 1);
        fs::remove_file(&archive).unwrap();
        fs::rename(home.path().join("parked"), &archive).unwrap();
        update_sessions(home.path(), now, &mut sessions).unwrap();
        assert_eq!(sessions.len(), 1);
        fs::remove_file(archive.join("session.jsonl")).unwrap();
        update_sessions(home.path(), now, &mut sessions).unwrap();
        assert!(sessions.is_empty());
    }
    #[test]
    fn fork_does_not_attribute_inherited_counters() {
        let now = 1_794_950_000_000;
        let log = tempfile::NamedTempFile::new().unwrap();
        let start = now - 60_000;
        let mut meta = event(
            start,
            "session_meta",
            json!({"id":"fork","creator_account_id":"a",
            "creator_user_id":"u","account_scoped":true,"forked_from_id":"parent"}),
        );
        meta["payload"]["timestamp"] = meta["timestamp"].clone();
        let mut file = log.reopen().unwrap();
        for line in [
            meta,
            usage(start - 10, 1_000, 100),
            usage(start + 1, 1_008, 8),
        ] {
            writeln!(file, "{line}").unwrap();
        }
        let mut session = Session::default();
        let mut budget = SCAN_BATCH_BYTES;
        session
            .update(log.path(), now - 31 * DAY, &mut budget)
            .unwrap();
        assert_eq!(session.samples.len(), 1);
        assert_eq!(session.samples[0].tokens.total(), 40);
        assert!(session.samples[0].model.is_none());
    }

    #[test]
    fn continues_in_the_same_scan_when_an_oversize_record_crosses_a_batch() {
        let now = 1_794_950_000_000;
        let log = tempfile::NamedTempFile::new().unwrap();
        let mut meta = event(
            now - DAY,
            "session_meta",
            json!({"id":"thread","creator_account_id":"a","creator_user_id":"u","account_scoped":true}),
        );
        meta["payload"]["timestamp"] = meta["timestamp"].clone();
        let mut file = log.reopen().unwrap();
        writeln!(file, "{meta}").unwrap();
        file.write_all(&vec![b'x'; 2 * MAX_LINE_BYTES]).unwrap();
        writeln!(file).unwrap();
        writeln!(file, "{}", usage(now - 1, 1, 1)).unwrap();

        let mut session = Session::default();
        let mut budget = 2 * MAX_LINE_BYTES;
        session
            .update(log.path(), now - 31 * DAY, &mut budget)
            .unwrap();
        assert!(budget > 2 * MAX_LINE_BYTES);
        assert!(!session.discarding_oversize_record);
        assert_eq!(session.samples.len(), 1);
        assert_eq!(session.samples[0].tokens.total(), 5);
    }
}
