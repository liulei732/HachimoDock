//! Paired runtime events. Only the three fixed operations cross this channel.
use super::{
    component_events::pairing_revision,
    exchange_response, load_peer, require_no_usb,
    widget_data::{self, same_peer, Route},
    ComponentTarget, Peer, DEVICE_STATUS,
};
use crate::usb_serial::UsbSerialManager;
use serde_json::{json, Value};
use std::time::{Duration, Instant};

const MODE: u8 = 24;
const JSON_MAX: usize = 512;
const ENGINE: &str = "runtime-events-v1";
const MAX_REPLY: usize = JSON_MAX + 1;

#[derive(Clone)]
pub(crate) struct Scope {
    peer: Peer,
    pub(crate) route: Route,
    pairing: u64,
    association: u32,
}

fn descriptor(peer: &Peer) -> Option<ComponentTarget> {
    let bytes = exchange_response(peer, DEVICE_STATUS, &[2], Some(1024)).ok()?;
    let t: ComponentTarget = serde_json::from_slice(&bytes).ok()?;
    (widget_data::valid_identity(&t.board_device_id, &t.boot_id)
        && t.association != 0
        && t.capabilities["runtimeEvents"] == ENGINE)
        .then_some(t)
}

impl Scope {
    fn capture(peer: &Peer, usb: &UsbSerialManager) -> Option<Self> {
        let pairing = pairing_revision();
        require_no_usb(usb).ok()?;
        let t = descriptor(peer)?;
        let scope = Self {
            peer: peer.clone(),
            pairing,
            association: t.association,
            route: Route::Wifi {
                board: t.board_device_id,
                boot: t.boot_id,
            },
        };
        scope.local(usb).then_some(scope)
    }

    fn local(&self, usb: &UsbSerialManager) -> bool {
        require_no_usb(usb).is_ok()
            && self.pairing == pairing_revision()
            && load_peer().is_some_and(|p| same_peer(&self.peer, &p))
    }

    pub(crate) fn current(&self, usb: &UsbSerialManager) -> bool {
        self.local(usb)
            && descriptor(&self.peer).is_some_and(|t| {
                self.matches(&t.board_device_id, &t.boot_id) && t.association == self.association
            })
    }

    pub(crate) fn matches(&self, board: &str, boot: &str) -> bool {
        matches!(&self.route,Route::Wifi{board:b,boot:i} if b==board && i==boot)
    }

    fn exchange(
        &self,
        usb: &UsbSerialManager,
        op: u8,
        value: Option<&Value>,
    ) -> Result<Vec<u8>, String> {
        self.exchange_checked(op, value, || self.local(usb))
    }

    fn exchange_checked(
        &self,
        op: u8,
        value: Option<&Value>,
        current: impl Fn() -> bool,
    ) -> Result<Vec<u8>, String> {
        let Route::Wifi { board, boot } = &self.route else {
            return Err("无线目标无效".into());
        };
        let mut body = Vec::from(boot.as_bytes());
        body.extend_from_slice(board.as_bytes());
        body.extend_from_slice(&self.association.to_be_bytes());
        body.push(op);
        if let Some(value) = value {
            serde_json::to_writer(&mut body, value).map_err(|e| e.to_string())?;
        }
        if body.len() > 36 + JSON_MAX || !current() {
            return Err("运行时消息或连接无效".into());
        }
        let reply = exchange_response(
            &self.peer,
            MODE,
            &body,
            Some(if op == 0 { MAX_REPLY } else { 1 }),
        )
        .map_err(|_| "无线运行时事件回执未确认".to_string())?;
        if !current() {
            return Err("无线连接已变化".into());
        }
        Ok(reply)
    }

    pub(crate) fn publish(&self, usb: &UsbSerialManager, p: &Value) -> Result<(), String> {
        if !self.current(usb) {
            return Err("无线刷新目标已变化".into());
        }
        widget_data::publish_scoped(&self.route, Some(&self.peer), usb, p)
    }
}

