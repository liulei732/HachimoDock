//! Application events share the paired device identity used for widget data.
//! Mode 22 only accepts poll, validate, snapshot and presentation image frames.
use super::widget_data::{foreground, same_peer, Route, Target};
use super::{exchange_response, require_no_usb, Peer};
use crate::usb_serial::UsbSerialManager;
use serde_json::{json, Value};
use std::{
    sync::atomic::{AtomicU64, Ordering},
    thread,
    time::{Duration, Instant},
};

static ACTIVITY: (std::sync::Mutex<u64>, std::sync::Condvar) =
    (std::sync::Mutex::new(0), std::sync::Condvar::new());

pub(super) fn foreground_observed() {
    let mut generation = ACTIVITY.0.lock().unwrap();
    *generation = generation.wrapping_add(1);
    ACTIVITY.1.notify_one();
}

fn wait_for_activity(observed: u64, duration: Duration) {
    let guard = ACTIVITY.0.lock().unwrap();
    drop(ACTIVITY.1.wait_timeout_while(guard, duration, |g| *g == observed).unwrap());
}

static PAIR_REVISION: AtomicU64 = AtomicU64::new(1);
pub(super) fn pairing_revision() -> u64 {
    PAIR_REVISION.load(Ordering::Acquire)
}
pub(super) fn pairing_changed() {
    PAIR_REVISION.fetch_add(1, Ordering::AcqRel);
}
const MODE: u8 = 22;
const IMAGE_MAX: usize = 3129; // 2732 Base64 bytes + 397 bytes of maximum-length reference metadata.
const MAX: usize = 2048;
const POLL_HEADER: usize = 5;
const POLL: u8 = 0;
const VALIDATE: u8 = 1;
const SNAPSHOT: u8 = 2;
const IMAGE: u8 = 3;

#[derive(Clone)]
pub(crate) struct EventTarget {
    pub route: Route,
    pub bridge: bool,
    wifi: Option<Session>,
}

#[derive(Clone)]
struct Session {
    peer: Peer,
    pairing: u64,
    client: [u8; 16],
    token: u32,
}

fn frame(
    board: &str,
    boot: &str,
    client: &[u8; 16],
    token: u32,
    op: u8,
    data: Option<&Value>,
) -> Result<Vec<u8>, String> {
    let mut body = Vec::with_capacity(128);
    body.extend_from_slice(boot.as_bytes());
    body.extend_from_slice(board.as_bytes());
    body.extend_from_slice(client);
    body.extend_from_slice(&token.to_be_bytes());
    body.push(op);
    if let Some(v) = data {
        serde_json::to_writer(&mut body, v).map_err(|e| e.to_string())?;
    }
    if body.len() > 52 + if op == IMAGE { IMAGE_MAX } else { MAX } {
        return Err("组件消息过大".into());
    }
    Ok(body)
}

fn parse_poll(bytes: &[u8]) -> Option<(bool, u32, Option<Value>)> {
    if !(POLL_HEADER..=POLL_HEADER + MAX).contains(&bytes.len()) || !matches!(bytes[0], 1 | 3) {
        return None;
    }
    let token = u32::from_be_bytes(bytes[1..5].try_into().ok()?);
    if token == 0 {
        return None;
    }
    let event = if bytes.len() == POLL_HEADER {
        None
    } else {
        let event: Value = serde_json::from_slice(&bytes[POLL_HEADER..]).ok()?;
        if !event.is_object() {
            return None;
        }
        Some(event)
    };
    Some((bytes[0] == 3, token, event))
}

impl EventTarget {
    pub fn usb(usb: &UsbSerialManager) -> Option<Self> {
        let s = usb.status();
        let connection = s.connection_id.parse().ok()?;
        (s.connected && connection != 0 && !s.board_device_id.is_empty()).then(|| Self {
            route: Route::Usb {
                board: s.board_device_id,
                connection,
            },
            bridge: s.capabilities["widgetServiceBridge"]
                == crate::service_bridge::surface::DEVICE_ENGINE,
            wifi: None,
        })
    }

    pub(super) fn av_peer(&self) -> Option<Peer> { self.wifi.as_ref().map(|s|s.peer.clone()) }

    pub fn board(&self) -> &str {
        match &self.route {
            Route::Usb { board, .. } | Route::Wifi { board, .. } => board,
        }
    }

    pub fn same(&self, other: &Self) -> bool {
        self.route == other.route
            && match (&self.wifi, &other.wifi) {
                (Some(a), Some(b)) => {
                    a.pairing == b.pairing
                        && a.token == b.token
                        && a.client == b.client
                        && same_peer(&a.peer, &b.peer)
                }
                (None, None) => true,
                _ => false,
            }
    }

