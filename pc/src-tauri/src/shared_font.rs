//! Install the firmware-pinned bubble font in component storage. Both links
//! use the same transaction and require a positive device-side activation ACK.
use base64::Engine;
use serde_json::{json, Value};
use std::collections::HashMap;
use std::sync::{Arc, LazyLock, Mutex, MutexGuard, TryLockError, Weak};
use std::time::{Duration, Instant};

const FONT: &[u8] = include_bytes!("../../../firmware/assets/fonts/bubble32-v1.fpk");
const CHUNK: usize = 3072;
static INSTALLS: LazyLock<Mutex<HashMap<String, Weak<Mutex<()>>>>> =
    LazyLock::new(|| Mutex::new(HashMap::new()));

fn board_install(board: &str) -> Result<Arc<Mutex<()>>, String> {
    let mut installs = INSTALLS.lock().map_err(|_| "字库同步任务不可用")?;
    installs.retain(|_, gate| gate.strong_count() != 0);
    if let Some(gate) = installs.get(board).and_then(Weak::upgrade) {
        return Ok(gate);
    }
    let gate = Arc::new(Mutex::new(()));
    installs.insert(board.to_owned(), Arc::downgrade(&gate));
    Ok(gate)
}

fn lock_while_current<'a>(
    lock: &'a Mutex<()>,
    current: &impl Fn() -> Result<(), String>,
) -> Result<MutexGuard<'a, ()>, String> {
    loop {
        match lock.try_lock() {
            Ok(guard) => return Ok(guard),
            Err(TryLockError::WouldBlock) => {
                current()?;
                std::thread::sleep(Duration::from_millis(250));
            }
            Err(TryLockError::Poisoned(_)) => return Err("设备资源传输不可用".into()),
        }
    }
}

pub(crate) fn supports(firmware: &str) -> bool {
    crate::parse_firmware_revision(firmware) >= 302
}

#[cfg(test)]
fn ensure(
    board: &str,
    send: impl FnMut(Value) -> Result<Value, String>,
) -> Result<(), String> {
    ensure_with_stream(board, send, None, None, || Ok(()))
}

pub(super) fn ensure_with_stream(
    board: &str,
    mut send: impl FnMut(Value) -> Result<Value, String>,
    stream: Option<&mut dyn FnMut(&str, &[u8]) -> Result<(), String>>,
    asset_transfer: Option<&Mutex<()>>,
    current: impl Fn() -> Result<(), String>,
) -> Result<(), String> {
    let gate = board_install(board)?;
    let transfer = uuid::Uuid::new_v4().simple().to_string();
    let mut call = |phase: &str, mut payload: Value| -> Result<Value, String> {
        payload["phase"] = json!(phase);
        payload["resource"] = json!("bubble32-v1");
        payload["transferId"] = json!(transfer);
        let reply = send(payload)?;
        if reply["ok"] != true || reply["boardDeviceId"] != board {
            return Err("设备未确认字库操作，请检查连接、设备空闲状态和组件空间。".into());
        }
        Ok(reply)
    };
    // Serialize the decision to begin with other installs on this board. Each
    // poll travels over the original scoped connection and may cancel the wait.
    let deadline = Instant::now() + Duration::from_secs(45);
    let (transaction, status) = loop {
        let transaction = lock_while_current(&gate, &current)?;
        let status = call("status", json!({}))?;
        if status["loaded"] == true { return current(); }
        if status["loading"] != true { break (transaction, status); }
        drop(transaction);
        if Instant::now() >= deadline { return Err("等待设备加载字库超时。".into()); }
        std::thread::sleep(Duration::from_millis(250));
    };
    let assets = asset_transfer.map(|lock| lock_while_current(lock, &current)).transpose()?;
    let asynchronous = status["asyncCommit"] == true;
    call("begin", json!({}))?;
    let result: Result<Value, String> = (|| {
        if let Some(send_bytes) = stream.filter(|_| status["fontStream"] == true) {
            send_bytes(&transfer, FONT)?;
        } else {
            for (i, chunk) in FONT.chunks(CHUNK).enumerate() {
                let offset = i * CHUNK;
                let reply = call(
                    "chunk",
                    json!({"offset":offset,"data":base64::engine::general_purpose::STANDARD.encode(chunk)}),
                )?;
                if reply["received"].as_u64() != Some((offset + chunk.len()) as u64) {
                    return Err("设备确认的字库写入位置不一致。".into());
                }
            }
        }
        let committed = call("commit", json!({"asyncCommit":asynchronous}))?;
        if committed["loaded"] != true && committed["loading"] != true {
            return Err("设备尚未加载已传输的字库。".into());
        }
        Ok(committed)
    })();
    if result.is_err() {
        let _ = call("abort", json!({}));
    }
    drop(assets);
    drop(transaction);
    let mut status = result?;
    let deadline = Instant::now() + Duration::from_secs(45);
    while status["loaded"] != true {
        if status["loading"] != true || Instant::now() >= deadline {
            return Err("设备尚未加载已传输的字库。".into());
        }
        std::thread::sleep(Duration::from_millis(250));
        status = call("status", json!({}))?;
    }
    current()
}

