//! PetUI Python reads only explicitly granted inputs and returns a bounded JSON object.
use crate::component_runtime_lifecycle::{capture_owner, Resource, ResourceOwner};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::{
    collections::BTreeMap,
    fs::{self, File},
    io::{Read, Write},
    path::{Path, PathBuf},
    process::{Command, Stdio},
    sync::{
        atomic::{AtomicBool, Ordering},
        mpsc, Arc, Mutex, OnceLock,
    },
    thread,
    time::{Duration, Instant},
};
use tauri::Manager;
use tauri_plugin_dialog::DialogExt;

pub(crate) const PATH: &str = "pc/plugin.py";
pub(crate) const MAX_SOURCE: usize = 16 * 1024 - 1;
const MAX_TEXT: usize = 12 * 1024;
const MAX_OUTPUT: usize = 16 * 1024;
const ERROR: &str = "Python 插件运行失败或超出限额";

#[derive(Clone, Deserialize, Serialize, PartialEq)]
#[serde(tag = "kind", rename_all = "kebab-case", deny_unknown_fields)]
pub(crate) enum Input {
    None,
    TextFile,
    HttpsJson {
        endpoint: String,
        hosts: Vec<String>,
        pointer: String,
    },
}

#[derive(Clone, Serialize, PartialEq)]
pub(crate) struct Spec {
    pub(crate) input: Input,
    pub(crate) refresh_sec: u32,
}

#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct SelectedFile {
    path: PathBuf,
    identity: [u64; 2],
}

#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Grant {
    fingerprint: String,
    file: Option<SelectedFile>,
}

struct Program {
    code: String,
    fingerprint: String,
    version: String,
}

struct Paths {
    resources: PathBuf,
    grants: PathBuf,
}
static PATHS: OnceLock<Paths> = OnceLock::new();
static AUTHORITY: Mutex<()> = Mutex::new(());

pub(crate) fn configure(app: &tauri::AppHandle, grants: PathBuf) {
    if let Ok(resources) = app.path().resource_dir() {
        let _ = PATHS.set(Paths { resources, grants });
    }
}

pub(crate) fn validate_source(data: &[u8]) -> Result<&str, String> {
    let code = std::str::from_utf8(data).map_err(|_| "Python 入口需为 UTF-8")?;
    if code.trim().is_empty() || data.len() > MAX_SOURCE || data.contains(&0) {
        return Err("Python 入口为空、包含空字节或超过 16 KiB".into());
    }
    Ok(code)
}

fn program(id: &str, spec: &Spec) -> Result<Program, String> {
    program_at(&crate::get_home_dir()?, id, spec)
}

fn program_at(home: &Path, id: &str, spec: &Spec) -> Result<Program, String> {
    let entry = crate::component_library::latest(home, id)?.ok_or("组件未安装")?;
    if !entry.valid {
        return Err("组件校验未通过".into());
    }
    let directory = Path::new(&entry.path);
    let path = directory.join(PATH);
    let bytes = crate::clawpkg::read_clawpkg_source_file(&path, MAX_SOURCE as u64)?;
    let actual = crate::component_library::package_content_hash(directory)?;
    if actual != entry.version_hash {
        return Err("组件内容已变化，请重新导入".into());
    }
    let mut digest = Sha256::new();
    digest.update(entry.version_hash.as_bytes());
    digest.update(serde_json::to_vec(spec).map_err(|_| ERROR)?);
    digest.update([0]);
    digest.update(&bytes);
    Ok(Program {
        code: validate_source(&bytes)?.to_owned(),
        fingerprint: format!("{:x}", digest.finalize()),
        version: actual,
    })
}

fn grant_path(paths: &Paths, id: &str) -> PathBuf {
    paths.grants.join(format!("{id}.json"))
}

fn load_grant(paths: &Paths, id: &str, program: &Program) -> Option<Grant> {
    grant_for_fingerprint(paths, id, &program.fingerprint)
}

fn grant_for_fingerprint(paths: &Paths, id: &str, fingerprint: &str) -> Option<Grant> {
    let path = grant_path(paths, id);
    let mut data = Vec::new();
    File::open(path)
        .ok()?
        .take(8193)
        .read_to_end(&mut data)
        .ok()?;
    if data.len() > 8192 {
        return None;
    }
    let grant: Grant = serde_json::from_slice(&data).ok()?;
    (grant.fingerprint == fingerprint).then_some(grant)
}

