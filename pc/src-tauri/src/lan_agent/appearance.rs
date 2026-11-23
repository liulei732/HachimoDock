//! Trusted LAN delivery of the same prepared appearance packs used by USB.
use super::{
    exchange_link_observed, query_status, read_with_recovery, require_no_usb, Peer,
};
use crate::usb_serial::{
    appearance_transaction::{
        asset_checksum_hex, build_asset_file_commit_payload, p4_builtin_slot,
        parse_p4_appearance_slot_state, verified_p4_payload_snapshot, P4AppearanceSlot,
        P4AppearanceSlotState, P4_BUILTIN_APPEARANCE_SLOT, P4_RAW_APPEARANCE_SLOT,
    },
    prepared_p4_lan_files, sync_global_cues, transfer_log, GlobalCuePhase, UsbSerialManager,
};
use base64::{engine::general_purpose::STANDARD, Engine as _};
use serde_json::{json, Value};
use std::io::Read;
use std::path::Path;
use std::time::{Duration, Instant};
use tauri::{Emitter, Manager};

const MODE: u8 = 18;
const CHUNK_BYTES: usize = 8192;
const REPLY_MAX: usize = 3072;
const RESULT_UNKNOWN: &str = "Wi-Fi 未能确认形象切换结果，请先查看设备当前形象。";
const NOT_READY: &str = "设备暂未接受无线形象操作，请确认设备空闲后再试。";
const PREPARED_ASSET_ERROR: &str = "无法完整读取已准备的形象素材，设备形象未改动。";

#[derive(Debug, PartialEq, Eq, serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub(crate) enum AppearanceSyncOutcome {
    AlreadyActive,
    Activated,
    Uploaded,
}

#[derive(Debug, PartialEq, Eq)]
enum AppearancePlan<'a> {
    Keep,
    Activate(&'a P4AppearanceSlot),
    Upload,
}

fn appearance_plan<'a>(
    slots: &'a P4AppearanceSlotState,
    custom_pack: Option<&str>,
) -> Result<AppearancePlan<'a>, String> {
    let target = match custom_pack {
        None => Some(p4_builtin_slot(slots)?),
        Some(pack) => slots
            .slots
            .iter()
            .find(|slot| slot.slot == P4_RAW_APPEARANCE_SLOT && slot.pack_id == pack),
    };
    Ok(match target {
        Some(slot) if slots.active_slot == Some(slot.slot) => AppearancePlan::Keep,
        Some(slot) => AppearancePlan::Activate(slot),
        None => AppearancePlan::Upload,
    })
}

fn confirmed_active_pack(slots: &P4AppearanceSlotState, slot: u32, pack_id: &str) -> bool {
    slots.active_slot == Some(slot)
        && slots
            .slots
            .iter()
            .any(|candidate| candidate.slot == slot && candidate.pack_id == pack_id)
}

#[derive(Clone, Copy)]
#[repr(u8)]
enum Phase {
    Slots,
    Activate,
    Begin,
    Chunk,
    File,
    Commit,
    AudioBegin,
    AudioChunk,
    AudioCommit,
    AudioAbort,
}

impl Phase {
    fn label(self) -> &'static str {
        match self {
            Self::Slots => "slot-query",
            Self::Activate => "activate",
            Self::Begin => "begin",
            Self::Chunk => "chunk",
            Self::File => "file",
            Self::Commit => "commit",
            Self::AudioBegin => GlobalCuePhase::Begin.label(),
            Self::AudioChunk => GlobalCuePhase::Chunk.label(),
            Self::AudioCommit => GlobalCuePhase::Commit.label(),
            Self::AudioAbort => GlobalCuePhase::Abort.label(),
        }
    }
}

fn appearance_pairing(peer: &Peer) -> Result<u64, String> {
    let _lock = super::PAIRING_MUTATION.lock().map_err(|_| "配对记录繁忙")?;
    if !super::read_peer()?.is_some_and(|current| super::widget_data::same_peer(&current, peer)) {
        return Err("Wi-Fi 配对已变化。".into());
    }
    Ok(super::component_events::pairing_revision())
}

