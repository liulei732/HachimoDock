//! Native account-management service. The IPC accepts actions, never paths,
//! executable names, client IDs, bearer tokens, or arbitrary endpoints.
use super::*;
#[path = "account_browser.rs"]
mod browser;
use serde::Deserialize;
use std::{
    io::{BufRead, BufReader},
    process::{Child, Command, Stdio},
    sync::{
        atomic::{AtomicBool, Ordering},
        mpsc, Arc,
    },
};

static MANAGER: Mutex<()> = Mutex::new(());
static LOGIN: OnceLock<Mutex<Option<Login>>> = OnceLock::new();
fn login() -> &'static Mutex<Option<Login>> {
    LOGIN.get_or_init(|| Mutex::new(None))
}
struct Login {
    id: String,
    slot: u8,
    phase: &'static str,
    url: Option<String>,
    error: Option<String>,
    browser_opened: bool,
    browser_error: Option<String>,
    cancel: Arc<AtomicBool>,
}

#[derive(Deserialize)]
#[serde(
    tag = "operation",
    rename_all = "camelCase",
    rename_all_fields = "camelCase",
    deny_unknown_fields
)]
pub enum Input {
    Status,
    Configure {
        revision: u64,
        order: Vec<u8>,
        visible: Vec<u8>,
    },
    Refresh,
    Begin {
        revision: u64,
        slot: Option<u8>,
    },
    Cancel {
        login_id: String,
    },
    OpenLogin {
        login_id: String,
    },
    Remove {
        revision: u64,
        slot: u8,
        confirm: bool,
    },
}
fn busy_login() -> bool {
    login()
        .lock()
        .unwrap_or_else(|p| p.into_inner())
        .as_ref()
        .is_some_and(|s| {
            matches!(
                s.phase,
                "preparing" | "pending" | "cancelling" | "committing"
            )
        })
}
fn status(root: &Path) -> Result<Value, String> {
    let s = settings::load(root)?;
    let now = chrono::Utc::now().timestamp_millis();
    let rows:Vec<Value>=s.order.iter().map(|slot| {
        let v=cached(*slot,now);let saved=identity(&root.join(slot.to_string())).is_some();
        json!({"slot":slot,"email":if saved {v["email"].clone()} else {Value::Null},"loginSaved":saved,
            "visible":s.visible.contains(slot),"plan":v["plan"],"observedAt":v["observedAt"],
            "status":v["status"],"primary":v["primary"]["remaining"],"secondary":v["secondary"]["remaining"]})
    }).collect();
    let pending = login().lock().map_err(|_| "登录服务暂不可用")?;
    let public = pending
        .as_ref()
        .map(|s| json!({"id":s.id,"slot":s.slot,"phase":s.phase,"canOpen":s.phase=="pending"&&s.url.is_some(),"browserOpened":s.browser_opened,"browserError":s.browser_error,"error":s.error}));
    Ok(json!({"settings":s,"accounts":rows,"maxAccounts":settings::MAX_ACCOUNTS,"login":public}))
}
pub fn request(input: Value) -> Result<Value, String> {
    if serde_json::to_vec(&input)
        .map_err(|_| "账号操作无效")?
        .len()
        > 4096
    {
        return Err("账号操作过大".into());
    }
    let fields: &[&str] = match input["operation"].as_str() {
        Some("status" | "refresh") => &["operation"],
        Some("configure") => &["operation", "revision", "order", "visible"],
        Some("begin") => &["operation", "revision", "slot"],
        Some("openLogin") => &["operation", "loginId"],
        Some("cancel") => &["operation", "loginId"],
        Some("remove") => &["operation", "revision", "slot", "confirm"],
        _ => return Err("未知账号操作".into()),
    };
    if input
        .as_object()
        .is_none_or(|v| v.keys().any(|key| !fields.contains(&key.as_str())))
    {
        return Err("账号操作包含未知参数".into());
    }
    let input: Input = serde_json::from_value(input).map_err(|_| "账号操作或参数无效")?;
    let _manager = MANAGER.lock().map_err(|_| "账号管理暂不可用")?;
    let root = root().ok_or("账号目录不可用")?;
    // Reject a redirected root before reading login metadata.
    if fs::symlink_metadata(&root).is_ok_and(|m| !m.is_dir() || m.file_type().is_symlink()) {
        return Err("账号目录不可用".into());
    }
    match input {
        Input::Status => {}
        Input::Configure {
            revision,
            order,
            visible,
        } => {
            if busy_login() {
                return Err("正在登录，请完成或取消后修改账号列表".into());
            }
            let mut s = settings::load(&root)?;
            s.replace_display(revision, order, visible)?;
            settings::save(&root, &s)?;
        }
        Input::Refresh => {
            let s = settings::load(&root)?;
            for slot in s.order {
                if identity(&root.join(slot.to_string())).is_some() {
                    snapshot(slot, chrono::Utc::now().timestamp_millis(), true);
                }
            }
        }
        Input::Begin { revision, slot } => begin(&root, revision, slot)?,
        Input::OpenLogin { login_id } => open_pending_login(&login_id, &browser::open_chrome)?,
        Input::Cancel { login_id } => {
            let mut pending = login().lock().map_err(|_| "登录服务暂不可用")?;
            let active = pending
                .as_mut()
                .filter(|s| s.id == login_id)
                .ok_or("登录请求已结束，请刷新")?;
            if active.phase == "committing" {
                return Err("登录正在保存，请稍后查看结果".into());
            }
            if matches!(active.phase, "preparing" | "pending") {
                active.cancel.store(true, Ordering::SeqCst);
                active.phase = "cancelling";
                active.url = None;
            }
        }
        Input::Remove {
            revision,
            slot,
            confirm,
        } => {
            if !confirm {
                return Err("移除账号需要确认".into());
            }
            if busy_login() {
                return Err("正在登录，请完成或取消后移除账号".into());
            }
            let mut s = settings::load(&root)?;
            if s.revision != revision || !s.order.contains(&slot) {
                return Err("账号列表已更新，请刷新后重试".into());
            }
            let next_revision = s.revision.checked_add(1).ok_or("设置修订已达上限")?;
            let _write = AUTH_WRITE.lock().map_err(|_| "登录状态暂不可用")?;
            settings::protected_directory(&root)?;
            let home = root.join(slot.to_string());
            if fs::symlink_metadata(&home).is_ok_and(|m| !m.is_dir() || m.file_type().is_symlink())
            {
                return Err("账号目录不可用".into());
            }
            // Recoverable local removal: preserve old login in a private archive.
            let archive = tempfile::Builder::new()
                .prefix(".removed-")
                .tempdir_in(&root)
                .map_err(|_| "无法准备移除账号")?;
            settings::protected_directory(archive.path())?;
            let moved = home.exists();
            if moved {
                fs::rename(&home, archive.path().join("account"))
                    .map_err(|_| "无法移除账号，原设置保留")?;
            }
            s.revision = next_revision;
            s.order.retain(|s| *s != slot);
            s.visible.retain(|s| *s != slot);
            if let Err(e) = settings::save(&root, &s) {
                if moved && fs::rename(archive.path().join("account"), &home).is_err() {
                    let _ = archive.keep();
                    return Err("设置未保存，登录备份已保留，请重新检查账号列表".into());
                }
                return Err(e);
            }
            retire_account(&home);
            if moved {
                let _ = archive.keep();
            }
        }
    }
    status(&root)
}