    pub fn local(&self, usb: &UsbSerialManager) -> bool {
        match &self.wifi {
            None => Self::usb(usb).is_some_and(|t| self.same(&t)),
            Some(session) => {
                !usb.status().connected && session.pairing == PAIR_REVISION.load(Ordering::Acquire)
            }
        }
    }

    fn exchange(&self, usb: &UsbSerialManager, op: u8, payload: &Value) -> Result<u8, String> {
        let Route::Wifi { board, boot } = &self.route else {
            return Err("无线组件未连接".into());
        };
        let session = self.wifi.as_ref().ok_or("无线组件未连接")?;
        let body = frame(
            board,
            boot,
            &session.client,
            session.token,
            op,
            Some(payload),
        )?;
        let _lane = foreground()?;
        if !self.local(usb) {
            return Err("组件连接已变化".into());
        }
        match exchange_response(&session.peer, MODE, &body, Some(1)).as_deref() {
            Ok([n @ (1 | 2)]) => Ok(*n),
            _ => Err("无线组件请求未获确认".into()),
        }
    }

    pub fn validate(&self, usb: &UsbSerialManager, event: &Value) -> bool {
        // The authenticated poll checked the view. Read-only heartbeats need
        // only local ownership; a queued press must still be pending on device.
        if self.wifi.is_some() && event["action"] != "status" {
            let identity = json!({"componentId":event["componentId"],"instance":event["instance"],
                "sequence":event["sequence"],"action":event["action"],"visible":true});
            self.exchange(usb, VALIDATE, &identity) == Ok(1)
        } else {
            self.local(usb)
        }
    }

    pub fn snapshot(
        &self,
        usb: &UsbSerialManager,
        payload: &Value,
        try_only: bool,
    ) -> Result<(), String> {
        if self.wifi.is_some() {
            return self.exchange(usb, SNAPSHOT, payload).and_then(|n| {
                if n == 1 {
                    Ok(())
                } else {
                    Err("组件快照无效".into())
                }
            });
        }
        let Route::Usb { board, connection } = &self.route else {
            return Err("USB 连接已变化".into());
        };
        usb.app_snapshot_scoped(board, Some(*connection), payload, try_only)
    }

    pub fn publish_data(&self, usb: &UsbSerialManager, data: &Value) -> Result<(), String> {
        if !self.local(usb) {
            return Err("组件连接已变化".into());
        }
        super::widget_data::publish_scoped(
            &self.route,
            self.wifi.as_ref().map(|s| &s.peer),
            usb,
            data,
        )
    }

    pub fn image_chunk_bytes(&self) -> usize {
        if self.wifi.is_some() {
            2048
        } else {
            1024
        }
    }

    pub fn image(&self, usb: &UsbSerialManager, payload: Value) -> Result<Value, String> {
        if !self.bridge {
            return Err("设备未声明服务桥图片能力".into());
        }
        if self.wifi.is_some() {
            let ready = self.exchange(usb, IMAGE, &payload)? == 2;
            return Ok(json!({"code": if ready {"ready"} else {"ok"}}));
        }
        let Route::Usb { board, connection } = &self.route else {
            return Err("USB 连接已变化".into());
        };
        usb.service_image_request(board, *connection, payload)
    }
}

fn poll(
    route: &Route,
    peer: &Peer,
    usb: &UsbSerialManager,
    client: [u8; 16],
    pairing: u64,
) -> Option<(EventTarget, Option<Value>)> {
    require_no_usb(usb).ok()?;
    if pairing != PAIR_REVISION.load(Ordering::Acquire) {
        return None;
    }
    let Route::Wifi { board, boot } = route else {
        return None;
    };
    let body = frame(board, boot, &client, 0, POLL, None).ok()?;
    let _lane = foreground().ok()?;
    require_no_usb(usb).ok()?;
    let bytes = exchange_response(peer, MODE, &body, Some(POLL_HEADER + MAX)).ok()?;
    let (bridge, token, event) = parse_poll(&bytes)?;
    let target = EventTarget {
        route: route.clone(),
        bridge,
        wifi: Some(Session {
            pairing,
            peer: peer.clone(),
            client,
            token,
        }),
    };
    target.local(usb).then_some((target, event))
}