fn retryable_exchange(phase: Phase, path: Option<&str>, submitted: bool, kind: std::io::ErrorKind) -> bool {
    use std::io::ErrorKind::*;
    let transient = matches!(kind, TimedOut | WouldBlock | Interrupted | UnexpectedEof
        | ConnectionReset | ConnectionAborted | BrokenPipe | ConnectionRefused);
    // On a board advertising raw appearance capacity the last video chunk is
    // deduplicated by transaction, path, index, size and checksum. SPIFFS and
    // audio append operations do not provide that guarantee.
    let duplicate_safe = matches!(phase, Phase::Slots) || (matches!(phase, Phase::Chunk)
        && path.is_some_and(|p| p.ends_with(".h264") || p.ends_with(".mjpg")));
    transient && (!submitted || duplicate_safe)
}

struct Transfer<'a> {
    peer: &'a Peer,
    pairing: u64,
    board: &'a str,
    boot: &'a str,
    capacity: u32,
    id: String,
    usb: &'a UsbSerialManager,
}

impl Transfer<'_> {
    fn request(&self, phase: Phase, payload: Value) -> Result<Value, String> {
        let reply = self.exchange_phase(phase, payload);
        if reply.is_err() {
            transfer_log::record(
                "appearance",
                "phase_send_failed",
                json!({
                    "transport": "wifi", "phase": phase.label(),
                }),
            );
        } else if matches!(phase, Phase::Activate | Phase::Begin | Phase::Commit) {
            transfer_log::record(
                "appearance",
                "phase_ack",
                json!({
                    "transport": "wifi", "phase": phase.label(),
                }),
            );
        }
        reply
    }

    fn check_route(&self) -> Result<(), String> {
        if super::component_events::pairing_revision() != self.pairing {
            return Err("Wi-Fi 配对已变化，形象传输已停止。".into());
        }
        require_no_usb(self.usb)
    }

    fn exchange_phase(&self, phase: Phase, payload: Value) -> Result<Value, String> {
        self.check_route()?;
        if !matches!(phase, Phase::AudioAbort) {
            self.usb.ensure_appearance_sync_not_cancelled(None)?;
        }
        let mut body = self.boot.as_bytes().to_vec();
        body.push(phase as u8);
        serde_json::to_writer(&mut body, &payload).map_err(|_| NOT_READY)?;
        if body.len() >= 16384 {
            return Err("无线素材请求超过固件上限。".into());
        }
        let timeout = match phase {
            Phase::Begin
            | Phase::File
            | Phase::Activate
            | Phase::Commit
            | Phase::AudioBegin
            | Phase::AudioCommit
            | Phase::AudioAbort => Duration::from_secs(58),
            _ => Duration::from_secs(6),
        };
        let reason = match phase {
            Phase::Activate | Phase::Commit => RESULT_UNKNOWN,
            Phase::AudioBegin | Phase::AudioChunk | Phase::AudioCommit | Phase::AudioAbort => {
                "无线音效操作结果未确认，请核对设备。"
            }
            Phase::Slots => "无法确认设备形象槽位，尚未发送素材。",
            _ => "Wi-Fi 形象传输已中断，尚未提交新形象。",
        };
        let mut retries = 0;
        let retry_until = Instant::now() + Duration::from_secs(10);
        let bytes = loop {
            self.check_route()?;
            if !matches!(phase, Phase::AudioAbort) {
                self.usb.ensure_appearance_sync_not_cancelled(None)?;
            }
            let mut submitted = false;
            match exchange_link_observed(self.peer, MODE, &body, Some(REPLY_MAX), timeout,
                                         None, || submitted = true) {
                Ok(link) if retries < 3 && Instant::now() < retry_until && (
                    link.reply == [2] || (link.reply == [0] &&
                    retryable_exchange(phase, payload["path"].as_str(), true, std::io::ErrorKind::WouldBlock))
                ) => {
                    // 2 is the authenticated worker receipt for a request that never ran.
                    // Released firmware's 0 has no such guarantee; only replay a safe operation.
                    transfer_log::record("appearance", "failure_metadata", json!({"phase":phase.label(),
                        "transport":"wifi", "automaticRetry":true, "attempt":retries+1,
                        "reason":if link.reply == [2] {"not_submitted"} else {"not_observed"}}));
                }
                Ok(link) => break link.reply,
                Err(error) => {
                    let retry = retries < 3 && Instant::now() < retry_until && retryable_exchange(phase, payload["path"].as_str(), submitted, error.kind());
                    transfer_log::record("appearance", "failure_metadata", json!({
                        "transport":"wifi", "phase":phase.label(), "writeStarted":submitted,
                        "automaticRetry":retry, "index":payload["index"].as_str().and_then(|n| n.parse::<u64>().ok()),
                        "errorKind": if error.kind() == std::io::ErrorKind::InvalidData { "malformed" }
                                     else if submitted { "ack_read_failed" } else { "not_submitted" },
                    }));
                    if !retry { return Err(reason.into()); }
                    // Same authenticated peer, same boot bound in the immutable body, same transfer ID.
                }
            }
            retries += 1;
            std::thread::sleep(Duration::from_millis(180 * retries));
        };
        self.check_route()?;
        if bytes == [2] { return Err("设备暂时繁忙，此步骤尚未执行。".into()); }
        if bytes == [0] {
            return Err(match phase {
                Phase::AudioBegin | Phase::AudioChunk | Phase::AudioCommit | Phase::AudioAbort => {
                    reason
                }
                Phase::Activate | Phase::Commit => RESULT_UNKNOWN,
                _ => NOT_READY,
            }
            .into());
        }
        let reply: Value = serde_json::from_slice(&bytes).map_err(|_| reason)?;
        let ack = &reply["payload"];
        if reply["topic"] != "asset/ack"
            || ack["phase"] != phase.label()
            || ack["transferId"] != self.id
            || (matches!(phase, Phase::Chunk | Phase::File) && ack["path"] != payload["path"])
            || (matches!(phase, Phase::Chunk) && ack["index"] != payload["index"])
        {
            return Err(reason.into());
        }
        if !matches!(phase, Phase::Slots) && ack["ok"] != true {
            if matches!(
                phase,
                Phase::AudioBegin | Phase::AudioChunk | Phase::AudioCommit | Phase::AudioAbort
            ) {
                return Err(if ack["errorCode"] == "audio_patch_rejected" {
                    "设备已拒绝无线提示音操作。"
                } else {
                    reason
                }
                .into());
            }
            return Err(if ack["errorCode"] == "appearance_query_outcome_unknown"
                || ack["error"]
                    .as_str()
                    .is_some_and(|error| error.contains("unknown"))
            {
                RESULT_UNKNOWN
            } else {
                NOT_READY
            }
            .into());
        }
        Ok(ack.clone())
    }

    fn global_cues(&self, directory: &Path) -> Result<(u32, u64, bool), String> {
        let current = || {
            self.check_route()?;
            self.usb.ensure_appearance_sync_not_cancelled(None)
        };
        sync_global_cues(
            directory,
            self.board,
            self.boot,
            &self.id,
            current,
            |phase, payload| {
                self.check_route()?;
                self.request(
                    match phase {
                        GlobalCuePhase::Slots => Phase::Slots,
                        GlobalCuePhase::Begin => Phase::AudioBegin,
                        GlobalCuePhase::Chunk => Phase::AudioChunk,
                        GlobalCuePhase::Commit => Phase::AudioCommit,
                        GlobalCuePhase::Abort => Phase::AudioAbort,
                    },
                    payload.clone(),
                )
            },
            true,
            |_, _, _, _| {},
        )
    }

    fn slots(&self) -> Result<P4AppearanceSlotState, String> {
        let deadline = Instant::now() + Duration::from_secs(5);
        loop {
            let nonce = uuid::Uuid::new_v4().to_string();
            let ack = self.request(
                Phase::Slots,
                json!({
                    "transferId": self.id, "queryNonce": nonce, "expectedBoardDeviceId": self.board,
                    "expectedBootId": self.boot, "observationSchema": "p4-appearance-quiescent-v1",
                }),
            )?;
            if ack["serialized"] != true
                || ack["queryNonce"] != nonce
                || ack["boardDeviceId"] != self.board
                || ack["bootId"] != self.boot
                || ack["observationSchema"] != "p4-appearance-quiescent-v1"
            {
                return Err(NOT_READY.into());
            }
            if ack["errorCode"] == "appearance_query_outcome_unknown" {
                return Err(RESULT_UNKNOWN.into());
            }
            if ack["ok"] == true && ack["quiescent"] == true && ack["snapshotChanged"] == false {
                return parse_p4_appearance_slot_state(&ack).map_err(|_| NOT_READY.into());
            }
            // Retry only read-only, scoped reports that explicitly indicate transient work.
            let busy = ack["errorCode"] == "appearance_query_busy"
                || (ack["ok"] == true && ack["quiescent"] == false);
            if !busy || Instant::now() >= deadline {
                return Err(NOT_READY.into());
            }
            std::thread::sleep(Duration::from_millis(200));
        }
    }

    fn confirm_active(&self, slot: u32, pack_id: &str) -> Result<(), String> {
        let observed = self.slots().map_err(|_| RESULT_UNKNOWN)?;
        if !confirmed_active_pack(&observed, slot, pack_id) {
            return Err(RESULT_UNKNOWN.into());
        }
        Ok(())
    }

    fn activate(&self, slot: &P4AppearanceSlot) -> Result<(), String> {
        match self.request(
            Phase::Activate,
            json!({ "transferId": self.id, "slot": slot.slot, "packId": slot.pack_id }),
        ) {
            Ok(_) => self.confirm_active(slot.slot, &slot.pack_id),
            // A reply can be lost after publication. Re-read the device, never resend.
            Err(error) if error == RESULT_UNKNOWN => self.confirm_active(slot.slot, &slot.pack_id),
            Err(error) => Err(error),
        }
    }

    fn sync(
        &self,
        dir: &Path,
        builtin: bool,
        emit_progress: impl Fn(Value),
    ) -> Result<AppearanceSyncOutcome, String> {
        let prepared = if builtin {
            None
        } else {
            Some(prepared_p4_lan_files(dir)?)
        };
        let slots = self.slots()?;
        let plan = appearance_plan(&slots, prepared.as_ref().map(|(pack, _)| pack.pack_id.as_str()))?;
        let reason = match &plan {
            AppearancePlan::Keep => "already_active",
            AppearancePlan::Activate(_) => "activate_cached",
            AppearancePlan::Upload if slots.slots.iter().any(|s| s.slot == P4_RAW_APPEARANCE_SLOT) => "different_pack",
            AppearancePlan::Upload => "custom_absent",
        };
        transfer_log::record("appearance", "slot_decision", json!({"transport":"wifi", "reason":reason}));
        match plan {
            AppearancePlan::Keep => return Ok(AppearanceSyncOutcome::AlreadyActive),
            AppearancePlan::Activate(slot) => {
                emit_progress(json!({ "stage": "activating" }));
                self.activate(slot)?;
                return Ok(AppearanceSyncOutcome::Activated);
            }
            AppearancePlan::Upload => {}
        }

        let (pack, files) = prepared.ok_or(NOT_READY)?;
        p4_builtin_slot(&slots)?;
        emit_progress(json!({ "stage": "preparing", "reason": reason }));
        let capacity = u64::from(self.capacity);
        let mut total = 0_u64;
        let mut raw = 0_u64;
        let mut payloads = Vec::with_capacity(files.len());
        // Fully read the upload before activating the factory fallback or erasing slot 1.
        for (path, file) in files {
            self.usb.ensure_appearance_sync_not_cancelled(None)?;
            let input = std::fs::File::open(file).map_err(|_| PREPARED_ASSET_ERROR)?;
            let length = input.metadata().map_err(|_| PREPARED_ASSET_ERROR)?.len();
            total = total.checked_add(length).ok_or(NOT_READY)?;
            if path.ends_with(".h264") || path.ends_with(".mjpg") {
                raw = raw
                    .next_multiple_of(4)
                    .checked_add(length)
                    .ok_or(NOT_READY)?;
            }
            if length == 0 || raw > capacity || total > capacity + 2 * 1024 * 1024 {
                return Err("形象素材超出设备的可用空间，未发送素材。".into());
            }
            let mut bytes = Vec::new();
            input
                .take(length + 1)
                .read_to_end(&mut bytes)
                .map_err(|_| PREPARED_ASSET_ERROR)?;
            if bytes.len() as u64 != length {
                return Err(PREPARED_ASSET_ERROR.into());
            }
            payloads.push((path, bytes));
        }
        if total != pack.byte_count || !verified_p4_payload_snapshot(&payloads, &pack.pack_id) {
            return Err(PREPARED_ASSET_ERROR.into());
        }
        // Local pack preparation may take time. Re-observe before the first
        // mutation so a newly available matching custom slot is never erased.
        let fresh = self.slots()?;
        match appearance_plan(&fresh, Some(&pack.pack_id))? {
            AppearancePlan::Keep => return Ok(AppearanceSyncOutcome::AlreadyActive),
            AppearancePlan::Activate(slot) => {
                emit_progress(json!({ "stage": "activating" }));
                self.activate(slot)?;
                return Ok(AppearanceSyncOutcome::Activated);
            }
            AppearancePlan::Upload => {}
        }
        if slots != fresh {
            return Err("准备素材期间设备槽位已变化，请重新选择形象。".into());
        }
        if fresh.active_slot != Some(P4_BUILTIN_APPEARANCE_SLOT) {
            self.activate(p4_builtin_slot(&fresh)?)?;
        }
        transfer_log::record(
            "appearance",
            "transaction_started",
            json!({
                "transport": "wifi", "totalBytes": total, "rawBytes": raw, "fileCount": payloads.len(),
            }),
        );
        self.request(
            Phase::Begin,
            json!({ "transferId": self.id, "totalBytes": total, "rawBytes": raw }),
        )?;
        let mut sent = 0;
        for (file_no, (path, data)) in payloads.iter().enumerate() {
            for (index, chunk) in data.chunks(CHUNK_BYTES).enumerate() {
                self.request(
                    Phase::Chunk,
                    json!({
                        "transferId": self.id, "path": path, "index": index.to_string(),
                        "size": chunk.len(), "data": STANDARD.encode(chunk),
                    }),
                )?;
                sent += chunk.len() as u64;
                emit_progress(json!({
                    "stage": "transferring", "currentFile": file_no + 1, "totalFiles": payloads.len(),
                    "bytesSent": sent, "bytesTotal": total,
                }));
            }
            self.request(
                Phase::File,
                build_asset_file_commit_payload(
                    &self.id,
                    path,
                    data.len() as u64,
                    &asset_checksum_hex(data),
                    data.len().div_ceil(CHUNK_BYTES) as u64,
                ),
            )?;
        }
        match self.request(
            Phase::Commit,
            json!({ "transferId": self.id, "fileCount": payloads.len(), "totalBytes": total }),
        ) {
            Ok(_) => {}
            Err(error) if error == RESULT_UNKNOWN => {}
            Err(error) => return Err(error),
        }
        self.confirm_active(P4_RAW_APPEARANCE_SLOT, &pack.pack_id)?;
        transfer_log::record(
            "appearance",
            "transaction_committed",
            json!({
                "transport": "wifi", "bytes": total, "files": payloads.len(),
            }),
        );
        Ok(AppearanceSyncOutcome::Uploaded)
    }
}