pub(super) fn executable() -> Result<PathBuf, String> {
    // A package cannot override the executable or its arguments.
    for p in [
        "/Applications/ChatGPT.app/Contents/Resources/codex-cli/CodexCLI.app/Contents/MacOS/codex",
        "/Applications/Codex.app/Contents/Resources/codex",
    ] {
        if Path::new(p).is_file() {
            return Ok(p.into());
        }
    }
    if let Some(paths) = std::env::var_os("PATH") {
        for directory in std::env::split_paths(&paths) {
            let p = directory.join(if cfg!(windows) { "codex.exe" } else { "codex" });
            if p.is_file() {
                return Ok(p);
            }
        }
    }
    Err("未找到 Codex 登录工具，请先安装 Codex".into())
}
fn auth_bytes(home: &Path) -> Result<Option<Vec<u8>>, String> {
    if fs::symlink_metadata(home).is_ok_and(|m| !m.is_dir() || m.file_type().is_symlink()) {
        return Err("账号目录不可用".into());
    }
    let path = home.join("auth.json");
    match fs::symlink_metadata(&path) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(_) => Err("无法读取登录信息".into()),
        Ok(m) => {
            if !m.is_file() || m.file_type().is_symlink() || m.len() > 65536 {
                return Err("登录信息无效，原账号保留".into());
            }
            let mut raw = Vec::new();
            std::io::Read::take(fs::File::open(path).map_err(|_| "无法读取登录信息")?, 65537)
                .read_to_end(&mut raw)
                .map_err(|_| "无法读取登录信息")?;
            if raw.len() > 65536 {
                return Err("登录信息过大，原账号保留".into());
            }
            Ok(Some(raw))
        }
    }
}
fn target_slot(root: &Path, s: &settings::Settings, requested: Option<u8>) -> Result<u8, String> {
    let slot = if let Some(slot) = requested {
        if !s.order.contains(&slot) {
            return Err("账号已被移除，请刷新后重试".into());
        }
        slot
    } else {
        s.order
            .iter()
            .copied()
            .find(|slot| identity(&root.join(slot.to_string())).is_none())
            .or_else(|| {
                (1..=settings::MAX_ACCOUNTS).find(|slot| {
                    !s.order.contains(slot)
                        && auth_bytes(&root.join(slot.to_string())).is_ok_and(|v| v.is_none())
                })
            })
            .ok_or("账号已达到 8 个，请先移除不再使用的账号")?
    };
    Ok(slot)
}
fn begin(root: &Path, revision: u64, requested: Option<u8>) -> Result<(), String> {
    if busy_login() {
        return Err("已有登录正在进行，请先完成或取消".into());
    }
    let s = settings::load(root)?;
    if s.revision != revision {
        return Err("账号列表已更新，请刷新后重试".into());
    }
    let slot = target_slot(root, &s, requested)?;
    let binary = executable()?;
    settings::protected_directory(root)?;
    let staged = tempfile::Builder::new()
        .prefix(".login-")
        .tempdir_in(root)
        .map_err(|_| "无法准备独立登录")?;
    settings::protected_directory(staged.path())?;
    let id = staged
        .path()
        .file_name()
        .unwrap()
        .to_string_lossy()
        .into_owned();
    let expected = identity(&root.join(slot.to_string())).map(|i| i.id);
    let original = auth_bytes(&root.join(slot.to_string()))?;
    let cancel = Arc::new(AtomicBool::new(false));
    *login().lock().map_err(|_| "登录服务暂不可用")? = Some(Login {
        id: id.clone(),
        slot,
        phase: "preparing",
        url: None,
        error: None,
        browser_opened: false,
        browser_error: None,
        cancel: cancel.clone(),
    });
    let root = root.to_path_buf();
    std::thread::spawn(move || {
        let result = login_worker(&binary, staged.path(), &id, &cancel);
        if result.is_ok() && !cancel.load(Ordering::SeqCst) {
            let commit = commit_login(
                &root,
                staged.path(),
                slot,
                &id,
                revision,
                expected.as_deref(),
                original.as_deref(),
                &cancel,
            );
            finish(&id, commit);
        } else {
            finish(&id, result);
        }
        // TempDir removes only this attempt's isolated login, including on error.
    });
    Ok(())
}
fn finish(id: &str, result: Result<(), String>) {
    if let Ok(mut pending) = login().lock() {
        if let Some(s) = pending.as_mut().filter(|s| s.id == id) {
            s.url = None;
            if s.cancel.load(Ordering::SeqCst) {
                s.phase = "cancelled";
                s.error = None;
            } else {
                match result {
                    Ok(()) => {
                        s.phase = "completed";
                        s.error = None;
                    }
                    Err(e) => {
                        s.phase = "error";
                        s.error = Some(e);
                    }
                }
            }
        }
    }
}
struct Process(Child);
impl Drop for Process {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}
fn send(child: &mut Child, value: Value) -> Result<(), String> {
    let input = child.stdin.as_mut().ok_or("登录工具已退出")?;
    input
        .write_all(&serde_json::to_vec(&value).map_err(|_| "登录请求无效")?)
        .and_then(|_| input.write_all(b"\n"))
        .and_then(|_| input.flush())
        .map_err(|_| "登录工具已退出".into())
}
fn login_worker(binary: &Path, staged: &Path, id: &str, cancel: &AtomicBool) -> Result<(), String> {
    login_worker_with_launcher(binary, staged, id, cancel, &browser::open_chrome)
}
fn open_pending_login(
    id: &str,
    launcher: &dyn Fn(&str) -> Result<(), String>,
) -> Result<(), String> {
    let mut pending = login().lock().map_err(|_| "登录服务暂不可用")?;
    let active = pending
        .as_mut()
        .filter(|s| s.id == id && s.phase == "pending" && !s.cancel.load(Ordering::SeqCst))
        .ok_or("登录请求已结束，请重新开始")?;
    let url = active
        .url
        .as_deref()
        .filter(|url| browser::valid_url(url))
        .ok_or("登录页面无效")?;
    let result = launcher(url);
    active.browser_opened = result.is_ok();
    active.browser_error = result.as_ref().err().cloned();
    result
}
fn login_worker_with_launcher(
    binary: &Path,
    staged: &Path,
    id: &str,
    cancel: &AtomicBool,
    launcher: &dyn Fn(&str) -> Result<(), String>,
) -> Result<(), String> {
    let child = Command::new(binary)
        .args([
            "app-server",
            "--stdio",
            "-c",
            "cli_auth_credentials_store=\"file\"",
        ])
        .env("CODEX_HOME", staged)
        .env_remove("OPENAI_API_KEY")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .map_err(|_| "无法启动 Codex 登录工具")?;
    let mut process = Process(child);
    let stdout = process.0.stdout.take().ok_or("登录工具无输出")?;
    let (tx, rx) = mpsc::sync_channel(8);
    std::thread::spawn(move || {
        let mut reader = BufReader::new(stdout);
        loop {
            let mut line = Vec::new();
            let read = std::io::Read::take(&mut reader, 65537).read_until(b'\n', &mut line);
            if !matches!(read,Ok(n) if n>0) || line.len() > 65536 {
                break;
            }
            let Ok(value) = serde_json::from_slice::<Value>(&line) else {
                continue;
            };
            if tx.send(value).is_err() {
                break;
            }
        }
    });
    send(
        &mut process.0,
        json!({"id":1,"method":"initialize","params":{"clientInfo":{"name":"pet-manager-quota","version":"1.4.0"},"capabilities":{"experimentalApi":false}}}),
    )?;
    let start = Instant::now();
    let mut initialized = false;
    let mut begun = false;
    loop {
        if cancel.load(Ordering::SeqCst) {
            return Err("登录已取消".into());
        }
        if start.elapsed() > Duration::from_secs(if begun { 600 } else { 30 }) {
            return Err("登录已超时，请重新开始".into());
        }
        let value = match rx.recv_timeout(Duration::from_millis(200)) {
            Ok(v) => v,
            Err(mpsc::RecvTimeoutError::Timeout) => continue,
            Err(_) => return Err("登录工具已退出，请重试".into()),
        };
        if value.get("error").is_some() {
            return Err("登录服务拒绝请求，请重试".into());
        }
        if value["id"] == 1 && !initialized {
            initialized = true;
            send(&mut process.0, json!({"method":"initialized"}))?;
            send(
                &mut process.0,
                json!({"id":2,"method":"account/login/start","params":{"type":"chatgpt"}}),
            )?;
        } else if value["id"] == 2 && initialized && !begun {
            let raw = value["result"]["authUrl"]
                .as_str()
                .ok_or("未收到登录页面")?;
            if !browser::valid_url(raw) {
                return Err("登录页面无效".into());
            }
            let mut pending = login().lock().map_err(|_| "登录服务暂不可用")?;
            let s = pending
                .as_mut()
                .filter(|s| s.id == id)
                .ok_or("登录请求已取消")?;
            if cancel.load(Ordering::SeqCst) {
                return Err("登录已取消".into());
            }
            s.url = Some(raw.into());
            s.phase = "pending";
            begun = true;
            drop(pending);
            // Only an explicit login attempt opens Chrome; polls never do.
            // Failure leaves this pending attempt available for a manual retry.
            let _ = open_pending_login(id, launcher);
        } else if value["method"] == "account/login/completed" && begun {
            return if value["params"]["success"] == true {
                Ok(())
            } else {
                Err("登录未完成，请重试".into())
            };
        }
    }
}
fn commit_login(
    root: &Path,
    staged: &Path,
    slot: u8,
    id: &str,
    revision: u64,
    expected: Option<&str>,
    original: Option<&[u8]>,
    cancel: &AtomicBool,
) -> Result<(), String> {
    let _manager = MANAGER.lock().map_err(|_| "账号管理暂不可用")?;
    let _write = AUTH_WRITE.lock().map_err(|_| "登录状态暂不可用")?;
    let identity = identity(staged).ok_or("登录信息无效，原账号保留")?;
    if expected.is_some_and(|v| identity.id != v) {
        return Err("登录了另一个账号，原账号保留；请使用原账号重新授权".into());
    }
    let mut settings = settings::load(root)?;
    if settings.revision != revision {
        return Err("账号列表已变更，原账号保留，请重新登录".into());
    }
    if settings
        .order
        .iter()
        .filter(|s| **s != slot)
        .any(|s| super::identity(&root.join(s.to_string())).is_some_and(|i| i.id == identity.id))
    {
        return Err("这个账号已经在列表中，未重复添加".into());
    }
    let home = root.join(slot.to_string());
    if auth_bytes(&home)?.as_deref() != original {
        return Err("原登录信息已更新，请重新授权".into());
    }
    // One lock linearizes cancellation against the credential replacement.
    let mut pending = login().lock().map_err(|_| "登录服务暂不可用")?;
    let active = pending
        .as_mut()
        .filter(|s| s.id == id)
        .ok_or("登录请求已结束")?;
    if cancel.load(Ordering::SeqCst) {
        return Err("登录已取消".into());
    }
    active.phase = "committing";
    let raw = auth_bytes(staged)?.ok_or("无法读取登录结果")?;
    settings::protected_directory(&home)?;
    let mut file = tempfile::NamedTempFile::new_in(&home).map_err(|_| "无法保存登录信息")?;
    file.write_all(&raw)
        .and_then(|_| file.as_file().sync_all())
        .map_err(|_| "无法保存登录信息")?;
    if !settings.order.contains(&slot) {
        settings.order.push(slot);
    }
    // New/replaced identities require an explicit display choice, which lets
    // the PC check that the current device package can render the new email.
    if expected.is_none() {
        settings.visible.retain(|s| *s != slot);
    }
    settings.revision = settings.revision.checked_add(1).ok_or("设置修订已达上限")?;
    file.persist(home.join("auth.json"))
        .map_err(|_| "无法保存登录信息")?;
    if let Err(e) = settings::save(root, &settings) {
        // Keep successful auth isolated rather than exposing it under old config.
        if let Some(old) = original {
            let mut restore = tempfile::NamedTempFile::new_in(&home)
                .map_err(|_| "登录保存状态未知，请检查账号列表")?;
            restore
                .write_all(old)
                .and_then(|_| restore.as_file().sync_all())
                .map_err(|_| "登录保存状态未知，请检查账号列表")?;
            restore
                .persist(home.join("auth.json"))
                .map_err(|_| "登录保存状态未知，请检查账号列表")?;
        } else {
            fs::remove_file(home.join("auth.json"))
                .map_err(|_| "登录保存状态未知，请检查账号列表")?;
        }
        return Err(e);
    }
    retire_account(&home);
    Ok(())
}