#[cfg(unix)]
fn file_identity(file: &File) -> Option<[u64; 2]> {
    use std::os::unix::fs::MetadataExt;
    let metadata = file.metadata().ok()?;
    metadata.is_file().then(|| [metadata.dev(), metadata.ino()])
}

#[cfg(windows)]
fn file_identity(file: &File) -> Option<[u64; 2]> {
    use std::os::windows::io::AsRawHandle;
    use windows_sys::Win32::Storage::FileSystem::{
        GetFileInformationByHandle, BY_HANDLE_FILE_INFORMATION,
    };
    if !file.metadata().ok()?.is_file() {
        return None;
    }
    let mut info = BY_HANDLE_FILE_INFORMATION::default();
    let success = unsafe { GetFileInformationByHandle(file.as_raw_handle(), &mut info) };
    (success != 0).then(|| {
        [
            u64::from(info.dwVolumeSerialNumber),
            u64::from(info.nFileIndexHigh) << 32 | u64::from(info.nFileIndexLow),
        ]
    })
}

#[cfg(not(any(unix, windows)))]
fn file_identity(_: &File) -> Option<[u64; 2]> {
    None
}

fn open_local(path: &Path) -> std::io::Result<File> {
    let mut options = fs::OpenOptions::new();
    options.read(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK);
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::OpenOptionsExt;
        use windows_sys::Win32::Storage::FileSystem::FILE_FLAG_OPEN_REPARSE_POINT;
        options.custom_flags(FILE_FLAG_OPEN_REPARSE_POINT);
    }
    options.open(path)
}

fn read_text(selected: &SelectedFile) -> Result<Value, String> {
    let file = open_local(&selected.path).map_err(|_| "无法读取已授权文件，请重新选择")?;
    if file_identity(&file) != Some(selected.identity) {
        return Err("已授权文件已更换，请重新选择".into());
    }
    let mut bytes = Vec::new();
    file.take((MAX_TEXT + 1) as u64)
        .read_to_end(&mut bytes)
        .map_err(|_| "无法读取已授权文件")?;
    if bytes.len() > MAX_TEXT {
        return Err("已授权文件超过 12 KiB".into());
    }
    String::from_utf8(bytes)
        .map(Value::String)
        .map_err(|_| "已授权文件需为 UTF-8 文本".into())
}

fn save_grant(
    paths: &Paths,
    id: &str,
    program: &Program,
    selected: Option<SelectedFile>,
) -> Result<(), String> {
    fs::create_dir_all(&paths.grants).map_err(|_| "无法保存 Python 授权")?;
    let grant = Grant {
        fingerprint: program.fingerprint.clone(),
        file: selected,
    };
    let mut pending =
        tempfile::NamedTempFile::new_in(&paths.grants).map_err(|_| "无法保存 Python 授权")?;
    serde_json::to_writer(&mut pending, &grant).map_err(|_| "无法保存 Python 授权")?;
    pending
        .as_file()
        .sync_all()
        .map_err(|_| "无法保存 Python 授权")?;
    pending
        .persist(grant_path(paths, id))
        .map_err(|_| "无法保存 Python 授权")?;
    Ok(())
}

// Only report status for the same installed content. Never expose the chosen path or value.
pub(crate) fn local_input_status(id: &str, version: &str, spec: &Spec) -> &'static str {
    let Ok(home) = crate::get_home_dir() else {
        return "unknown";
    };
    let Ok(Some(entry)) = crate::component_library::latest(&home, id) else {
        return "not-installed";
    };
    if entry.version_hash != version {
        return "different-installed-version";
    }
    let Ok(program) = program_at(&home, id, spec) else {
        return "unknown";
    };
    let data = if cfg!(target_os = "macos") {
        home.join("Library/Application Support")
    } else if cfg!(windows) {
        let Some(dir) = std::env::var_os("APPDATA") else {
            return "unknown";
        };
        PathBuf::from(dir)
    } else {
        return "unknown";
    };
    let config: Value = match serde_json::from_str(include_str!("../tauri.conf.json")) {
        Ok(v) => v,
        Err(_) => return "unknown",
    };
    let Some(identifier) = config["identifier"].as_str() else {
        return "unknown";
    };
    let paths = Paths {
        resources: PathBuf::new(),
        grants: data.join(identifier).join("python-component-grants"),
    };
    match load_grant(&paths, id, &program) {
        None => "grant-required",
        Some(grant) if matches!(spec.input, Input::TextFile) => {
            if grant.file.as_ref().is_some_and(|f| read_text(f).is_ok()) {
                "file-ready"
            } else {
                "reselect-file"
            }
        }
        Some(_) => "granted",
    }
}