#[tauri::command]
pub(crate) async fn lan_sync_appearance(
    app: tauri::AppHandle,
    usb_manager: tauri::State<'_, UsbSerialManager>,
    appearance_id: String,
    board_device_id: String,
    boot_id: String,
    request_id: Option<String>,
) -> Result<AppearanceSyncOutcome, String> {
    let appearance_id = crate::validate_appearance_id(&appearance_id)?.to_string();
    let dir = app
        .path()
        .app_local_data_dir()
        .map_err(|_| "无法读取本机形象库。")?
        .join("custom-appearances")
        .join(&appearance_id);
    with_current_slots(usb_manager.inner().clone(), board_device_id, boot_id, request_id.clone(),
        move |transfer| transfer.sync(&dir, appearance_id == "builtin-terrier", |mut event| {
            event["requestId"] = json!(request_id);
            event["boardDeviceId"] = json!(transfer.board);
            event["bootId"] = json!(transfer.boot);
            let _ = app.emit("wifi-appearance-progress", event);
        })).await
}

#[tauri::command]
pub(crate) async fn lan_read_appearance_slot(
    usb_manager: tauri::State<'_, UsbSerialManager>, board_device_id: String, boot_id: String,
) -> Result<u32, String> {
    with_current_slots(usb_manager.inner().clone(), board_device_id, boot_id, None, |transfer| {
        let snapshot = transfer.slots()?;
        let active = snapshot.active_slot.ok_or(NOT_READY)?;
        if !snapshot.slots.iter().any(|slot| slot.slot == active && !slot.pack_id.is_empty()) {
            return Err(NOT_READY.into());
        }
        Ok(active)
    }).await
}