pub(crate) fn notify(usb: &UsbSerialManager, p: &Value) -> Result<(), String> {
    if !crate::widget_notify::valid_payload(p) {
        return Err("通知负载无效".into());
    }
    let peer = load_peer().ok_or("设备未配对")?;
    let _lane = widget_data::foreground()?;
    let scope = Scope::capture(&peer, usb).ok_or("设备未声明无线运行时事件")?;
    (scope.exchange(usb, 1, Some(p))? == [1])
        .then_some(())
        .ok_or_else(|| "设备未接收通知".into())
}

pub(super) fn failed_press(
    usb: &UsbSerialManager,
    peer: &Peer,
    board: &str,
    boot: &str,
    seq: u32,
    owner: &[u8; 16],
) -> Result<(), String> {
    let _lane = widget_data::foreground()?;
    let scope = Scope::capture(peer, usb)
        .filter(|s| s.matches(board, boot))
        .ok_or("无线按键连接已变化")?;
    let owner = uuid::Uuid::from_bytes(*owner).simple().to_string();
    (scope.exchange(usb, 2, Some(&json!({"sequence":seq,"owner":owner})))? == [1])
        .then_some(())
        .ok_or_else(|| "该按键已过期或已提示".into())
}

fn event(bytes: &[u8]) -> Option<(u8, Value)> {
    let (&tag, body) = bytes.split_first()?;
    if body.len() > JSON_MAX {
        return None;
    }
    let p: Value = serde_json::from_slice(body).ok()?;
    match tag {
        2 if crate::widget_notify::valid_payload(&p) => Some((tag, p)),
        3 if p.as_object()?.len() == 2
            && crate::component_extension::valid_id(p["componentId"].as_str()?)
            && crate::component_app::refresh_source(p["source"].as_str()?) =>
        {
            Some((tag, p))
        }
        _ => None,
    }
}