// The caller owns a fresh temporary library. All writes stay in that library.
pub(crate) fn rehearsal(
    home: &Path,
    resources: &Path,
    id: &str,
    spec: &Spec,
    fixture: Option<Value>,
) -> Result<(Value, Option<Value>), String> {
    let paths = Paths {
        resources: resources.into(),
        grants: home.join("preview-grants"),
    };
    let program = program_at(home, id, spec)?;
    let before = load_grant(&paths, id, &program).is_some();
    let mut result =
        json!({"grantBefore":before,"grantAfter":"unobserved","input":"fixture-needed"});
    let (selected, input) = match (&spec.input, fixture) {
        (Input::None, None | Some(Value::Null)) => (None, Value::Null),
        (_, None) => return Ok((result, None)),
        (Input::TextFile, Some(Value::String(text))) => {
            let path = home.join("fixture.txt");
            fs::write(&path, text).map_err(|_| "无法写入预演输入")?;
            let file = open_local(&path).map_err(|_| "无法打开预演输入")?;
            let selection = SelectedFile {
                path,
                identity: file_identity(&file).ok_or("预演输入无效")?,
            };
            let value = read_text(&selection)?;
            (Some(selection), value)
        }
        (Input::HttpsJson { pointer, .. }, Some(response)) => (
            None,
            response
                .pointer(pointer)
                .cloned()
                .ok_or("HTTPS 样例缺少输入投影字段")?,
        ),
        _ => return Err("text-file 样例须为 JSON 字符串，none 须为空".into()),
    };
    save_grant(&paths, id, &program, selected)?;
    result["grantAfter"] = load_grant(&paths, id, &program).is_some().into();
    result["input"] = "fixture-ready".into();
    let value = run(resources, &program.code, input, || false)?;
    Ok((result, Some(value)))
}

fn paths() -> Result<&'static Paths, String> {
    PATHS
        .get()
        .ok_or_else(|| "Python 插件服务尚未初始化".into())
}

fn request(
    app: &tauri::AppHandle,
    id: &str,
    version: &str,
    operation: &str,
) -> Result<Value, String> {
    if !crate::component_extension::valid_id(id) {
        return Err("组件 ID 无效".into());
    }
    let paths = paths()?;
    let (decl, spec) = crate::component_app::python_spec(id)?;
    if decl != version {
        return Err("组件已经更新，请重新打开详情".into());
    }
    let program = program(id, &spec)?;
    if program.version != version {
        return Err("组件已经更新，请重新打开详情".into());
    }

    let chosen = if operation == "enable" && matches!(spec.input, Input::TextFile) {
        let Some(picked) = app
            .dialog()
            .file()
            .set_title("为此组件选择一个只读 UTF-8 文件")
            .blocking_pick_file()
        else {
            return Ok(json!({"cancelled":true}));
        };
        let path = picked
            .into_path()
            .map_err(|_| "请选择本机文本文件")?
            .canonicalize()
            .map_err(|_| "无法打开所选文件")?;
        let file = open_local(&path).map_err(|_| "无法打开所选文件")?;
        let identity = file_identity(&file).ok_or("请选择本机普通文件")?;
        let selection = SelectedFile { path, identity };
        read_text(&selection)?;
        Some(selection)
    } else {
        None
    };

    if operation == "enable" {
        let (current, _) = crate::component_app::python_spec(id)?;
        if current != program.version {
            return Err("组件已经更新，请重新授权".into());
        }
    }
    let _guard = AUTHORITY.lock().map_err(|_| "Python 授权服务暂不可用")?;
    match operation {
        "status" => (),
        "enable" => {
            save_grant(paths, id, &program, chosen)?;
            disable(id);
        }
        "disable" => {
            match fs::remove_file(grant_path(paths, id)) {
                Ok(()) => (),
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => (),
                Err(_) => return Err("无法撤销 Python 授权".into()),
            }
            disable(id);
        }
        _ => return Err("未知 Python 授权操作".into()),
    }
    let grant = load_grant(paths, id, &program);
    Ok(json!({"enabled":grant.is_some(), "input":spec.input,
        "file":grant.as_ref().and_then(|g|g.file.as_ref()?.path.file_name()).map(|v|v.to_string_lossy())}))
}