// An authenticated foreground hint interrupts the idle wait. Preserve the
// original discovery cadence when the board is quiet or does not send hints.
fn poll_window(
    flags: u8,
    mut poll: impl FnMut() -> bool,
    mut wait: impl FnMut(Duration),
    within: impl Fn() -> bool,
) {
    if flags == 1 {
        wait(Duration::from_secs(4));
        return;
    }
    if flags != 3 {
        wait(Duration::from_secs(15));
        return;
    }
    while within() {
        if !poll() {
            wait(Duration::from_millis(750));
            break;
        }
        wait(Duration::from_millis(350));
    }
}

pub(crate) fn start(usb: UsbSerialManager) {
    thread::Builder::new()
        .name("component-lan-events".into())
        .spawn(move || {
            let mut client = [0; 16];
            let mut last_pairing = 0;
            loop {
                let observed = *ACTIVITY.0.lock().unwrap();
                let pairing = PAIR_REVISION.load(Ordering::Acquire);
                if last_pairing != pairing {
                    client = *uuid::Uuid::new_v4().as_bytes();
                    last_pairing = pairing;
                }
                let Some(target) = Target::current(&usb) else {
                    wait_for_activity(observed, Duration::from_secs(2));
                    continue;
                };
                let Some(peer) = target.paired_peer() else {
                    thread::sleep(Duration::from_secs(2));
                    continue;
                };
                let until = Instant::now() + Duration::from_secs(4);
                poll_window(
                    target.host_flags,
                    || {
                        let Some((route, event)) = poll(&target.route, peer, &usb, client, pairing)
                        else {
                            return false;
                        };
                        crate::component_app::poll_event(route, event.as_ref());
                        true
                    },
                    |duration| {
                        if target.host_flags == 1 {
                            wait_for_activity(observed, duration);
                        } else {
                            thread::sleep(duration);
                        }
                    },
                    || Instant::now() < until,
                );
            }
        })
        .expect("component LAN worker");
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn foreground_hint_wakes_first_poll_even_if_it_arrived_before_wait() {
        let observed = *ACTIVITY.0.lock().unwrap();
        foreground_observed();
        let at = Instant::now();
        poll_window(1, || panic!("idle peer polled"), |d| wait_for_activity(observed, d), || true);
        assert!(at.elapsed() < Duration::from_millis(500));
    }
    #[test]
    fn old_idle_and_active_peer_polling() {
        use std::cell::Cell;
        let base = json!({"firmware":"v1","bootId":"0123456789abcdef","wifiConnected":true,
            "clockSynchronized":true,"httpsSuccesses":0,"httpsFailures":0});
        let legacy = serde_json::from_value::<super::super::LanDeviceStatus>(base).unwrap();
        assert_eq!(legacy.component_host_flags, 0);
        for (flags, expected, delay) in [(0, 0, 15000), (1, 0, 4000), (3, 4, 350), (4, 0, 15000)] {
            let calls = Cell::new(0);
            let mut waits = Vec::new();
            poll_window(
                flags,
                || {
                    calls.set(calls.get() + 1);
                    true
                },
                |d| waits.push(d.as_millis()),
                || calls.get() < 4,
            );
            assert_eq!(calls.get(), expected);
            assert!(waits.iter().all(|v| *v == delay));
        }
    }
    #[test]
    fn authenticated_component_poll_snapshot_and_image_rejects_damaged_ack() {
        use super::super::{nonce, session_key};
        use ring::aead;
        use std::{
            io::{Read, Write},
            net::TcpListener,
        };
        let socket = TcpListener::bind("127.0.0.1:0").unwrap();
        let peer = Peer {
            address: socket.local_addr().unwrap(),
            key: zeroize::Zeroizing::new([42; 16]),
        };
        let board = "p4-001122334455";
        let boot = "0123456789abcdef";
        let client = [7; 16];
        let event = json!({"componentId":"counter","boardDeviceId":board,"instance":"abc-def",
            "sequence":1,"visible":true,"action":"host.next"});
        let expected = event.clone();
        let worker = thread::spawn(move || {
            for op in [POLL, VALIDATE, SNAPSHOT, IMAGE, IMAGE] {
                let mut stream = crate::lan_agent::confirmation_tests::accept(&socket);
                let mut challenge = [0; 16];
                stream.read_exact(&mut challenge).unwrap();
                stream.write_all(&[9; 16]).unwrap();
                let key = session_key(&[42; 16], &challenge, &[9; 16]);
                let mut header = [0; 3];
                stream.read_exact(&mut header).unwrap();
                assert_eq!(header[0], MODE);
                let n = u16::from_be_bytes([header[1], header[2]]) as usize;
                let mut body = vec![0; n + 16];
                stream.read_exact(&mut body).unwrap();
                let plain = key
                    .open_in_place(nonce(0), aead::Aad::from(header), &mut body)
                    .unwrap();
                assert_eq!(plain[51], op);
                assert_eq!(&plain[31..47], &client);
                assert_eq!(&plain[..31], format!("{boot}{board}").as_bytes());
                assert_eq!(
                    &plain[47..51],
                    &(if op == POLL { 0u32 } else { 13 }).to_be_bytes()
                );
                let mut ack = vec![1];
                let mut damage = false;
                if op == POLL {
                    ack = vec![3, 0, 0, 0, 13];
                    ack.extend(serde_json::to_vec(&expected).unwrap());
                } else {
                    let payload: Value = serde_json::from_slice(&plain[52..]).unwrap();
                    assert_eq!(payload["instance"], "abc-def");
                    if op == IMAGE {
                        ack[0] = 2;
                        damage = payload["damage"] == true;
                    }
                }
                let size = (ack.len() as u16).to_be_bytes();
                let aad = [header[0], header[1], header[2], size[0], size[1]];
                key.seal_in_place_append_tag(nonce(1), aead::Aad::from(aad), &mut ack)
                    .unwrap();
                if damage {
                    *ack.last_mut().unwrap() ^= 1;
                }
                stream.write_all(&size).unwrap();
                stream.write_all(&ack).unwrap();
            }
        });
        let route = Route::Wifi {
            board: board.into(),
            boot: boot.into(),
        };
        let usb = UsbSerialManager::new();
        let (target, actual) = poll(
            &route,
            &peer,
            &usb,
            client,
            PAIR_REVISION.load(Ordering::Acquire),
        )
        .unwrap();
        assert_eq!(actual.as_ref(), Some(&event));
        assert!(target.validate(&usb, &event));
        assert!(target.validate(&usb, &json!({"action":"status"})));
        let snapshot =
            json!({"componentId":"counter","instance":"abc-def","sequence":1,"values":{}});
        target.snapshot(&usb, &snapshot, false).unwrap();
        let mut image = json!({"operation":"query","instance":"abc-def"});
        assert_eq!(target.image(&usb, image.clone()).unwrap()["code"], "ready");
        image["damage"] = true.into();
        assert!(target.image(&usb, image).is_err());
        worker.join().unwrap();
    }

    #[test]
    fn paired_component_frames_bound_identity_operation_and_payload() {
        let b = "p4-001122334455";
        let boot = "0123456789abcdef";
        let c = [7; 16];
        let data = json!({"componentId":"counter","instance":"a-b","sequence":3});
        let body = frame(b, boot, &c, 0x01234567, SNAPSHOT, Some(&data)).unwrap();
        assert_eq!(&body[..31], format!("{boot}{b}").as_bytes());
        assert_eq!(&body[31..47], &c);
        assert_eq!(&body[47..52], &[1, 35, 69, 103, 2]);
        assert_eq!(serde_json::from_slice::<Value>(&body[52..]).unwrap(), data);
        assert!(frame(b, boot, &c, 1, SNAPSHOT, Some(&json!("a".repeat(MAX - 2)))).is_ok());
        assert!(frame(b, boot, &c, 1, SNAPSHOT, Some(&json!("a".repeat(MAX - 1)))).is_err());
        let image = json!({"componentId":"c".repeat(47),"instance":"a".repeat(31),
            "alias":"a".repeat(15),"key":"k".repeat(47),"revision":"f".repeat(64),"context":"f".repeat(64),
            "offset":71680,"width":192,"height":192,"data":"A".repeat(2732)});
        assert_eq!(
            frame(b, boot, &c, 1, IMAGE, Some(&image)).unwrap().len(),
            52 + IMAGE_MAX
        );
        assert!(frame(b, boot, &c, 1, IMAGE, Some(&json!("a".repeat(IMAGE_MAX)))).is_err());
        let reply = [3, 0, 0, 0, 9];
        assert_eq!(parse_poll(&reply), Some((true, 9, None)));
        assert!(parse_poll(&[3, 0, 0, 0, 0]).is_none());
        assert!(parse_poll(&[5, 0, 0, 0, 9]).is_none());
        let mut record = reply.to_vec();
        record.extend(serde_json::to_vec(&data).unwrap());
        assert_eq!(parse_poll(&record), Some((true, 9, Some(data))));
        record.resize(MAX + 6, b' ');
        assert!(parse_poll(&record).is_none());
    }
}