#[cfg(all(test, windows))]
pub(crate) use tests::assert_no_account_writes;

#[cfg(test)]
mod tests {
    use super::*;
    fn auth(home: &Path, id: &str, email: &str) -> Vec<u8> {
        settings::protected_directory(home).unwrap();
        let claims = json!({"email":email,"https://api.openai.com/auth":{"chatgpt_account_id":id}});
        let jwt = format!(
            "e30.{}.signature",
            base64::engine::general_purpose::URL_SAFE_NO_PAD
                .encode(serde_json::to_vec(&claims).unwrap())
        );
        let bytes=serde_json::to_vec(&json!({"auth_mode":"chatgpt","tokens":{"account_id":id,"id_token":jwt,"access_token":"fixture-private-access","refresh_token":"fixture-private-refresh"}})).unwrap();
        fs::write(home.join("auth.json"), &bytes).unwrap();
        bytes
    }
    static TEST_ENV: Mutex<()> = Mutex::new(());
    struct Env {
        _guard: std::sync::MutexGuard<'static, ()>,
        previous: Option<std::ffi::OsString>,
        dir: tempfile::TempDir,
    }
    impl Env {
        fn new() -> Self {
            let guard = TEST_ENV.lock().unwrap_or_else(|e| e.into_inner());
            let dir = tempfile::tempdir().unwrap();
            let previous = std::env::var_os("HACHIMO_CODEX_ACCOUNTS_HOME");
            std::env::set_var("HACHIMO_CODEX_ACCOUNTS_HOME", dir.path());
            *login().lock().unwrap() = None;
            Self { previous, dir, _guard: guard }
        }
        fn root(&self) -> &Path {
            self.dir.path()
        }
    }
    impl Drop for Env {
        fn drop(&mut self) {
            *login().lock().unwrap() = None;
            if let Some(value) = &self.previous {
                std::env::set_var("HACHIMO_CODEX_ACCOUNTS_HOME", value)
            } else {
                std::env::remove_var("HACHIMO_CODEX_ACCOUNTS_HOME")
            }
        }
    }
    #[cfg(windows)]
    pub(crate) fn assert_no_account_writes(check: impl FnOnce()) {
        let env = Env::new();
        check();
        assert!(std::fs::read_dir(env.root()).unwrap().next().is_none());
    }

