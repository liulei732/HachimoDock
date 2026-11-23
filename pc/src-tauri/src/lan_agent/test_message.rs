//! A read-only session capture followed by at most one fixed notice. No mutation retries.
use super::{exchange_response_with_timeout, query_status, read_with_recovery, require_no_usb, PANEL_CONTROL};
use crate::usb_serial::UsbSerialManager;
use serde_json::{json, Value};
use std::{sync::Mutex, time::Duration};

const PANEL_TEST_MESSAGE: u8 = 13;
const UNKNOWN: &str = "设备回执未确认。请查看桌宠主屏，本次不会自动重发。";
static SENDING: Mutex<()> = Mutex::new(());

fn once(board: &str, boot: &str, mut exchange: impl FnMut(&Value) -> Result<Value, String>) -> Result<(), String> {
    let intent = uuid::Uuid::new_v4().simple().to_string();
    let mut request = json!({"phase":"prepare","boardDeviceId":board,"bootId":boot,"intent":intent});
    let checked = |reply: &Value| reply["ok"] == true && reply["intent"] == intent
        && reply["boardDeviceId"] == board && reply["bootId"] == boot;
    let ready = exchange(&request).map_err(|_| "未发送测试消息。请确认设备在线且固件支持测试消息回执。")?;
    if !checked(&ready) { return Err("设备或会话无法确认，测试消息未发送。".into()); }
    if ready["status"] == "busy" { return Err("请回到桌宠主屏并结束占用屏幕的操作，再发送测试消息。".into()); }
    let token = ready["token"].as_u64().filter(|t| (1..=u64::from(u32::MAX)).contains(t))
        .filter(|_| ready["status"] == "ready").ok_or("设备未准备好接收测试消息。")?;
    request["phase"] = "send".into();
    request["token"] = token.into();
    let receipt = exchange(&request).map_err(|_| UNKNOWN)?;
    if !checked(&receipt) || receipt["token"] != token { return Err(UNKNOWN.into()); }
    match receipt["status"].as_str() {
        Some("delivered") => Ok(()),
        Some("changed" | "expired") => Err("设备会话已变化或请求已过期，测试消息未投递。".into()),
        Some("busy" | "refused") => Err("设备拒绝了测试消息。请确认桌宠主屏空闲。".into()),
        _ => Err(UNKNOWN.into()),
    }
}

#[tauri::command]
pub(crate) async fn send_device_test_message(
    usb_manager: tauri::State<'_, UsbSerialManager>, board_device_id: String, boot_id: String,
    connection_id: Option<String>,
) -> Result<(), String> {
    if board_device_id.is_empty() || boot_id.len() != 16 || !boot_id.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err("设备身份尚未确认。".into());
    }
    let usb = usb_manager.inner().clone();
    tauri::async_runtime::spawn_blocking(move || {
        let _sending = SENDING.try_lock().map_err(|_| "正在发送测试消息，请等待本次回执。")?;
        if let Some(connection) = connection_id {
            let generation = connection.parse::<u64>().map_err(|_| "USB 连接代次无效")?;
            let current = || {
                let s = usb.status();
                (s.connected && s.connection_id == connection && s.boot_id == boot_id
                    && s.board_device_id == board_device_id && s.capabilities["testMessageReceipt"] == true)
                    .then_some(()).ok_or_else(|| "USB 设备或启动身份已变化。".to_string())
            };
            current()?;
            return once(&board_device_id, &boot_id, |q| {
                current()?;
                let r = usb.test_message_exchange(&board_device_id, generation, q.clone())?;
                current()?;
                Ok(r)
            });
        }
        require_no_usb(&usb)?;
        let (peer, status) = read_with_recovery(query_status, "无线设备未连接。")?;
        if !status.wifi_connected || !status.control_trusted || !status.panel_control ||
            status.board_device_id != board_device_id || status.boot_id != boot_id {
            return Err("目标设备尚未授权或本次启动已变化。".into());
        }
        let _lane = super::widget_data::foreground()?;
        let revision = {
            let _lock = super::PAIRING_MUTATION.lock().map_err(|_| "配对记录繁忙")?;
            if !super::read_peer()?.is_some_and(|now| super::widget_data::same_peer(&now, &peer)) {
                return Err("无线配对已变化。".into());
            }
            super::component_events::pairing_revision()
        };
        let current = || {
            require_no_usb(&usb)?;
            (revision == super::component_events::pairing_revision()).then_some(())
                .ok_or_else(|| "无线配对已变化。".to_string())
        };
        once(&board_device_id, &boot_id, |q| {
            current()?;
            let mut body = boot_id.as_bytes().to_vec();
            body.push(PANEL_TEST_MESSAGE);
            serde_json::to_writer(&mut body, q).map_err(|_| "消息参数无效")?;
            if body.len() > 273 { return Err("测试消息超过设备上限。".into()); }
            let bytes = exchange_response_with_timeout(&peer, PANEL_CONTROL, &body, Some(512), Duration::from_secs(4))
                .map_err(|_| "设备未返回测试消息回执")?;
            current()?;
            serde_json::from_slice(&bytes).map_err(|_| "设备未提供有效的测试消息回执".into())
        })
    }).await.map_err(|_| UNKNOWN.to_string())?
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn test_message_receipt_is_scoped_and_commit_is_never_repeated() {
        for case in ["ok","old","wrongBoot","busy","switch","lost","wrongToken","wrongIntent"] {
            let mut writes = 0;
            let result = once("board", "0123456789abcdef", |q| {
                let mut r = q.clone();
                r["ok"] = true.into();
                r["token"] = 7.into();
                if q["phase"] == "prepare" {
                    if case == "old" { return Err("unsupported".into()); }
                    r["status"] = (if case == "busy" { "busy" } else { "ready" }).into();
                    if case == "wrongBoot" { r["bootId"] = "stale".into(); }
                } else {
                    writes += 1;
                    assert_eq!(q["token"], 7);
                    if case == "lost" { return Err("timeout".into()); }
                    if case == "wrongIntent" { r["intent"] = "stale".into(); }
                    if case == "wrongToken" { r["token"] = 8.into(); }
                    r["status"] = (if case == "switch" { "changed" } else { "delivered" }).into();
                }
                Ok(r)
            });
            assert_eq!(result.is_ok(), case == "ok");
            assert_eq!(writes, usize::from(!matches!(case, "old"|"wrongBoot"|"busy")));
        }
    }
}