#[tauri::command]
pub(crate) async fn component_python_request(
    app: tauri::AppHandle,
    component_id: String,
    version_hash: String,
    operation: String,
) -> Result<Value, String> {
    tauri::async_runtime::spawn_blocking(move || {
        request(&app, &component_id, &version_hash, &operation)
    })
    .await
    .map_err(|_| "Python 授权服务暂不可用")?
}

pub(crate) fn run(
    resources: &Path,
    code: &str,
    input: Value,
    cancelled: impl Fn() -> bool,
) -> Result<Value, String> {
    let node =
        resources
            .join("bridge/runtime")
            .join(if cfg!(windows) { "node.exe" } else { "node" });
    let runner = resources.join("skills/petui/runtime/python/runner.mjs");
    let request = serde_json::to_vec(&json!({"source":code,"input":input})).map_err(|_| ERROR)?;
    if request.len() > 112 * 1024 || cancelled() {
        return Err(ERROR.into());
    }

    let mut command = Command::new(node);
    command
        .arg("--max-old-space-size=48")
        .arg(runner)
        .env_clear()
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(0x08000000);
    }
    let mut child = command.spawn().map_err(|_| "未找到随包 Python 运行时")?;
    let mut stdin = child.stdin.take().expect("piped stdin");
    let stdout = child.stdout.take().expect("piped stdout");
    let deadline = Instant::now() + Duration::from_secs(3);
    let writer = thread::spawn(move || stdin.write_all(&request));
    let reader = thread::spawn(move || {
        let mut output = Vec::new();
        stdout
            .take((MAX_OUTPUT + 1) as u64)
            .read_to_end(&mut output)
            .map(|_| output)
    });
    let completed = loop {
        if cancelled() || Instant::now() >= deadline {
            let _ = child.kill();
            let _ = child.wait();
            break false;
        }
        match child.try_wait() {
            Ok(Some(status)) => break status.success(),
            Ok(None) => thread::sleep(Duration::from_millis(20)),
            Err(_) => {
                let _ = child.kill();
                let _ = child.wait();
                break false;
            }
        }
    };
    let written = matches!(writer.join(), Ok(Ok(())));
    let output = reader.join().map_err(|_| ERROR)?.map_err(|_| ERROR)?;
    if !completed || !written || cancelled() || output.len() > MAX_OUTPUT {
        return Err(ERROR.into());
    }
    let value: Value = serde_json::from_slice(&output).map_err(|_| ERROR)?;
    if !value.is_object() || !crate::component_expr::bounded(&value) {
        return Err(ERROR.into());
    }
    Ok(value)
}

struct Job {
    deadline: Instant,
    id: String,
    owner: ResourceOwner,
    cancelled: Arc<AtomicBool>,
    code: String,
    input: Input,
    file: Option<SelectedFile>,
}
struct Cache {
    owner: ResourceOwner,
    spec: Spec,
    fingerprint: String,
    cancelled: Arc<AtomicBool>,
    value: Value,
    status: &'static str,
    last_run: Option<Instant>,
}
struct Pool {
    cache: Arc<Mutex<BTreeMap<String, Cache>>>,
    jobs: mpsc::SyncSender<Job>,
}
static POOL: OnceLock<Pool> = OnceLock::new();

async fn wait_input<T>(input: impl std::future::Future<Output = Result<T, String>>,
                       cancelled: impl Fn() -> bool) -> Result<T, String> {
    tokio::pin!(input);
    while !cancelled() {
        if let Ok(result) = tokio::time::timeout(Duration::from_millis(25), &mut input).await {
            return if cancelled() { Err(ERROR.into()) } else { result };
        }
    }
    Err(ERROR.into())
}