    #[cfg(windows)]
    #[test]
    fn windows_rejects_quota_snapshots_and_actions_without_writing_accounts() {
        assert_no_account_writes(|| {
            let mut selection = 2;
            for action in [None, Some("refresh"), Some("next"), Some("previous")] {
                let result = crate::local_telemetry::snapshot("codex.accounts", &mut selection, 0, action);
                assert_eq!(result.unwrap_err(), "Codex 额度账号管理需要 macOS");
                assert_eq!(selection, 2);
            }
        });
    }

    fn pending(id: &str) -> Arc<AtomicBool> {
        let cancel = Arc::new(AtomicBool::new(false));
        *login().lock().unwrap() = Some(Login {
            id: id.into(),
            slot: 1,
            phase: "pending",
            url: None,
            error: None,
            browser_opened: false,
            browser_error: None,
            cancel: cancel.clone(),
        });
        cancel
    }
    #[cfg(unix)]
    #[test]
    fn status_is_metadata_only_and_does_not_query_or_modify_auth() {
        let t = Env::new();
        let raw = auth(&t.root().join("1"), "account-secret-id", "a@example.com");
        let v = request(json!({"operation":"status"})).unwrap();
        assert_eq!(v["accounts"][0]["email"], "a@example.com");
        assert_eq!(v["accounts"][0]["plan"], "待获取");
        assert!(v["accounts"][0]["loginSaved"].as_bool().unwrap());
        assert!(!v.to_string().contains("fixture-private"));
        assert!(!v.to_string().contains("account-secret-id"));
        assert_eq!(fs::read(t.root().join("1/auth.json")).unwrap(), raw);
        assert!(!t.root().join("display-settings.json").exists());
    }
    #[cfg(unix)]
    #[test]
    fn configured_visibility_and_order_drive_selection_and_clear_all_quota() {
        let t = Env::new();
        let mut slot = 1;
        request(json!({"operation":"configure","revision":0,"order":[2,1],"visible":[2]})).unwrap();
        let v = managed_snapshot(&mut slot, 1794816000000, None).unwrap();
        assert_eq!(slot, 2);
        assert_eq!(v["identity"], "1/1 · 未登录");
        request(json!({"operation":"configure","revision":1,"order":[2,1],"visible":[]})).unwrap();
        let v = managed_snapshot(&mut slot, 1794816000000, Some("next")).unwrap();
        assert_eq!(v["accountCount"], 0);
        assert_eq!(v["primary"]["remaining"], -1);
        assert_eq!(v["secondary"]["remaining"], -1);
        assert!(
            request(json!({"operation":"configure","revision":0,"order":[1,2],"visible":[1]}))
                .is_err()
        );
        assert_eq!(settings::load(t.root()).unwrap().visible, Vec::<u8>::new());
    }
    #[cfg(unix)]
    #[test]
    fn remove_requires_confirmation_and_keeps_an_owner_only_recovery_copy() {
        let t = Env::new();
        let raw = auth(&t.root().join("1"), "one", "a@example.com");
        assert!(
            request(json!({"operation":"remove","revision":0,"slot":1,"confirm":false})).is_err()
        );
        assert!(t.root().join("1/auth.json").exists());
        let v =
            request(json!({"operation":"remove","revision":0,"slot":1,"confirm":true})).unwrap();
        assert_eq!(v["settings"]["order"], json!([2]));
        assert!(!t.root().join("1").exists());
        let archive = fs::read_dir(t.root())
            .unwrap()
            .map(|e| e.unwrap().path())
            .find(|p| {
                p.file_name()
                    .unwrap()
                    .to_string_lossy()
                    .starts_with(".removed-")
            })
            .unwrap();
        assert_eq!(fs::read(archive.join("account/auth.json")).unwrap(), raw);
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(
                fs::metadata(archive).unwrap().permissions().mode() & 0o777,
                0o700
            );
        }
    }
    #[cfg(unix)]
    #[test]
    fn wrong_account_duplicate_and_cancelled_logins_preserve_original_auth() {
        let t = Env::new();
        let home = t.root().join("1");
        let raw = auth(&home, "one", "a@example.com");
        auth(&t.root().join("2"), "two", "b@example.com");
        let staged = tempfile::tempdir_in(t.root()).unwrap();
        auth(staged.path(), "other", "c@example.com");
        let cancel = pending("test");
        assert!(commit_login(
            t.root(),
            staged.path(),
            1,
            "test",
            0,
            Some("one"),
            Some(&raw),
            &cancel
        )
        .unwrap_err()
        .contains("另一个账号"));
        auth(staged.path(), "two", "b@example.com");
        assert!(commit_login(
            t.root(),
            staged.path(),
            1,
            "test",
            0,
            None,
            Some(&raw),
            &cancel
        )
        .unwrap_err()
        .contains("已经在列表"));
        auth(staged.path(), "one", "a@example.com");
        cancel.store(true, Ordering::SeqCst);
        assert!(commit_login(
            t.root(),
            staged.path(),
            1,
            "test",
            0,
            Some("one"),
            Some(&raw),
            &cancel
        )
        .is_err());
        assert_eq!(fs::read(home.join("auth.json")).unwrap(), raw);
    }
    #[cfg(unix)]
    #[test]
    fn same_account_reauth_commits_privately_and_rejects_a_racing_auth_change() {
        let t = Env::new();
        let home = t.root().join("1");
        let raw = auth(&home, "one", "a@example.com");
        let staged = tempfile::tempdir_in(t.root()).unwrap();
        let next = auth(staged.path(), "one", "a@example.com");
        let cancel = pending("test");
        fs::write(home.join("auth.json"), b"changed independently").unwrap();
        assert!(commit_login(
            t.root(),
            staged.path(),
            1,
            "test",
            0,
            Some("one"),
            Some(&raw),
            &cancel
        )
        .is_err());
        fs::write(home.join("auth.json"), &raw).unwrap();
        commit_login(
            t.root(),
            staged.path(),
            1,
            "test",
            0,
            Some("one"),
            Some(&raw),
            &cancel,
        )
        .unwrap();
        assert_eq!(fs::read(home.join("auth.json")).unwrap(), next);
        assert_eq!(settings::load(t.root()).unwrap().revision, 1);
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(
                fs::metadata(home.join("auth.json"))
                    .unwrap()
                    .permissions()
                    .mode()
                    & 0o777,
                0o600
            );
        }
    }
    #[cfg(unix)]
    #[test]
    fn new_accounts_are_added_without_silently_enabling_unknown_device_characters() {
        let t = Env::new();
        let staged = tempfile::tempdir_in(t.root()).unwrap();
        auth(staged.path(), "new", "new@example.com");
        let cancel = pending("test");
        commit_login(t.root(), staged.path(), 3, "test", 0, None, None, &cancel).unwrap();
        let s = settings::load(t.root()).unwrap();
        assert_eq!(s.order, vec![1, 2, 3]);
        assert_eq!(s.visible, vec![1, 2]);
        assert_eq!(s.revision, 1);
    }
    #[cfg(unix)]
    #[test]
    fn empty_slot_left_by_an_interrupted_save_can_be_reused_without_overwriting_a_login() {
        let t = Env::new();
        auth(&t.root().join("1"), "one", "a@example.com");
        auth(&t.root().join("2"), "two", "b@example.com");
        fs::create_dir(t.root().join("3")).unwrap();
        assert_eq!(
            target_slot(t.root(), &settings::Settings::default(), None).unwrap(),
            3
        );
        fs::write(t.root().join("3/auth.json"), "corrupt existing auth").unwrap();
        assert_eq!(
            target_slot(t.root(), &settings::Settings::default(), None).unwrap(),
            4
        );
        assert!(target_slot(t.root(), &settings::Settings::default(), Some(9)).is_err());
    }
    #[cfg(unix)]
    #[test]
    fn no_selected_accounts_never_leak_the_previously_cached_email_or_membership() {
        let t = Env::new();
        let now = 1794816000000;
        auth(&t.root().join("1"), "one", "a@example.com");
        let home = t.root().join("1");
        let who = super::super::identity(&home).unwrap();
        let key = cache_key(&home, &who);
        cache().lock().unwrap().insert(key.clone(),Entry{attempt:Instant::now(),busy:false,error:None,credential_revision:who.key.clone(),request_id:0,value:Some((now,json!({"plan_type":"plus","primary":{"used_percent":25,"window_minutes":300,"resets_at":now/1000+3600}})))});
        let mut selected = 1;
        let before = managed_snapshot(&mut selected, now, None).unwrap();
        assert_eq!(before["account"], "Plus");
        assert_eq!(before["primary"]["remaining"], 75);
        request(json!({"operation":"configure","revision":0,"order":[1,2],"visible":[]})).unwrap();
        let after = managed_snapshot(&mut selected, now, None).unwrap();
        assert_eq!(after["account"], "待获取");
        assert_eq!(after["email"], "未登录");
        assert_eq!(after["primary"]["remaining"], -1);
        assert!(!after.to_string().contains("a@example.com"));
        cache().lock().unwrap().remove(&key);
    }
    #[cfg(unix)]
    #[test]
    fn login_claims_must_match_the_saved_account_identifier() {
        let t = Env::new();
        let home = t.root().join("1");
        auth(&home, "one", "a@example.com");
        let mut value: Value =
            serde_json::from_slice(&fs::read(home.join("auth.json")).unwrap()).unwrap();
        value["tokens"]["account_id"] = "another".into();
        fs::write(home.join("auth.json"), value.to_string()).unwrap();
        assert!(super::super::identity(&home).is_none());
        let view = request(json!({"operation":"status"})).unwrap();
        assert_eq!(view["accounts"][0]["loginSaved"], false);
    }
    #[test]
    fn rejected_ipc_cannot_supply_credentials_paths_or_executables() {
        let _t = Env::new();
        for v in [
            json!({"operation":"status","path":"/tmp/auth.json"}),
            json!({"operation":"begin","revision":0,"binary":"anything"}),
            json!({"operation":"configure","revision":0,"order":[1,2],"visible":[1],"token":"secret"}),
            json!({"operation":"cancel","loginId":"unknown"}),
            json!({"operation":"openLogin","loginId":"unknown","url":"https://example.com"}),
            json!({"operation":"openLogin","loginId":"unknown"}),
        ] {
            assert!(request(v).is_err());
        }
    }
    #[test]
    fn chrome_retry_is_bound_to_the_current_login_and_status_never_exposes_the_url() {
        let t = Env::new();
        let cancel = pending("owned");
        login().lock().unwrap().as_mut().unwrap().url =
            Some("https://auth.openai.com/oauth/authorize?state=private-login-state".into());
        let calls = std::cell::Cell::new(0);
        assert!(open_pending_login("other", &|_| {
            calls.set(calls.get() + 1);
            Ok(())
        })
        .is_err());
        assert!(open_pending_login("owned", &|_| {
            calls.set(calls.get() + 1);
            Err("Chrome 未响应".into())
        })
        .is_err());
        let failed = status(t.root()).unwrap();
        assert_eq!(failed["login"]["canOpen"], true);
        assert_eq!(failed["login"]["browserError"], "Chrome 未响应");
        assert!(!failed.to_string().contains("private-login-state"));
        assert!(failed["login"].get("authUrl").is_none());
        open_pending_login("owned", &|_| {
            calls.set(calls.get() + 1);
            Ok(())
        })
        .unwrap();
        let ready = status(t.root()).unwrap();
        assert_eq!(ready["login"]["browserOpened"], true);
        assert_eq!(ready["login"]["browserError"], Value::Null);
        status(t.root()).unwrap();
        status(t.root()).unwrap();
        assert_eq!(calls.get(), 2);
        cancel.store(true, Ordering::SeqCst);
        assert!(open_pending_login("owned", &|_| {
            calls.set(calls.get() + 1);
            Ok(())
        })
        .is_err());
        assert_eq!(calls.get(), 2);
    }
    #[cfg(unix)]
    #[test]
    fn symlink_account_and_root_are_never_read_as_saved_logins() {
        use std::os::unix::fs::symlink;
        let t = Env::new();
        let real = tempfile::tempdir().unwrap();
        auth(real.path(), "one", "a@example.com");
        symlink(real.path(), t.root().join("1")).unwrap();
        let v = request(json!({"operation":"status"})).unwrap();
        assert_eq!(v["accounts"][0]["loginSaved"], false);
        assert!(
            request(json!({"operation":"remove","revision":0,"slot":1,"confirm":true})).is_err()
        );
        let alias = t.root().join("alias");
        symlink(real.path(), &alias).unwrap();
        std::env::set_var("HACHIMO_CODEX_ACCOUNTS_HOME", alias);
        assert!(request(json!({"operation":"status"})).is_err());
    }
    #[cfg(unix)]
    #[test]
    fn isolated_login_rpc_accepts_only_the_expected_https_authorization_host() {
        use std::os::unix::fs::PermissionsExt;
        let t = Env::new();
        for (host, success) in [("auth.openai.com", true), ("example.com", false)] {
            let binary = t.root().join("fake-login");
            let script=format!("#!/usr/bin/env python3\nimport json,sys\njson.loads(sys.stdin.readline())\nprint(json.dumps({{'id':1,'result':{{}}}}),flush=True)\njson.loads(sys.stdin.readline());json.loads(sys.stdin.readline())\nprint(json.dumps({{'id':2,'result':{{'authUrl':'https://{host}/oauth/authorize?state=test'}}}}),flush=True)\nprint(json.dumps({{'method':'account/login/completed','params':{{'success':True}}}}),flush=True)\n");
            fs::write(&binary, script).unwrap();
            fs::set_permissions(&binary, fs::Permissions::from_mode(0o700)).unwrap();
            let cancel = pending("test");
            let opened = std::cell::RefCell::new(Vec::new());
            let result = login_worker_with_launcher(&binary, t.root(), "test", &cancel, &|url| {
                opened.borrow_mut().push(url.to_owned());
                Ok(())
            });
            assert_eq!(result.is_ok(), success);
            assert_eq!(opened.borrow().len(), if success { 1 } else { 0 });
            if success {
                let cancel = pending("retry");
                let result =
                    login_worker_with_launcher(&binary, t.root(), "retry", &cancel, &|_| {
                        Err("Chrome 未响应".into())
                    });
                assert!(result.is_ok());
                assert_eq!(
                    status(t.root()).unwrap()["login"]["browserError"],
                    "Chrome 未响应"
                );
            }
        }
    }
    fn quota(now: i64, used: i64) -> Value {
        json!({"plan_type":"plus","primary":{"used_percent":used,"window_minutes":300,"resets_at":now/1000+3600},
            "secondary":{"used_percent":25,"window_minutes":10080,"resets_at":now/1000+86400}})
    }
    fn seed(home: &Path, now: i64, used: i64) -> String {
        let who = identity(home).unwrap();
        let key = cache_key(home, &who);
        let mut entry = new_entry(&who.key);
        entry.value = Some((now, quota(now, used)));
        cache().lock().unwrap().insert(key.clone(), entry);
        key
    }
    fn finished(key: &str) {
        let end = Instant::now() + Duration::from_secs(3);
        loop {
            if cache().lock().unwrap().get(key).is_none_or(|e| !e.busy) {
                return;
            }
            assert!(Instant::now() < end, "background query did not finish");
            std::thread::sleep(Duration::from_millis(5));
        }
    }
    #[cfg(unix)]
    #[test]
    fn refreshing_keeps_last_quota_during_token_rotation_without_duplicate_requests() {
        let t = Env::new();
        let now = chrono::Utc::now().timestamp_millis();
        let home = t.root().join("1");
        auth(&home, "one", "a@example.com");
        let key = seed(&home, now, 25);
        let (ready_tx, ready_rx) = mpsc::channel();
        let (done_tx, done_rx) = mpsc::channel();
        let initial = snapshot_with(1, now, true, move |home, _| {
            let mut saved: Value =
                serde_json::from_slice(&fs::read(home.join("auth.json")).unwrap()).unwrap();
            saved["tokens"]["access_token"] = "fixture-rotated-access".into();
            fs::write(home.join("auth.json"), saved.to_string()).unwrap();
            ready_tx.send(()).unwrap();
            done_rx.recv_timeout(Duration::from_secs(3)).unwrap();
            Ok((quota(now, 50), identity(home).unwrap().key))
        });
        assert_eq!(initial["primary"]["remaining"], 75);
        assert_eq!(initial["status"], "查询中");
        ready_rx.recv_timeout(Duration::from_secs(3)).unwrap();
        let pending = snapshot_with(1, now + 1000, true, |_, _| {
            panic!("duplicate query after credential rotation")
        });
        assert_eq!(pending["primary"]["remaining"], 75);
        assert_eq!(pending["plan"], "Plus");
        assert_eq!(pending["observedAt"], now);
        assert_eq!(pending["status"], "查询中");
        let manager = request(json!({"operation":"status"})).unwrap();
        assert_eq!(manager["accounts"][0]["plan"], "Plus");
        assert_eq!(manager["accounts"][0]["observedAt"], now);
        done_tx.send(()).unwrap();
        finished(&key);
        let updated = cached(1, chrono::Utc::now().timestamp_millis());
        assert_eq!(updated["primary"]["remaining"], 50);
        retire_account(&home);
    }
    #[cfg(unix)]
    #[test]
    fn failed_refresh_preserves_zero_remaining_and_the_original_update_time() {
        let t = Env::new();
        let now = chrono::Utc::now().timestamp_millis();
        let home = t.root().join("1");
        auth(&home, "one", "a@example.com");
        let key = seed(&home, now, 100);
        let (reply_tx, reply_rx) = mpsc::channel();
        let pending = snapshot_with(1, now, true, move |_, _| {
            reply_rx.recv_timeout(Duration::from_secs(3)).unwrap();
            Err("查询失败，请重试".into())
        });
        assert_eq!(pending["primary"]["remaining"], 0);
        assert_eq!(pending["status"], "查询中");
        reply_tx.send(()).unwrap();
        finished(&key);
        let failed = cached(1, now + 1000);
        assert_eq!(failed["primary"]["remaining"], 0);
        assert_eq!(failed["observedAt"], now);
        assert_eq!(failed["plan"], "Plus");
        assert!(failed["status"].as_str().unwrap().contains("查询失败"));
        retire_account(&home);
    }
    #[cfg(unix)]
    #[test]
    fn retired_inflight_result_cannot_overwrite_a_recreated_cache_entry() {
        let t = Env::new();
        let now = chrono::Utc::now().timestamp_millis();
        let home = t.root().join("1");
        auth(&home, "one", "a@example.com");
        let key = seed(&home, now, 25);
        let (reply_tx, reply_rx) = mpsc::channel();
        let (finished_tx, finished_rx) = mpsc::channel();
        snapshot_with(1, now, true, move |home, _| {
            reply_rx.recv_timeout(Duration::from_secs(3)).unwrap();
            finished_tx.send(()).unwrap();
            Ok((quota(now, 99), identity(home).unwrap().key))
        });
        retire_account(&home);
        seed(&home, now, 10);
        reply_tx.send(()).unwrap();
        finished_rx.recv_timeout(Duration::from_secs(3)).unwrap();
        std::thread::sleep(Duration::from_millis(30));
        assert_eq!(cached(1, now)["primary"]["remaining"], 90);
        assert_eq!(cache().lock().unwrap()[&key].request_id, 0);
        retire_account(&home);
    }
    #[cfg(unix)]
    #[test]
    fn a_different_account_reusing_the_slot_never_inherits_old_quota() {
        let t = Env::new();
        let now = chrono::Utc::now().timestamp_millis();
        let home = t.root().join("1");
        auth(&home, "one", "a@example.com");
        seed(&home, now, 25);
        auth(&home, "two", "b@example.com");
        let other = cached(1, now);
        assert_eq!(other["email"], "b@example.com");
        assert_eq!(other["primary"]["remaining"], -1);
        assert_eq!(other["observedAt"], Value::Null);
        retire_account(&home);
    }
}
