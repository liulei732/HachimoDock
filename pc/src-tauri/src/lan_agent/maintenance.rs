//! Paired maintenance on a single observed boot and Wi-Fi association.
use super::{
    component, component_events, exchange_response_with_timeout, query_component_status, read_peer,
    require_no_usb, widget_data, ComponentTarget, LanDeviceStatus, Peer, PAIRING_MUTATION,
};
use crate::{app_program_background as program, usb_serial::UsbSerialManager};
use serde::Deserialize;
use serde_json::{json, Value};
use std::{collections::HashSet, path::Path, sync::Mutex, time::Duration};
use tauri::{Emitter, Manager};

pub(super) const MODE: u8 = 26;
const CAPABILITY: &str = "paired-component-maintenance-v1";
const STALE: &str = "无线维护目标已变化，请刷新设备状态。";
static LANE: Mutex<()> = Mutex::new(());

#[derive(Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub(crate) struct Scope {
    pub boot_id: String,
    pub association: u32,
}

struct Session {
    peer: Peer,
    revision: u64,
    board: String,
    boot: String,
    association: u32,
    caps: Value,
}

fn attested<'a>(
    status: &'a LanDeviceStatus,
    board: &str,
    scope: &Scope,
) -> Option<&'a ComponentTarget> {
    let t = status.component_target.as_ref()?;
    (status.control_trusted
        && status.wifi_connected
        && status.component_bundle
        && status.board_device_id == board
        && status.boot_id == scope.boot_id
        && t.board_device_id == board
        && t.boot_id == scope.boot_id
        && scope.association != 0
        && scope.association == t.association
        && t.capabilities["widgetMaintenance"] == CAPABILITY
        && widget_data::valid_identity(board, &scope.boot_id))
    .then_some(t)
}

impl Session {
    fn open(usb: &UsbSerialManager, board: &str, scope: &Scope) -> Result<Self, String> {
        require_no_usb(usb)?;
        let (peer, revision) = {
            let _guard = PAIRING_MUTATION.lock().map_err(|_| STALE)?;
            (
                read_peer()?.ok_or(STALE)?,
                component_events::pairing_revision(),
            )
        };
        let status = query_component_status(&peer).map_err(|_| STALE)?;
        let t = attested(&status, board, scope).ok_or(STALE)?;
        let session = Self {
            peer,
            revision,
            board: board.into(),
            boot: t.boot_id.clone(),
            association: t.association,
            caps: t.capabilities.clone(),
        };
        session.current(usb)?;
        Ok(session)
    }

    fn current(&self, usb: &UsbSerialManager) -> Result<(), String> {
        require_no_usb(usb)?;
        if component_events::pairing_revision() != self.revision || usb.transfer_in_progress() {
            return Err(STALE.into());
        }
        Ok(())
    }

    fn prefix(&self, op: u8) -> Vec<u8> {
        let mut body = self.boot.as_bytes().to_vec();
        body.extend_from_slice(self.board.as_bytes());
        body.extend_from_slice(&self.association.to_be_bytes());
        body.push(op);
        body
    }

    fn request(&self, usb: &UsbSerialManager, op: u8, mut request: Value) -> Result<Value, String> {
        self.current(usb)?;
        let rid = uuid::Uuid::new_v4().simple().to_string();
        request["requestId"] = json!(rid);
        let mut wire = self.prefix(op);
        serde_json::to_writer(&mut wire, &request).map_err(|_| STALE)?;
        if wire.len() > 548 {
            return Err("无线维护请求超出上限".into());
        }
        let bytes = exchange_response_with_timeout(
            &self.peer,
            MODE,
            &wire,
            Some(super::MAX_BODY - 2),
            Duration::from_secs(12),
        )
        .map_err(|_| "设备未返回无线维护回执")?;
        self.current(usb)?;
        let reply: Value = serde_json::from_slice(&bytes).map_err(|_| STALE)?;
        if !receipt_matches(&reply, &self.board, &self.boot, self.association, &rid) {
            return Err(STALE.into());
        }
        Ok(reply)
    }

    fn inventory(&self, usb: &UsbSerialManager) -> Result<Vec<Value>, String> {
        if self.caps["widgetInventory"] != true {
            return Err("设备未声明组件清单能力".into());
        }
        inventory_items(self.request(usb, 0, json!({}))?)
    }

    fn require_program(&self) -> Result<(), String> {
        program::supported(&self.caps)
            .then_some(())
            .ok_or_else(|| "设备尚未开放后台程序管理".into())
    }
}

