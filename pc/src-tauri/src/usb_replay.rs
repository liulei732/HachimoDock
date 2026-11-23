// Only the last connection owns the transient speech restore state.
use crate::usb_serial::UsbConnectionStatus;
use serde_json::Value;
use std::sync::{Mutex, OnceLock};

struct Snapshot {
    board: String,
    connection: String,
    boot: String,
    source: String,
    payload: Value,
}

static CACHE: OnceLock<Mutex<Option<Snapshot>>> = OnceLock::new();

pub(super) fn same_connection(
    current: &UsbConnectionStatus,
    expected: &UsbConnectionStatus,
) -> bool {
    current.connected
        && expected.connected
        && !expected.connection_id.is_empty()
        && current.connection_id == expected.connection_id
        && current.boot_id == expected.boot_id
        && current.board_device_id == expected.board_device_id
}

pub(super) fn store(owner: &UsbConnectionStatus, source: &str, payload: &Value) {
    if let Ok(mut slot) = CACHE.get_or_init(Default::default).lock() {
        *slot = Some(Snapshot {
            board: owner.board_device_id.clone(),
            connection: owner.connection_id.clone(),
            boot: owner.boot_id.clone(),
            source: source.to_owned(),
            payload: payload.clone(),
        });
    }
}

pub(super) fn load(owner: &UsbConnectionStatus) -> Option<(String, Value)> {
    let guard = CACHE.get()?.lock().ok()?;
    let value = guard.as_ref()?;
    (owner.connected
        && !owner.connection_id.is_empty()
        && owner.connection_id == value.connection
        && owner.boot_id == value.boot
        && owner.board_device_id == value.board)
        .then(|| (value.source.clone(), value.payload.clone()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn delayed_speech_restore_requires_the_same_connection_boot_and_device() {
        let active: UsbConnectionStatus = serde_json::from_value(serde_json::json!({
            "connected": true, "boardDeviceId":"test-board", "connectionId":"7", "bootId":"first",
            "portName":"test", "transport":"usb", "runtime":"esp-p4", "deviceModel":"",
            "firmware":"", "wireProtocol":"", "capabilities":{}
        }))
        .unwrap();
        store(
            &active,
            "codex",
            &serde_json::json!({"sessionId":"example"}),
        );
        assert!(same_connection(&active, &active));
        assert!(load(&active).is_some());
        let mut other = active.clone();
        other.boot_id = "second".into();
        assert!(!same_connection(&active, &other));
        assert!(load(&other).is_none());
        other = active.clone();
        other.connection_id = "8".into();
        assert!(load(&other).is_none());
        other = active.clone();
        other.board_device_id = "another-board".into();
        assert!(load(&other).is_none());
    }
}
