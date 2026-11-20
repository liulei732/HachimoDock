//! Component events and snapshots over the existing authenticated LAN link.
use super::{exchange_response, load_peer, query_status, LanDeviceStatus, Peer};
use crate::device_transport::{Route, APP_EVENT, APP_SNAPSHOT_MAX};
use crate::usb_serial::UsbSerialManager;
use serde::Deserialize;
use serde_json::Value;
use std::{
    io,
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};

const MODE: u8 = 22;
const HEADER: usize = 36;
const EVENT_MAX: usize = 3072;
const POLL_INTERVAL: Duration = Duration::from_millis(240);
const STATUS_INTERVAL: Duration = Duration::from_secs(5);
const SESSION_TIMEOUT: Duration = Duration::from_secs(3);

type Active = Option<(Arc<Session>, Instant)>;
static ACTIVE: Mutex<Active> = Mutex::new(None);

pub(crate) struct Session {
    peer: Peer,
    board: String,
    boot: String,
    owner: [u8; 16],
    outbox: Mutex<Option<Vec<u8>>>,
}

#[derive(Deserialize)]
struct Event {
    topic: String,
    payload: Value,
}

impl Session {
    fn new(peer: Peer, status: LanDeviceStatus) -> Option<Self> {
        if !status.control_trusted
            || !status.wifi_connected
            || status.rtc_key & 2 != 0
            || status.boot_id.len() != 16
            || !status.boot_id.bytes().all(|byte| byte.is_ascii_hexdigit())
            || status.board_device_id.len() != 15
        {
            return None;
        }
        Some(Self {
            peer,
            board: status.board_device_id,
            boot: status.boot_id,
            owner: *uuid::Uuid::new_v4().as_bytes(),
            outbox: Mutex::new(None),
        })
    }

    pub(crate) fn board(&self) -> &str {
        &self.board
    }

    pub(crate) fn queue_snapshot(&self, value: &Value) -> Result<(), String> {
        let bytes = serde_json::to_vec(value).map_err(|err| err.to_string())?;
        if bytes.len() > APP_SNAPSHOT_MAX {
            return Err("应用快照过大".into());
        }
        let mut outbox = self.outbox.lock().map_err(|_| "Wi-Fi 组件队列不可用")?;
        *outbox = Some(bytes);
        Ok(())
    }

    fn exchange(&self, ack: u32) -> io::Result<Option<(u32, Option<Event>)>> {
        let snapshot = self
            .outbox
            .lock()
            .ok()
            .and_then(|mut pending| pending.take());
        let mut body = Vec::with_capacity(HEADER + snapshot.as_ref().map_or(0, Vec::len));
        body.extend_from_slice(self.boot.as_bytes());
        body.extend_from_slice(&self.owner);
        body.extend_from_slice(&ack.to_be_bytes());
        if let Some(snapshot) = snapshot {
            body.extend_from_slice(&snapshot);
        }
        let reply = exchange_response(&self.peer, MODE, &body, Some(5 + EVENT_MAX))?;
        decode(&reply)
    }
}

fn decode(reply: &[u8]) -> io::Result<Option<(u32, Option<Event>)>> {
    let invalid = || io::Error::new(io::ErrorKind::InvalidData, "invalid LAN component offer");
    if reply == [0] {
        return Ok(None);
    }
    let [1, a, b, c, d, body @ ..] = reply else {
        return Err(invalid());
    };
    let id = u32::from_be_bytes([*a, *b, *c, *d]);
    if id == 0 {
        return if body.is_empty() {
            Ok(Some((0, None)))
        } else {
            Err(invalid())
        };
    }
    // A malformed event is discarded, with the same ACK as any filtered
    // event, so it cannot hold newer messages in the device mailbox.
    let event = serde_json::from_slice::<Event>(body)
        .ok()
        .filter(|event| event.topic == APP_EVENT);
    Ok(Some((id, event)))
}

pub(crate) fn active() -> Option<Arc<Session>> {
    ACTIVE
        .lock()
        .ok()?
        .as_ref()
        .filter(|(_, until)| Instant::now() < *until)
        .map(|(session, _)| session.clone())
}

fn set_active(value: Active) {
    if let Ok(mut active) = ACTIVE.lock() {
        *active = value;
    }
}

fn same_peer(left: &Peer, right: &Peer) -> bool {
    left.address == right.address && left.key.as_ref() == right.key.as_ref()
}