fn receipt_matches(reply: &Value, board: &str, boot: &str, association: u32, rid: &str) -> bool {
    reply["requestId"] == rid
        && reply["boardDeviceId"] == board
        && reply["bootId"] == boot
        && reply["association"].as_u64() == Some(association.into())
}

fn inventory_items(mut reply: Value) -> Result<Vec<Value>, String> {
    let invalid = "设备组件清单不完整，已停止维护";
    if reply["ok"] != true || reply["complete"] != true || reply["recoveryBlocked"] != false {
        return Err(invalid.into());
    }
    let limit = reply["maxInstalled"]
        .as_u64()
        .filter(|n| *n > 0)
        .ok_or(invalid)?;
    let items = reply["items"].as_array_mut().ok_or(invalid)?;
    let mut ids = HashSet::new();
    if items.len() as u64 > limit
        || !items.iter().all(|v| {
            matches!(
                v["resourceState"].as_str(),
                Some("ready" | "damaged" | "bootstrap" | "unchecked")
            ) && v["id"]
                .as_str()
                .is_some_and(|id| program::valid_widget_id(id) && ids.insert(id))
        })
    {
        return Err(invalid.into());
    }
    Ok(std::mem::take(items))
}

pub(crate) fn query(
    usb: &UsbSerialManager,
    board: &str,
    scope: &Scope,
) -> Result<program::Snapshot, String> {
    let _lane = LANE.try_lock().map_err(|_| "设备维护繁忙")?;
    let _foreground = widget_data::foreground()?;
    let s = Session::open(usb, board, scope)?;
    s.require_program()?;
    program::snapshot(&s.request(usb, 1, json!({}))?, board, Some(&scope.boot_id))
}

pub(crate) fn start(
    usb: &UsbSerialManager,
    board: &str,
    scope: &Scope,
    id: &str,
) -> Result<program::Outcome, String> {
    if !program::valid_widget_id(id) {
        return Err("组件 ID 无效".into());
    }
    let _lane = LANE.try_lock().map_err(|_| "设备维护繁忙")?;
    let _foreground = widget_data::foreground()?;
    let s = Session::open(usb, board, scope)?;
    s.require_program()?;
    if !s.inventory(usb)?.iter().any(|v| v["id"] == id) {
        return Err("设备未安装此组件".into());
    }
    let content = program::installed(&s.request(usb, 2, json!({"widgetId":id}))?, board, id)?;
    Ok(match s.request(usb, 3, program::start_payload(&content)) {
        Ok(reply) => program::Outcome::started(&reply, board, &content),
        Err(_) => program::Outcome::unknown(),
    })
}

pub(crate) fn stop(
    usb: &UsbSerialManager,
    board: &str,
    scope: &Scope,
    id: &str,
    boot: &str,
    registration: &str,
) -> Result<program::Outcome, String> {
    if boot != scope.boot_id {
        return Err(STALE.into());
    }
    let payload = program::stop_payload(id, boot, registration)?;
    let _lane = LANE.try_lock().map_err(|_| "设备维护繁忙")?;
    let _foreground = widget_data::foreground()?;
    let s = Session::open(usb, board, scope)?;
    s.require_program()?;
    crate::program_autostart::native::suppress_now(board, boot);
    Ok(match s.request(usb, 4, payload) {
        Ok(reply) => program::Outcome::stopped(&reply, board, id, boot, registration),
        Err(_) => program::Outcome::unknown(),
    })
}

#[derive(Default)]
struct Repairs {
    owner: Option<(u64, String, String)>,
    attempted: HashSet<String>,
    running: bool,
    scanned: bool,
    next_read: Option<std::time::Instant>,
}
static REPAIRS: std::sync::LazyLock<Mutex<Repairs>> = std::sync::LazyLock::new(Default::default);

fn quiet(usb: &UsbSerialManager) -> bool {
    !usb.status().connected
        && !usb.transfer_in_progress()
        && !crate::realtime_chat::status().active
        && !crate::component_audio::voice_busy()
        && !crate::component_audio::active_for_exit()
}