pub(crate) fn start(app: tauri::AppHandle, usb: UsbSerialManager) {
    std::thread::spawn(move || {
        let mut scope: Option<Scope> = None;
        let mut next = Instant::now();
        loop {
            std::thread::sleep(Duration::from_millis(1800));
            let Some(_lane) = widget_data::background(&usb) else {
                continue;
            };
            let Some(peer) = load_peer() else {
                scope = None;
                next = Instant::now();
                continue;
            };
            if scope
                .as_ref()
                .is_some_and(|s| !same_peer(&s.peer, &peer) || s.pairing != pairing_revision())
            {
                scope = None;
                next = Instant::now();
            }
            if Instant::now() >= next {
                scope = Scope::capture(&peer, &usb);
                next = Instant::now() + Duration::from_secs(30);
            }
            let Some(target) = scope.as_ref() else {
                continue;
            };
            let Ok(reply) = target.exchange(&usb, 0, None) else {
                continue;
            };
            if reply == [0] {
                scope = None;
                next = Instant::now() + Duration::from_secs(2);
                continue;
            }
            let Some((tag, mut p)) = event(&reply) else {
                continue;
            };
            drop(_lane);
            if tag == 2 {
                crate::widget_notify::device_event(&app, &p);
            } else if let Route::Wifi { board, .. } = &target.route {
                p["boardDeviceId"] = board.clone().into();
                crate::component_app::wifi_data_refresh_event(&p, target.clone());
            }
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
    use std::io::{Read, Write};
    use std::net::TcpListener;

    #[test]
    fn paired_runtime_events_bind_identity_and_never_replay_a_lost_receipt() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let peer = Peer {
            address: listener.local_addr().unwrap(),
            key: zeroize::Zeroizing::new([0x32; 16]),
        };
        let board = "p4-0123456789ab";
        let boot = "000000000000000a";
        let scope = Scope {
            peer: peer.clone(),
            pairing: 1,
            association: 7,
            route: Route::Wifi {
                board: board.into(),
                boot: boot.into(),
            },
        };
        let notice = json!({"componentId":"demo","title":"Done"});
        let offer = notice.clone();
        let server = std::thread::spawn(move || {
            for step in 0..4 {
                let mut socket = accept(&listener);
                let mut cn = [0; 16];
                socket.read_exact(&mut cn).unwrap();
                let sn = [0x74; 16];
                socket.write_all(&sn).unwrap();
                let crypto = session_key(peer.key.as_ref(), &cn, &sn);
                let mut header = [0; 3];
                socket.read_exact(&mut header).unwrap();
                let n = u16::from_be_bytes([header[1], header[2]]) as usize;
                assert!(n <= 548);
                let mut data = vec![0; n + 16];
                socket.read_exact(&mut data).unwrap();
                let body = crypto
                    .open_in_place(nonce(0), aead::Aad::from(header), &mut data)
                    .unwrap();
                let mut reply = vec![];
                if step == 0 {
                    assert_eq!(header[0], DEVICE_STATUS);
                    assert_eq!(body, [2]);
                    reply=serde_json::to_vec(&json!({"boardDeviceId":board,"bootId":boot,"association":7,"capabilities":{"runtimeEvents":ENGINE}})).unwrap();
                } else {
                    assert_eq!(header[0], MODE);
                    assert_eq!(&body[..16], boot.as_bytes());
                    assert_eq!(&body[16..31], board.as_bytes());
                    assert_eq!(&body[31..35], &7u32.to_be_bytes());
                    assert_eq!(body[35], (step - 1) as u8);
                    match step {
                        1 => {
                            assert_eq!(body.len(), 36);
                            reply.push(2);
                            reply.extend(serde_json::to_vec(&offer).unwrap());
                        }
                        2 => {
                            assert_eq!(
                                serde_json::from_slice::<Value>(&body[36..]).unwrap(),
                                offer
                            );
                            reply.push(1);
                        }
                        _ => {
                            assert_eq!(
                                serde_json::from_slice::<Value>(&body[36..]).unwrap(),
                                json!({"sequence":8,"owner":"01000000000000000000000000000000"})
                            );
                            break;
                        }
                    }
                }
                let len = (reply.len() as u16).to_be_bytes();
                let aad = [header[0], header[1], header[2], len[0], len[1]];
                crypto
                    .seal_in_place_append_tag(nonce(1), aead::Aad::from(aad), &mut reply)
                    .unwrap();
                socket.write_all(&len).unwrap();
                socket.write_all(&reply).unwrap();
            }
            listener
        });
        assert_eq!(descriptor(&scope.peer).unwrap().association, 7);
        assert!(scope.exchange_checked(0, None, || false).is_err());
        assert_eq!(
            event(&scope.exchange_checked(0, None, || true).unwrap()),
            Some((2, notice.clone()))
        );
        assert_eq!(
            scope.exchange_checked(1, Some(&notice), || true).unwrap(),
            [1]
        );
        assert!(scope
            .exchange_checked(
                2,
                Some(&json!({"sequence":8,"owner":"01000000000000000000000000000000"})),
                || true
            )
            .is_err());
        assert_no_replay(server.join().unwrap());
    }

    #[test]
    fn runtime_events_only_accept_exact_direction_and_shape() {
        for (tag, p, ok) in [
            (2, json!({"title":"标题","componentId":"demo"}), true),
            (
                3,
                json!({"componentId":"demo","source":"computer.status"}),
                true,
            ),
            (
                3,
                json!({"componentId":"demo","source":"https://example.com"}),
                false,
            ),
            (
                3,
                json!({"componentId":"demo","source":"todos.upcoming","url":"x"}),
                false,
            ),
            (1, json!({"title":"x","componentId":"demo"}), false),
            (
                2,
                json!({"title":"x","componentId":"demo","command":"x"}),
                false,
            ),
        ] {
            let mut b = vec![tag];
            b.extend(serde_json::to_vec(&p).unwrap());
            assert_eq!(event(&b).is_some(), ok);
        }
        assert!(event(&[2; 514]).is_none());
        assert!(event(&[1]).is_none());
    }
}