async fn with_current_slots<T: Send + 'static>(
    usb: UsbSerialManager, board_device_id: String, boot_id: String, request_id: Option<String>,
    operation: impl FnOnce(&Transfer<'_>) -> Result<T, String> + Send + 'static,
) -> Result<T, String> {

    require_no_usb(&usb)?;
    usb.begin_owned_appearance_sync(request_id)?;
    let worker = usb.clone();
    let result = tauri::async_runtime::spawn_blocking(move || {
        let _foreground = super::widget_data::foreground()?;
        let (peer, status) = read_with_recovery(query_status, "无法读取已配对的 Wi-Fi 设备。")?;
        if status.boot_id != boot_id
            || status.board_device_id != board_device_id
            || !status.wifi_connected
        {
            return Err("Wi-Fi 设备已变更，请确认连接后再切换形象。".into());
        }
        if status.appearance_capacity == 0 {
            return Err("当前固件需要升级后才能通过 Wi-Fi 切换形象。".into());
        }
        if !status.control_trusted {
            return Err(
                "请通过 USB 连接同一台设备一次，授权无线控制后即可通过 Wi-Fi 切换形象。".into(),
            );
        }
        let transfer = Transfer {
            pairing: appearance_pairing(&peer)?,
            peer: &peer,
            board: &board_device_id,
            boot: &boot_id,
            capacity: status.appearance_capacity,
            id: uuid::Uuid::new_v4().to_string(),
            usb: &worker,
        };
        operation(&transfer)
    })
    .await;
    usb.finish_appearance_sync();
    result.map_err(|_| "Wi-Fi 形象同步任务未完成。")?
}