// Trigger on the ordinary authenticated status observation. A failed read may be
// tried later. An attempted write is never repeated on another association of this boot.
pub(super) fn observe(app: &tauri::AppHandle, peer: &Peer, status: &LanDeviceStatus) {
    let Some(t) = &status.component_target else {
        return;
    };
    let scope = Scope {
        boot_id: t.boot_id.clone(),
        association: t.association,
    };
    if attested(status, &t.board_device_id, &scope).is_none()
        || t.capabilities["widgetResourceIntegrity"] != "p4-resource-integrity-v1"
    {
        return;
    }
    let Some(manager) = app.try_state::<UsbSerialManager>() else {
        return;
    };
    let usb = manager.inner().clone();
    if !quiet(&usb) {
        return;
    }
    let owner = (
        component_events::pairing_revision(),
        t.board_device_id.clone(),
        t.boot_id.clone(),
    );
    {
        let Ok(mut r) = REPAIRS.lock() else { return };
        if r.running {
            return;
        }
        if r.owner.as_ref() != Some(&owner) {
            *r = Repairs {
                owner: Some(owner.clone()),
                ..Default::default()
            };
        }
        if r.scanned
            || r.next_read
                .is_some_and(|when| when > std::time::Instant::now())
        {
            return;
        }
        r.running = true;
    }
    let app = app.clone();
    let expected = peer.clone();
    tauri::async_runtime::spawn_blocking(move || {
        let result = (|| -> Result<(), String> {
            let _lane = LANE.try_lock().map_err(|_| STALE)?;
            let _background = widget_data::background(&usb).ok_or(STALE)?;
            let s = Session::open(&usb, &owner.1, &scope)?;
            if s.revision != owner.0
                || !widget_data::same_peer(&s.peer, &expected)
                || s.caps["widgetResourceIntegrity"] != "p4-resource-integrity-v1"
            {
                return Err(STALE.into());
            }
            for row in s.inventory(&usb)? {
                if !matches!(row["resourceState"].as_str(), Some("bootstrap" | "damaged")) {
                    continue;
                }
                if !quiet(&usb) {
                    return Err("设备正忙".into());
                }
                let id = row["id"]
                    .as_str()
                    .filter(|id| program::valid_widget_id(id))
                    .ok_or(STALE)?;
                let Some(entry) = crate::component_library::bundled_repair_source(id)? else {
                    continue;
                };
                let mut files = UsbSerialManager::prepare_widget_delivery(
                    id,
                    Path::new(&entry.path),
                    &Default::default(),
                    None,
                    Some(&s.caps),
                )?;
                if files
                    .iter()
                    .any(|(p, _)| p == "runtime/app.wasm" || p == crate::wasm_audio::DEVICE_PATH)
                {
                    continue;
                }
                files.retain(|(_, b)| !b.is_empty());
                files.sort_by(|a, b| a.0.cmp(&b.0));
                s.current(&usb)?;
                if !REPAIRS
                    .lock()
                    .map_err(|_| STALE)?
                    .attempted
                    .insert(id.into())
                {
                    continue;
                }
                crate::program_autostart::native::component_changed(id, false);
                let result = component::repair_bundle(&s.peer, &s.prefix(5), id, &files, || {
                    if !quiet(&usb) {
                        return Err("设备正忙".into());
                    }
                    s.current(&usb)
                });
                // Even if the final ACK was lost, a fresh scoped inventory can establish the current state.
                let healthy = s.inventory(&usb).is_ok_and(|rows| {
                    rows.iter()
                        .any(|v| v["id"] == id && v["resourceState"] == "ready")
                });
                let _ = app.emit(
                    "component-resource-recovery",
                    json!({"componentId":id,"ok":healthy,
                    "error":if healthy{""}else{"无线资源修复结果未确认，请刷新组件清单。"}}),
                );
                if !healthy {
                    return Err(result.err().unwrap_or_else(|| STALE.into()));
                }
            }
            Ok(())
        })();
        if let Ok(mut r) = REPAIRS.lock() {
            r.running = false;
            r.scanned = result.is_ok();
            r.next_read = Some(std::time::Instant::now() + Duration::from_secs(30));
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::lan_agent::{
        confirmation_tests::{accept, assert_no_replay},
        nonce, session_key,
    };
    use ring::aead;
    use std::{
        io::{Read, Write},
        net::TcpListener,
    };
    use zeroize::Zeroizing;

    const BOARD: &str = "p4-001122334455";
    const BOOT: &str = "1122334455667788";

    #[test]
    fn inventory_requires_a_complete_unique_device_list() {
        let base = json!({"ok":true,"complete":true,"recoveryBlocked":false,"maxInstalled":4,
                        "items":[{"id":"demo","resourceState":"damaged"}]});
        assert_eq!(
            inventory_items(base.clone()).unwrap()[0]["resourceState"],
            "damaged"
        );
        for (field, value) in [
            ("ok", json!(false)),
            ("recoveryBlocked", json!(true)),
            ("complete", json!(false)),
            ("maxInstalled", json!(0)),
            (
                "items",
                json!([{"id":"demo","resourceState":"ready"},{"id":"demo","resourceState":"ready"}]),
            ),
            ("items", json!([{"id":"","resourceState":"ready"}])),
            ("items", json!([{"id":"demo"}])),
        ] {
            let mut bad = base.clone();
            bad[field] = value;
            assert!(inventory_items(bad).is_err());
        }
    }

    #[test]
    fn full_maintenance_witness_matches_board_boot_association_and_trust() {
        let scope = Scope {
            boot_id: BOOT.into(),
            association: 17,
        };
        let mut status: LanDeviceStatus =
            serde_json::from_value(json!({"firmware":"fixture","bootId":BOOT,
            "wifiConnected":true,"controlTrusted":true,"componentBundle":true,"boardDeviceId":BOARD,
            "clockSynchronized":false,"httpsSuccesses":0,"httpsFailures":0}))
            .unwrap();
        assert!(attested(&status, BOARD, &scope).is_none());
        status.component_target = Some(ComponentTarget {
            board_device_id: BOARD.into(),
            boot_id: BOOT.into(),
            association: 17,
            capabilities: json!({}),
        });
        assert!(attested(&status, BOARD, &scope).is_none());
        status.component_target.as_mut().unwrap().capabilities =
            json!({"widgetMaintenance":CAPABILITY});
        assert!(attested(&status, BOARD, &scope).is_some());
        assert!(attested(&status, "p4-000000000000", &scope).is_none());
        for wrong in [
            Scope {
                boot_id: BOOT.into(),
                association: 18,
            },
            Scope {
                boot_id: "ffffffffffffffff".into(),
                association: 17,
            },
        ] {
            assert!(attested(&status, BOARD, &wrong).is_none());
        }
        status.control_trusted = false;
        assert!(attested(&status, BOARD, &scope).is_none());
    }

    #[test]
    fn real_inventory_reader_binds_each_sealed_receipt_and_never_replays() {
        for fault in ["", "board", "boot", "association", "request", "tag", "lost"] {
            let listener = TcpListener::bind("127.0.0.1:0").unwrap();
            let session = Session {
                peer: Peer {
                    address: listener.local_addr().unwrap(),
                    key: Zeroizing::new([22; 16]),
                },
                revision: component_events::pairing_revision(),
                board: BOARD.into(),
                boot: BOOT.into(),
                association: 17,
                caps: json!({"widgetInventory":true}),
            };
            let server = std::thread::spawn(move || {
                let mut conn = accept(&listener);
                let mut client = [0; 16];
                conn.read_exact(&mut client).unwrap();
                let sn = [44; 16];
                conn.write_all(&sn).unwrap();
                let key = session_key(&[22; 16], &client, &sn);
                let mut h = [0; 3];
                conn.read_exact(&mut h).unwrap();
                assert_eq!(h[0], 26);
                let n = u16::from_be_bytes([h[1], h[2]]) as usize;
                let mut encrypted = vec![0; n + 16];
                conn.read_exact(&mut encrypted).unwrap();
                let body = key
                    .open_in_place(nonce(0), aead::Aad::from(h), &mut encrypted)
                    .unwrap();
                assert_eq!(&body[..16], BOOT.as_bytes());
                assert_eq!(&body[16..31], BOARD.as_bytes());
                assert_eq!(&body[31..36], &[0, 0, 0, 17, 0]);
                let q: Value = serde_json::from_slice(&body[36..]).unwrap();
                let rid = q["requestId"].as_str().unwrap();
                assert_eq!(rid.len(), 32);
                let mut reply = json!({"requestId":rid,"boardDeviceId":BOARD,"bootId":BOOT,"association":17,"ok":true,
                    "complete":true,"recoveryBlocked":false,"maxInstalled":16,"items":[{"id":"demo","resourceState":"ready"}]});
                match fault {
                    "board" => reply["boardDeviceId"] = json!("other"),
                    "boot" => reply["bootId"] = json!("another"),
                    "association" => reply["association"] = json!(16),
                    "request" => reply["requestId"] = json!("other"),
                    _ => {}
                }
                if fault != "lost" {
                    let mut ack = serde_json::to_vec(&reply).unwrap();
                    let len = (ack.len() as u16).to_be_bytes();
                    key.seal_in_place_append_tag(
                        nonce(1),
                        aead::Aad::from([26, h[1], h[2], len[0], len[1]]),
                        &mut ack,
                    )
                    .unwrap();
                    if fault == "tag" {
                        ack[0] ^= 1;
                    }
                    conn.write_all(&len).unwrap();
                    conn.write_all(&ack).unwrap();
                }
                listener
            });
            let result = session.inventory(&UsbSerialManager::new());
            if fault.is_empty() {
                assert_eq!(result.unwrap()[0]["resourceState"], "ready");
            } else {
                assert!(result.is_err());
            }
            assert_no_replay(server.join().unwrap());
        }
    }
}