#[tauri::command]
pub(crate) async fn ensure_device_bubble_font(
    usb_manager: tauri::State<'_, crate::usb_serial::UsbSerialManager>,
    board: String,
    connection: Option<String>,
    boot: Option<String>,
) -> Result<(), String> {
    let usb = usb_manager.inner().clone();
    tauri::async_runtime::spawn_blocking(move || match (connection, boot) {
        (Some(id), None) => usb.ensure_bubble_font(&board, &id),
        (None, Some(id)) => crate::lan_agent::ensure_bubble_font(&usb, &board, &id),
        _ => Err("无法确认字库同步目标。".into()),
    })
    .await
    .map_err(|e| e.to_string())?
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn confirms_device_activation_and_skips_installed_font() {
        let mut installed = Vec::new();
        let mut phases = Vec::new();
        for _ in 0..2 {
            ensure("board", |p| {
                let phase = p["phase"].as_str().unwrap();
                phases.push(phase.to_owned());
                if phase == "chunk" {
                    assert_eq!(p["offset"], installed.len());
                    installed.extend(base64::engine::general_purpose::STANDARD.decode(p["data"].as_str().unwrap()).unwrap());
                }
                Ok(json!({"ok":true,"boardDeviceId":"board","received":installed.len(),"loaded":installed == FONT}))
            }).unwrap();
        }
        assert_eq!(installed, FONT);
        assert_eq!(phases.iter().filter(|p| *p == "begin").count(), 1);
        assert!(phases.ends_with(&["commit".into(), "status".into()]));
    }

    #[test]
    fn negotiated_stream_waits_for_activation_and_preserves_legacy_fallback() {
        let mut phases = Vec::new();
        let mut streamed = 0;
        ensure_with_stream("board", |p| {
            let phase = p["phase"].as_str().unwrap();
            assert_ne!(phase, "chunk");
            let finished = phase == "status" && phases.len() == 3;
            phases.push(phase.to_string());
            Ok(json!({"ok":true, "boardDeviceId":"board", "asyncCommit":true, "fontStream":true,
                "loading":phase=="commit", "loaded":finished}))
        }, Some(&mut |id, bytes| {
            assert_eq!(id.len(), 32);
            assert_eq!(bytes, FONT);
            streamed += 1;
            Ok(())
        }), None, || Ok(())).unwrap();
        assert_eq!(streamed, 1);
        assert_eq!(phases, ["status", "begin", "commit", "status"]);
    }

    #[test]
    fn board_transactions_share_only_their_own_gate() {
        let first = board_install("first").unwrap();
        let transaction = first.lock().unwrap();
        assert!(board_install("first").unwrap().try_lock().is_err());
        ensure("second", |_| Ok(json!({"ok":true,"boardDeviceId":"second","loaded":true}))).unwrap();
        drop(transaction);
        assert!(board_install("first").unwrap().try_lock().is_ok());
    }

    #[test]
    fn simultaneous_requests_install_once_on_the_same_board() {
        use std::sync::{Barrier, atomic::{AtomicBool, AtomicUsize, Ordering}};

        let start = Barrier::new(2);
        let loaded = AtomicBool::new(false);
        let begins = AtomicUsize::new(0);
        let board = "shared-device";
        std::thread::scope(|scope| {
            for _ in 0..2 {
                scope.spawn(|| {
                    start.wait();
                    ensure_with_stream(board, |p| {
                        match p["phase"].as_str().unwrap() {
                            "begin" => { begins.fetch_add(1, Ordering::SeqCst); }
                            "commit" => loaded.store(true, Ordering::SeqCst),
                            "status" => (),
                            phase => panic!("unexpected {phase}"),
                        }
                        Ok(json!({"ok":true, "boardDeviceId":board, "fontStream":true,
                            "loaded":loaded.load(Ordering::SeqCst)}))
                    }, Some(&mut |_, bytes| { assert_eq!(bytes, FONT); Ok(()) }), None, || Ok(())).unwrap();
                });
            }
        });
        assert_eq!(begins.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn stale_waiter_leaves_an_occupied_transaction() {
        let board = "old-connection";
        let gate = board_install(board).unwrap();
        let _transaction = gate.lock().unwrap();
        let result = ensure_with_stream(board, |_| panic!("retired route sent a request"),
            None, None, || Err("retired".into()));
        assert_eq!(result.unwrap_err(), "retired");
    }

    #[test]
    fn activation_polls_release_both_locks_and_cancel_on_disconnect() {
        let board = "reconnecting-board";
        let gate = board_install(board).unwrap();
        let assets = Mutex::new(());
        let mut committed = false;
        let result = ensure_with_stream(board, |p| {
            let phase = p["phase"].as_str().unwrap();
            if committed {
                assert_eq!(phase, "status");
                assert!(assets.try_lock().is_ok() && gate.try_lock().is_ok());
                return Err("connection retired".into());
            }
            if phase != "status" {
                assert!(assets.try_lock().is_err() && gate.try_lock().is_err());
            }
            committed = phase == "commit";
            Ok(json!({"ok":true,"boardDeviceId":board,"fontStream":true,
                "loading":committed,"asyncCommit":true}))
        }, Some(&mut |_, _| Ok(())), Some(&assets), || Ok(()));
        assert_eq!(result.unwrap_err(), "connection retired");
    }

    #[test]
    fn interrupted_or_wrong_peer_never_completes_installation() {
        for failure in ["peer", "offset", "commit"] {
            let mut phases = Vec::new();
            let result = ensure("board", |p| {
                let phase = p["phase"].as_str().unwrap();
                phases.push(phase.to_owned());
                let offset = p["offset"].as_u64().unwrap_or_default() as usize;
                Ok(
                    json!({"ok":true, "boardDeviceId":if failure=="peer" {"other"} else {"board"},
                    "received":if failure=="offset" {0} else {(offset + CHUNK).min(FONT.len())}, "loaded":false}),
                )
            });
            assert!(result.is_err());
            assert_eq!(
                phases.last().unwrap(),
                if failure == "peer" { "status" } else { "abort" }
            );
            assert_eq!(phases.iter().any(|p| p == "commit"), failure == "commit");
        }
    }
}