fn execute(job: &Job) -> Result<Value, String> {
    let cancelled = || Instant::now() >= job.deadline || job.cancelled.load(Ordering::Acquire) || !job.owner.is_current();
    if cancelled() {
        return Err(ERROR.into());
    }
    let data = match &job.input {
        Input::None => Value::Null,
        Input::TextFile => {
            static READ: tokio::sync::Semaphore = tokio::sync::Semaphore::const_new(1);
            let file = job.file.clone().ok_or("请先选择只读文件")?;
            let owner = job.owner.clone();
            let stop = job.cancelled.clone();
            let deadline = job.deadline;
            tauri::async_runtime::block_on(wait_input(async move {
                let slot = READ.try_acquire().map_err(|_| "文件读取繁忙")?;
                tokio::task::spawn_blocking(move || {
                    let _slot = slot;
                    if !owner.is_current() || stop.load(Ordering::Acquire) || Instant::now() >= deadline {
                        return Err(ERROR.into());
                    }
                    read_text(&file)
                }).await.map_err(|_| "文件读取失败")?
            }, cancelled))?
        }
        Input::HttpsJson {
            endpoint,
            hosts,
            pointer,
        } => {
            let permit = Arc::new(job.owner.acquire(Resource::PublicNetwork)?);
            let stop = job.cancelled.clone();
            let deadline = job.deadline;
            let check = move || {
                if stop.load(Ordering::Acquire) || Instant::now() >= deadline { return Err(ERROR.into()); }
                permit.ensure_current()
            };
            let response = tauri::async_runtime::block_on(wait_input(
                crate::media_network::python_json(endpoint, hosts, 32 * 1024, check), cancelled,
            ))?;
            response
                .pointer(pointer)
                .cloned()
                .ok_or("响应中缺少所声明的 JSON 字段")?
        }
    };
    run(&paths()?.resources, &job.code, data, cancelled)
}

fn pool() -> &'static Pool {
    POOL.get_or_init(|| {
        let cache: Arc<Mutex<BTreeMap<String, Cache>>> = Default::default();
        let (jobs, pending) = mpsc::sync_channel::<Job>(4);
        let update = cache.clone();
        thread::Builder::new()
            .name("petui-python".into())
            .spawn(move || {
                while let Ok(job) = pending.recv() {
                    let result = execute(&job);
                    let mut cache = update.lock().unwrap();
                    if let Some(entry) = cache
                        .get_mut(&job.id)
                        .filter(|e| Arc::ptr_eq(&e.cancelled, &job.cancelled))
                    {
                        if !job.owner.is_current() {
                            entry.value = Value::Null;
                            entry.status = "idle";
                            entry.last_run = None;
                        } else {
                            match result {
                                Ok(value) if Instant::now() < job.deadline => {
                                    entry.value = value;
                                    entry.status = "ready";
                                }
                                _ => {
                                    entry.value = Value::Null;
                                    entry.status = "error";
                                }
                            }
                        }
                    }
                }
            })
            .expect("bounded Python worker");
        Pool { cache, jobs }
    })
}

pub(crate) fn disable(id: &str) {
    if let Some(pool) = POOL.get() {
        if let Some(entry) = pool.cache.lock().unwrap().remove(id) {
            entry.cancelled.store(true, Ordering::Release);
        }
    }
}