pub(super) fn start(usb: UsbSerialManager) {
    std::thread::spawn(move || {
        let mut session: Option<Arc<Session>> = None;
        let mut next_status = Instant::now();
        let mut ack = 0;
        loop {
            std::thread::sleep(POLL_INTERVAL);
            let peer = if usb.status().connected {
                None
            } else {
                load_peer()
            };
            let Some(peer) = peer else {
                if session.take().is_some() {
                    set_active(None);
                }
                next_status = Instant::now();
                ack = 0;
                continue;
            };
            if session
                .as_ref()
                .is_some_and(|current| !same_peer(&current.peer, &peer))
            {
                set_active(None);
                session = None;
                next_status = Instant::now();
                ack = 0;
            }
            if Instant::now() >= next_status {
                next_status = Instant::now() + STATUS_INTERVAL;
                if let Ok(status) = query_status(&peer) {
                    match Session::new(peer, status) {
                        Some(new)
                            if session.as_ref().is_some_and(|old| {
                                old.boot == new.boot && old.board == new.board
                            }) => {}
                        new => {
                            set_active(None);
                            session = new.map(Arc::new);
                            ack = 0;
                        }
                    }
                }
            }
            let Some(current) = session.as_ref() else {
                continue;
            };
            let began = Instant::now();
            match current.exchange(ack) {
                Ok(Some((id, event))) if !usb.status().connected => {
                    set_active(Some((current.clone(), began + SESSION_TIMEOUT)));
                    if id > ack {
                        if let Some(event) = event {
                            crate::component_app::device_message(
                                Route::Wifi(current.clone()),
                                &event.topic,
                                &event.payload,
                            );
                        }
                        ack = id;
                    }
                }
                Ok(_) => {
                    set_active(None);
                    session = None;
                    next_status = Instant::now();
                    ack = 0;
                }
                Err(_) if active().is_none() => {
                    // Retire outstanding work after the lease, even if the
                    // old address is still stored for normal LAN rediscovery.
                    set_active(None);
                    session = None;
                    next_status = Instant::now() + STATUS_INTERVAL;
                    ack = 0;
                }
                Err(_) => {}
            }
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn authenticated_app_exchange_carries_event_ack_and_latest_snapshot() {
        use super::super::{key_from_digest, nonce, session_digest};
        use ring::aead;
        use serde_json::json;
        use std::{
            io::{Read, Write},
            net::TcpListener,
        };

        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let session = Session {
            peer: Peer {
                address: listener.local_addr().unwrap(),
                key: zeroize::Zeroizing::new([42; 16]),
            },
            board: "p4-0123456789ab".into(),
            boot: "0123456789abcdef".into(),
            owner: [7; 16],
            outbox: Mutex::new(None),
        };
        let fixture = std::thread::spawn(move || {
            for ack in [0u32, 1] {
                let (mut stream, _) = listener.accept().unwrap();
                stream
                    .set_read_timeout(Some(Duration::from_secs(2)))
                    .unwrap();
                let mut client = [0; 16];
                stream.read_exact(&mut client).unwrap();
                let server = [9; 16];
                stream.write_all(&server).unwrap();
                let key = key_from_digest(&session_digest(&[42; 16], &client, &server));
                let mut header = [0; 3];
                stream.read_exact(&mut header).unwrap();
                assert_eq!(header[0], MODE);
                let size = u16::from_be_bytes([header[1], header[2]]) as usize;
                let mut encrypted = vec![0; size + 16];
                stream.read_exact(&mut encrypted).unwrap();
                let body = key
                    .open_in_place(nonce(0), aead::Aad::from(header), &mut encrypted)
                    .unwrap();
                assert_eq!(&body[..16], b"0123456789abcdef");
                assert_eq!(&body[16..32], &[7; 16]);
                assert_eq!(&body[32..HEADER], &ack.to_be_bytes());
                let mut reply = vec![1, 0, 0, 0, 0];
                if ack == 0 {
                    assert_eq!(body.len(), HEADER);
                    reply[4] = 1;
                    reply.extend_from_slice(br#"{"topic":"app/event","payload":{"sequence":2}}"#);
                } else {
                    let snapshot: Value = serde_json::from_slice(&body[HEADER..]).unwrap();
                    assert_eq!(snapshot["sequence"], 2);
                }
                let length = (reply.len() as u16).to_be_bytes();
                let aad = [header[0], header[1], header[2], length[0], length[1]];
                key.seal_in_place_append_tag(nonce(1), aead::Aad::from(aad), &mut reply)
                    .unwrap();
                stream.write_all(&length).unwrap();
                stream.write_all(&reply).unwrap();
            }
        });
        let Some((id, Some(_))) = session.exchange(0).unwrap() else {
            panic!("missing offer");
        };
        session.queue_snapshot(&json!({"sequence":1})).unwrap();
        session.queue_snapshot(&json!({"sequence":2})).unwrap();
        assert!(matches!(session.exchange(id).unwrap(), Some((0, None))));
        fixture.join().unwrap();
    }

    #[test]
    fn decodes_mailbox_refusal_empty_and_exact_app_event() {
        assert!(decode(&[0]).unwrap().is_none());
        assert!(matches!(decode(&[1, 0, 0, 0, 0]).unwrap(), Some((0, None))));
        assert!(decode(&[1, 0, 0, 0, 0, 1]).is_err());
        let mut reply = vec![1, 0, 0, 0, 7];
        reply.extend_from_slice(br#"{"topic":"app/event","payload":{"sequence":2}}"#);
        let Some((7, Some(event))) = decode(&reply).unwrap() else {
            panic!("missing offer");
        };
        assert_eq!(event.payload["sequence"], 2);
        let mut unauthorized = vec![1, 0, 0, 0, 8];
        unauthorized.extend_from_slice(br#"{"topic":"audio/begin","payload":{}}"#);
        assert!(matches!(decode(&unauthorized).unwrap(), Some((8, None))));
    }
}