fn global_cue_target(status: &super::LanDeviceStatus, board: &str, boot: &str) -> bool {
    status.global_audio_cues
        && status.control_trusted
        && status.wifi_connected
        && !board.is_empty()
        && status.board_device_id == board
        && boot.len() == 16
        && boot.bytes().all(|c| c.is_ascii_hexdigit())
        && status.boot_id == boot
}

#[tauri::command]
pub(crate) async fn lan_sync_global_audio_cues(
    app: tauri::AppHandle,
    usb_manager: tauri::State<'_, UsbSerialManager>,
    board_device_id: String,
    boot_id: String,
) -> Result<crate::UsbSyncAppearanceResult, String> {
    let directory = app
        .path()
        .app_local_data_dir()
        .map_err(|_| "无法读取本机提示音。")?;
    let usb = usb_manager.inner().clone();
    require_no_usb(&usb)?;
    usb.begin_appearance_sync()?;
    let worker = usb.clone();
    let result = tauri::async_runtime::spawn_blocking(move || {
        let _foreground = super::widget_data::foreground()?;
        let (peer, status) = read_with_recovery(query_status, "无法读取已配对的 Wi-Fi 设备。")?;
        if !global_cue_target(&status, &board_device_id, &boot_id) {
            return Err("需要本次启动已授权且支持无线全局提示音的设备。".into());
        }
        let transfer = Transfer {
            pairing: appearance_pairing(&peer)?,
            peer: &peer,
            board: &board_device_id,
            boot: &boot_id,
            capacity: status.appearance_capacity,
            id: uuid::Uuid::new_v4().to_string(),
            usb: &worker,
        };
        let (file_count, byte_count, reused_slot) = transfer.global_cues(&directory)?;
        Ok(crate::UsbSyncAppearanceResult {
            ok: true,
            file_count,
            byte_count,
            reused_slot,
            error: None,
        })
    })
    .await;
    usb.finish_appearance_sync();
    result.map_err(|_| "无线音效任务结果未确认。")?
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn wifi_lost_ack_retries_only_read_or_identical_raw_video_chunk() {
        use std::io::ErrorKind::*;
        for kind in [TimedOut, UnexpectedEof, ConnectionReset] {
            assert!(retryable_exchange(Phase::Chunk, Some("p4/a.h264"), true, kind));
            assert!(retryable_exchange(Phase::Slots, None, true, kind));
            for phase in [Phase::Begin, Phase::File, Phase::Commit, Phase::Activate, Phase::AudioChunk] {
                assert!(!retryable_exchange(phase, None, true, kind));
                assert!(retryable_exchange(phase, None, false, kind));
            }
            assert!(!retryable_exchange(Phase::Chunk, Some("p4/manifest.json"), true, kind));
        }
        assert!(!retryable_exchange(Phase::Chunk, Some("p4/a.h264"), true, InvalidData));
    }

    #[test]
    fn authenticated_missing_chunk_receipt_retries_identical_bytes_only() {
        use crate::lan_agent::{confirmation_tests::{accept, assert_no_replay}, nonce, session_key};
        use ring::aead;
        use std::io::{Read, Write};
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let master = [0x71; 16];
        let peer = Peer { address: listener.local_addr().unwrap(), key: zeroize::Zeroizing::new(master) };
        let server = std::thread::spawn(move || {
            let mut original = Vec::new();
            for attempt in 0..2 {
                let mut socket = accept(&listener);
                let mut client = [0; 16];
                socket.read_exact(&mut client).unwrap();
                let challenge = [attempt + 1; 16];
                socket.write_all(&challenge).unwrap();
                let key = session_key(&master, &client, &challenge);
                let mut header = [0; 3];
                socket.read_exact(&mut header).unwrap();
                assert_eq!(header[0], MODE);
                let mut frame = vec![0; u16::from_be_bytes([header[1],header[2]]) as usize + 16];
                socket.read_exact(&mut frame).unwrap();
                let plain = key.open_in_place(nonce(0), aead::Aad::from(header), &mut frame).unwrap();
                assert_eq!(plain[16], Phase::Chunk as u8);
                let mut reply = if attempt == 0 {
                    original = plain.to_vec();
                    vec![0]
                } else {
                    assert_eq!(plain, original.as_slice());
                    serde_json::to_vec(&json!({"topic":"asset/ack","payload":{"phase":"chunk",
                        "ok":true,"transferId":"fixture","path":"p4/video.h264","index":"7"}})).unwrap()
                };
                let length = (reply.len() as u16).to_be_bytes();
                let aad = [header.as_slice(),length.as_slice()].concat();
                key.seal_in_place_append_tag(nonce(1),aead::Aad::from(&aad),&mut reply).unwrap();
                socket.write_all(&length).unwrap();
                socket.write_all(&reply).unwrap();
            }
            listener
        });
        let usb = UsbSerialManager::new();
        let transfer = Transfer { peer: &peer, board:"fixture-board", boot:"0123456789abcdef",
            pairing: super::super::component_events::pairing_revision(), capacity: 4096, id:"fixture".into(), usb:&usb };
        let ack = transfer.request(Phase::Chunk, json!({"transferId":"fixture","path":"p4/video.h264",
            "index":"7","size":3,"data":"AQID"})).unwrap();
        assert_eq!(ack["ok"], true);
        assert_no_replay(server.join().unwrap());
    }

    #[test]
    fn retired_pairing_rejects_even_a_read_before_sending() {
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let usb = UsbSerialManager::new();
        let peer = Peer { address: listener.local_addr().unwrap(), key: zeroize::Zeroizing::new([0; 16]) };
        let transfer = Transfer { peer: &peer, board: "fixture", boot: "0123456789abcdef",
            capacity: 1, pairing: 0, id: "fixture".into(), usb: &usb };
        assert!(matches!(transfer.slots(), Err(e) if e.contains("配对已变化")));
        crate::lan_agent::confirmation_tests::assert_no_replay(listener);
    }

    fn slots(active_slot: u32, has_custom: bool) -> P4AppearanceSlotState {
        let mut slots = vec![P4AppearanceSlot {
            slot: P4_BUILTIN_APPEARANCE_SLOT,
            pack_id: "factory".into(),
        }];
        if has_custom {
            slots.push(P4AppearanceSlot {
                slot: P4_RAW_APPEARANCE_SLOT,
                pack_id: "cached".into(),
            });
        }
        P4AppearanceSlotState {
            active_slot: Some(active_slot),
            slots,
        }
    }

    #[test]
    fn builtin_is_only_checked_or_activated_even_when_custom_slot_is_invalid() {
        let active = slots(P4_BUILTIN_APPEARANCE_SLOT, false);
        assert_eq!(
            appearance_plan(&active, None).unwrap(),
            AppearancePlan::Keep
        );
        let inactive = slots(P4_RAW_APPEARANCE_SLOT, true);
        assert_eq!(
            appearance_plan(&inactive, None).unwrap(),
            AppearancePlan::Activate(&inactive.slots[0])
        );
        assert!(appearance_plan(
            &P4AppearanceSlotState {
                active_slot: None,
                slots: vec![]
            },
            None
        )
        .is_err());
    }

    #[test]
    fn custom_upload_depends_on_the_device_pack_and_active_slot() {
        let active = slots(P4_RAW_APPEARANCE_SLOT, true);
        assert_eq!(
            appearance_plan(&active, Some("cached")).unwrap(),
            AppearancePlan::Keep
        );
        let inactive = slots(P4_BUILTIN_APPEARANCE_SLOT, true);
        assert_eq!(
            appearance_plan(&inactive, Some("cached")).unwrap(),
            AppearancePlan::Activate(&inactive.slots[1])
        );
        assert_eq!(
            appearance_plan(&inactive, Some("other")).unwrap(),
            AppearancePlan::Upload
        );
        assert_eq!(
            appearance_plan(&slots(P4_BUILTIN_APPEARANCE_SLOT, false), Some("cached")).unwrap(),
            AppearancePlan::Upload
        );
        assert!(!confirmed_active_pack(
            &inactive,
            P4_RAW_APPEARANCE_SLOT,
            "cached"
        ));
        assert!(!confirmed_active_pack(
            &active,
            P4_RAW_APPEARANCE_SLOT,
            "other"
        ));
        assert!(confirmed_active_pack(
            &active,
            P4_RAW_APPEARANCE_SLOT,
            "cached"
        ));
    }
}

#[cfg(test)]
#[path = "global_cues_tests.rs"]
mod global_cues_tests;