pub(crate) fn snapshot(id: &str, spec: Spec, refresh: bool) -> (Value, &'static str) {
    let Ok(paths) = paths() else {
        return (Value::Null, "error");
    };
    let Ok(owner) = capture_owner() else {
        return (Value::Null, "error");
    };
    let delay = if refresh { 15 } else { u64::from(spec.refresh_sec) };
    {
        // Library mutations and formal grant changes evict this entry. Reading
        // a cached result never executes code; recheck the on-disk grant too.
        let Ok(_authority) = AUTHORITY.lock() else { return (Value::Null, "error"); };
        let cache = pool().cache.lock().unwrap();
        if let Some(e) = cache.get(id).filter(|e| e.owner.is_current() && e.spec == spec && (e.status == "preparing"
            || e.last_run.is_some_and(|last| last.elapsed().as_secs() < delay))) {
            return if grant_for_fingerprint(paths, id, &e.fingerprint).is_some() {
                (e.value.clone(), e.status)
            } else { (Value::Null, "authorization") };
        }
    }
    let Ok(program) = program(id, &spec) else {
        return (Value::Null, "error");
    };
    let Ok(_guard) = AUTHORITY.lock() else {
        return (Value::Null, "error");
    };
    let Some(grant) = load_grant(paths, id, &program) else {
        return (Value::Null, "authorization");
    };
    let pool = pool();
    let mut cache = pool.cache.lock().unwrap();
    if cache
        .get(id)
        .is_some_and(|e| !e.owner.is_current() || e.fingerprint != program.fingerprint)
    {
        cache
            .remove(id)
            .unwrap()
            .cancelled
            .store(true, Ordering::Release);
    }
    if !cache.contains_key(id) && cache.len() >= 16 {
        let evict = cache
            .iter()
            .filter(|(_, v)| v.status != "preparing")
            .min_by_key(|(_, v)| v.last_run)
            .map(|(id, _)| id.clone());
        let Some(evict) = evict else {
            return (Value::Null, "busy");
        };
        cache.remove(&evict);
    }
    let entry = cache.entry(id.to_owned()).or_insert_with(|| Cache {
        owner: owner.clone(),
        spec: spec.clone(),
        fingerprint: program.fingerprint,
        cancelled: Arc::new(AtomicBool::new(false)),
        value: Value::Null,
        status: "idle",
        last_run: None,
    });
    if entry.status != "preparing"
        && entry
            .last_run
            .is_none_or(|last| last.elapsed().as_secs() >= delay)
    {
        let job = Job {
            deadline: Instant::now() + Duration::from_secs(18),
            id: id.into(),
            owner: owner.clone(),
            cancelled: entry.cancelled.clone(),
            code: program.code,
            input: spec.input,
            file: grant.file,
        };
        entry.status = if pool.jobs.try_send(job).is_ok() {
            entry.owner = owner;
            entry.last_run = Some(Instant::now());
            "preparing"
        } else {
            "busy"
        };
    }
    (entry.value.clone(), entry.status)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn expired_queued_job_fails_before_accessing_inputs() {
        let job = Job {
            deadline: Instant::now(), id: "expired".into(), owner: ResourceOwner::fresh(),
            cancelled: Arc::new(AtomicBool::new(false)), code: String::new(),
            input: Input::TextFile, file: None,
        };
        assert_eq!(execute(&job), Err(ERROR.into()));
    }

    #[test]
    fn stalled_input_cancel_drops_socket_before_next_job() {
        use std::net::TcpListener;
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let address = listener.local_addr().unwrap();
        let (started, stalled) = mpsc::channel();
        let server = thread::spawn(move || {
            let (mut peer, _) = listener.accept().unwrap();
            peer.set_read_timeout(Some(Duration::from_secs(3))).unwrap();
            let mut request = Vec::new();
            while !request.ends_with(b"\r\n\r\n") {
                let mut byte = [0];
                peer.read_exact(&mut byte).unwrap();
                request.push(byte[0]);
                assert!(request.len() < 4096);
            }
            peer.write_all(b"HTTP/1.1 200 OK\r\nContent-Length: 500\r\n\r\n{").unwrap();
            started.send(Instant::now()).unwrap();
            assert_eq!(peer.read(&mut [0; 1]).unwrap(), 0);
        });
        let owner = ResourceOwner::fresh();
        let request_owner = owner.clone();
        let worker = thread::spawn(move || {
            tauri::async_runtime::block_on(wait_input(async {
                let client = reqwest::Client::builder().no_proxy().build().unwrap();
                let response = client.get(format!("http://{address}")).send().await.unwrap();
                response.bytes().await.map_err(|e| e.to_string())
            }, || !request_owner.is_current()))
        });
        stalled.recv_timeout(Duration::from_secs(3)).unwrap();
        let start = Instant::now();
        owner.cancel();
        assert!(worker.join().unwrap().is_err());
        assert_eq!(tauri::async_runtime::block_on(wait_input(async { Ok(42) }, || false)).unwrap(), 42);
        assert!(start.elapsed() < Duration::from_millis(500));
        server.join().unwrap();
    }

    #[cfg(any(unix, windows))]
    #[test]
    fn file_grant_reads_the_selected_file_and_rejects_a_replacement() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("sample.txt");
        fs::write(&path, "first").unwrap();
        let identity = file_identity(&open_local(&path).unwrap()).unwrap();
        let selected = SelectedFile {
            path: path.clone(),
            identity,
        };
        assert_eq!(read_text(&selected).unwrap(), json!("first"));
        fs::rename(&path, dir.path().join("original.txt")).unwrap();
        fs::write(&path, "replacement").unwrap();
        assert!(read_text(&selected).is_err());
    }

    #[test]
    fn changing_a_program_invalidates_its_previous_grant() {
        let dir = tempfile::tempdir().unwrap();
        let paths = Paths {
            resources: dir.path().into(),
            grants: dir.path().into(),
        };
        let mut program = Program {
            code: String::new(),
            version: "version".into(),
            fingerprint: "first".into(),
        };
        let grant = Grant {
            fingerprint: program.fingerprint.clone(),
            file: None,
        };
        fs::write(
            grant_path(&paths, "sample"),
            serde_json::to_vec(&grant).unwrap(),
        )
        .unwrap();
        assert!(load_grant(&paths, "sample", &program).is_some());
        program.fingerprint = "updated".into();
        assert!(load_grant(&paths, "sample", &program).is_none());
    }
}
