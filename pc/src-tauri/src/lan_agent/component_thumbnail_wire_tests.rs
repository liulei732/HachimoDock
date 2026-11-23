//! Actual status and mode-14 clients over loopback, with a paired fixture receiver.
use super::*;
use crate::lan_agent::{confirmation_tests::accept, nonce, session_key, DEVICE_STATUS};
use crate::usb_serial::center_thumb;
use std::{
    collections::BTreeMap,
    io::Read,
    net::TcpListener,
    sync::atomic::{AtomicU64, Ordering},
    sync::Arc,
};
use zeroize::Zeroizing;

const TOKEN: [u8; 16] = [0x58; 16];
const BOOT: &str = "ca1100fe00000001";
const BOARD: &str = "p4-010203040506";

fn hello(
    listener: &TcpListener,
    mode: u8,
    body: &[u8],
    reply: &[u8],
) -> (TcpStream, aead::LessSafeKey) {
    let mut socket = accept(listener);
    socket.set_nodelay(true).unwrap();
    let mut client = [0; 16];
    socket.read_exact(&mut client).unwrap();
    let server = [mode; 16];
    socket.write_all(&server).unwrap();
    let key = session_key(&TOKEN, &client, &server);
    let mut header = [0; 3];
    socket.read_exact(&mut header).unwrap();
    assert_eq!(header, [mode, 0, body.len() as u8]);
    let mut incoming = vec![0; body.len() + 16];
    socket.read_exact(&mut incoming).unwrap();
    assert_eq!(
        key.open_in_place(nonce(0), aead::Aad::from(header), &mut incoming)
            .unwrap(),
        body
    );
    let length = (reply.len() as u16).to_be_bytes();
    let mut ack = reply.to_vec();
    let aad = [mode, header[1], header[2], length[0], length[1]];
    key.seal_in_place_append_tag(nonce(1), aead::Aad::from(aad), &mut ack)
        .unwrap();
    socket.write_all(&length).unwrap();
    socket.write_all(&ack).unwrap();
    (socket, key)
}

fn status_reply(listener: &TcpListener, thumbnail: bool) {
    let status = serde_json::json!({"firmware":"fixture","boardDeviceId":BOARD,"bootId":BOOT,
        "wifiConnected":true,"clockSynchronized":true,"httpsSuccesses":0,"httpsFailures":0,
        "controlTrusted":true,"componentDelivery":true,"componentBundle":true,"ops":true,
        "rtcKey":if thumbnail {32} else {0}});
    hello(listener, DEVICE_STATUS, &[1], status.to_string().as_bytes());
}

type Files = BTreeMap<String, Vec<u8>>;

// Parse the records on the receiving socket independently of the sender's builders.
// Cap every socket read at 512 bytes, including the 1,568-byte thumbnail.
fn receive(
    listener: &TcpListener,
    target: &[u8],
    opcode: u8,
    id: &str,
    stop: Option<&AtomicU64>,
) -> Option<Files> {
    let mode = if opcode == 7 { super::super::maintenance::MODE } else { MODE };
    let (mut socket, key) = hello(listener, mode, target, &[1]);
    let mut files = Files::new();
    let mut digest = [0; 32];
    let mut file_count = 0;
    let mut total = 0;
    let mut active_path = String::new();
    let mut expected_offset = 0;
    let mut expected_index = 0;
    for sequence in 0..MAX_RECORDS {
        let mut length = [0; 2];
        socket.read_exact(&mut length).unwrap();
        let size = u16::from_be_bytes(length) as usize;
        assert!((1..=4096 + 107).contains(&size));
        let mut encrypted = vec![0; size + 16];
        for part in encrypted.chunks_mut(512) {
            socket.read_exact(part).unwrap();
        }
        let record = key
            .open_in_place(
                record_nonce(2, sequence),
                aead::Aad::from(length),
                &mut encrypted,
            )
            .unwrap();
        if sequence == 0 {
            assert_eq!(record[0], opcode);
            assert_eq!(record[1] as usize, id.len());
            assert_eq!(&record[39..39 + id.len()], id.as_bytes());
            let expected_version: &[u8] = if opcode == 4 { b"1.0.2" } else { b"" };
            assert_eq!(&record[39 + id.len()..], expected_version);
            file_count = record[2] as usize;
            total = u32::from_be_bytes(record[3..7].try_into().unwrap()) as usize;
            digest.copy_from_slice(&record[7..39]);
            if opcode == 6 {
                assert_eq!((file_count, total), (2, 1600));
            }
        } else if record == [3] {
            assert_eq!(expected_offset, 0);
            assert_eq!(files.len(), file_count);
            assert_eq!(files.values().map(Vec::len).sum::<usize>(), total);
            let mut hash = Sha256::new();
            for (name, bytes) in &files {
                hash.update(name.as_bytes());
                hash.update([0]);
                hash.update((bytes.len() as u32).to_le_bytes());
                hash.update(bytes);
            }
            assert_eq!(hash.finalize()[..], digest);
        } else {
            assert_eq!(record[0], 2);
            assert_eq!(record[1] as usize, expected_index);
            let offset = u32::from_be_bytes(record[2..6].try_into().unwrap()) as usize;
            let file_bytes = u32::from_be_bytes(record[6..10].try_into().unwrap()) as usize;
            let end = 11 + record[10] as usize;
            let path = std::str::from_utf8(&record[11..end]).unwrap();
            let bytes = &record[end..];
            assert_eq!(offset, expected_offset);
            assert_eq!(bytes.len(), (file_bytes - offset).min(4096));
            if offset == 0 {
                assert!(path > active_path.as_str());
                active_path = path.to_owned();
            } else {
                assert_eq!(path, active_path);
            }
            files
                .entry(path.to_owned())
                .or_default()
                .extend_from_slice(bytes);
            expected_offset += bytes.len();
            if expected_offset == file_bytes {
                expected_index += 1;
                expected_offset = 0;
            }
            if path == center_thumb::PATH {
                assert_eq!((offset, bytes.len(), file_bytes), (0, 1568, 1568));
                if let Some(revision) = stop {
                    revision.fetch_add(1, Ordering::Release);
                }
            }
        }
        let mut ack = vec![1];
        key.seal_in_place_append_tag(record_nonce(3, sequence), aead::Aad::from([0, 1]), &mut ack)
            .unwrap();
        socket.write_all(&[0, 1]).unwrap();
        socket.write_all(&ack).unwrap();
        if stop.is_some_and(|rev| rev.load(Ordering::Acquire) != 1) {
            assert_eq!(
                socket.read(&mut [0]).unwrap(),
                0,
                "must close before commit"
            );
            return None;
        }
        if record == [3] {
            return Some(files);
        }
    }
    panic!("transaction did not terminate")
}

#[test]
fn center_thumb_authenticated_install_repair_legacy_and_pairing_change() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../builtin-clawpkgs/wasm-2046");
    let pkg = Package::read(&root).unwrap();
    let id = pkg.preview.id.as_str();
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let peer = Peer {
        address: listener.local_addr().unwrap(),
        key: Zeroizing::new(TOKEN),
    };
    let revision = Arc::new(AtomicU64::new(1));
    let revoked = revision.clone();
    let worker = std::thread::spawn(move || {
        status_reply(&listener, true);
        let mut installed = receive(&listener, BOOT.as_bytes(), 1, "wasm-2046", None).unwrap();
        assert!(installed["runtime/app.wasm"].len() > 4096);
        assert!(installed.contains_key("runtime/center-title.rle"));
        let cover = installed.remove(center_thumb::PATH).unwrap();
        let prior = installed.clone();
        let repair = receive(&listener, BOOT.as_bytes(), 6, "wasm-2046", None).unwrap();
        assert_eq!(
            repair.keys().map(String::as_str).collect::<Vec<_>>(),
            [center_thumb::MATCH, center_thumb::PATH]
        );
        assert_eq!(
            repair[center_thumb::MATCH],
            Sha256::digest(&installed["runtime/widget.json"])[..]
        );
        assert_eq!(repair[center_thumb::PATH], cover);
        assert_eq!(installed, prior);
        assert!(receive(&listener, BOOT.as_bytes(), 6, "wasm-2046", Some(&revoked)).is_none());
        status_reply(&listener, false);
        let old = receive(&listener, BOOT.as_bytes(), 1, "wasm-2046", None).unwrap();
        assert_eq!(old, prior);
        listener
    });
    let binding = ComponentPairing::bind(&peer, Some(&peer), 1).unwrap();
    let current = || binding.current(revision.load(Ordering::Acquire));
    let status = query_status(&peer).unwrap();
    require_thumbnail_target(&status, BOARD, BOOT).unwrap();
    assert!(require_thumbnail_target(&status, BOARD, "different").is_err());
    assert!(require_thumbnail_target(&status, "different", BOOT).is_err());
    let title: ComponentCenterTitle =
        serde_json::from_value(serde_json::json!({"text":"2046","data":"AQIDBA=="})).unwrap();
    let capabilities = serde_json::json!({"widgetWasm":"hachi-wasm-v1","widgetWasmCanvas":"640x480-v1","widgetWasmHost":"hachi-host-v1","widgetCenterTitle":"rle4-v1","widgetWasmMaxBytes":196608,"widgetWasmMemoryPages":4});
    let full = pkg
        .prepare(
            &HashMap::new(),
            Some(&title),
            status.component_bundle && status.center_thumb,
            Some(&capabilities),
        )
        .unwrap();
    install_bundle(&peer, BOOT.as_bytes(), id, &full, None, CommitReply::Legacy, current).unwrap();
    let repair = center_thumb::repair_files(&pkg.stage.path().join("package"), &root, id).unwrap();
    install_bundle(&peer, BOOT.as_bytes(), id, &repair, None, CommitReply::Legacy, current).unwrap();
    assert!(install_bundle(&peer, BOOT.as_bytes(), id, &repair, None, CommitReply::Legacy, current)
        .unwrap_err()
        .contains("配对已变化"));
    let old_status = query_status(&peer).unwrap();
    assert!(require_thumbnail_target(&old_status, BOARD, BOOT).is_err());
    let old = pkg
        .prepare(
            &HashMap::new(),
            Some(&title),
            old_status.component_bundle && old_status.center_thumb,
            Some(&capabilities),
        )
        .unwrap();
    let next = ComponentPairing::bind(&peer, Some(&peer), 2).unwrap();
    install_bundle(&peer, BOOT.as_bytes(), id, &old, None, CommitReply::Legacy, || {
        next.current(revision.load(Ordering::Acquire))
    })
    .unwrap();
    for changed in [
        Peer {
            key: Zeroizing::new([0x95; 16]),
            ..peer.clone()
        },
        Peer {
            address: "127.0.0.1:1".parse().unwrap(),
            ..peer.clone()
        },
    ] {
        assert!(ComponentPairing::bind(&peer, Some(&changed), 2).is_err());
    }
    assert!(ComponentPairing::bind(&peer, None, 2).is_err());
    assert_eq!(peer.key.as_ref(), TOKEN);
    crate::lan_agent::confirmation_tests::assert_no_replay(worker.join().unwrap());
}

#[test]
fn component_discovery_rejects_stale_or_lost_witnesses_without_losing_base_status() {
    use super::super::query_component_status;
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let peer = Peer { address: listener.local_addr().unwrap(), key: Zeroizing::new(TOKEN) };
    let worker = std::thread::spawn(move || {
        status_reply(&listener, false); // Old bundle firmware never offered selector 2.
        let basic = serde_json::json!({"firmware":"fixture","boardDeviceId":BOARD,"bootId":BOOT,
            "wifiConnected":true,"clockSynchronized":false,"httpsSuccesses":0,"httpsFailures":0,
            "componentDelivery":true,"componentBundle":true,"controlTrusted":true,"rtcKey":64});
        for (board, boot, association) in [("another", BOOT, 1), (BOARD, "0123456789abcdef", 1), (BOARD, BOOT, 0)] {
            hello(&listener, DEVICE_STATUS, &[1], basic.to_string().as_bytes());
            let snapshot = serde_json::json!({"boardDeviceId":board,"bootId":boot,"association":association,"capabilities":{}});
            hello(&listener, DEVICE_STATUS, &[2], snapshot.to_string().as_bytes());
        }
        for stall in [false, true] {
            hello(&listener, DEVICE_STATUS, &[1], basic.to_string().as_bytes());
            let mut connection = accept(&listener);
            let mut client = [0; 16];
            connection.read_exact(&mut client).unwrap();
            if stall {
                connection.set_read_timeout(Some(Duration::from_secs(8))).unwrap();
                assert_eq!(connection.read(&mut [0]).unwrap(), 0, "client must close on timeout");
            }
        }
        listener
    });
    let old = query_component_status(&peer).unwrap();
    assert!(old.component_bundle && old.component_target.is_none() && old.component_target_error.is_none());
    for _ in 0..5 {
        let status = query_component_status(&peer).unwrap();
        assert!(status.wifi_connected && status.component_target.is_none() && status.component_target_error.is_some());
    }
    crate::lan_agent::confirmation_tests::assert_no_replay(worker.join().unwrap());
}

fn read_record(socket: &mut TcpStream, key: &aead::LessSafeKey, sequence: u32) -> Vec<u8> {
    let mut length = [0; 2];
    socket.read_exact(&mut length).unwrap();
    let mut record = vec![0; 16 + usize::from(u16::from_be_bytes(length))];
    socket.read_exact(&mut record).unwrap();
    key.open_in_place(record_nonce(2, sequence), aead::Aad::from(length), &mut record).unwrap().to_vec()
}

fn send_ack(socket: &mut TcpStream, key: &aead::LessSafeKey, sequence: u32, status: u8) {
    let mut ack = vec![status];
    key.seal_in_place_append_tag(record_nonce(3, sequence), aead::Aad::from([0, 1]), &mut ack).unwrap();
    socket.write_all(&[0, 1]).unwrap();
    socket.write_all(&ack).unwrap();
}

#[test]
fn interrupted_bundle_closes_without_replaying_data_or_commit() {
    use crate::lan_agent::confirmation_tests::assert_no_replay;
    let files = [("runtime/widget.json".to_owned(), vec![1; CHUNK + 1])];
    let target = [BOOT.as_bytes(), &7u32.to_be_bytes()].concat();
    for (last_sequence, stall) in [(1, false), (1, true), (3, false)] {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let peer = Peer { address: listener.local_addr().unwrap(), key: Zeroizing::new(TOKEN) };
        let expected = target.clone();
        let worker = std::thread::spawn(move || {
            let (mut socket, key) = hello(&listener, MODE, &expected, &[1]);
            for sequence in 0..=last_sequence {
                assert_eq!(read_record(&mut socket, &key, sequence)[0], [1, 2, 2, 3][sequence as usize]);
                if sequence == last_sequence {
                    if stall {
                        socket.set_read_timeout(Some(Duration::from_secs(10))).unwrap();
                        assert_eq!(socket.read(&mut [0]).unwrap(), 0);
                    }
                    break;
                }
                send_ack(&mut socket, &key, sequence, 1);
            }
            listener
        });
        let error = install_bundle(&peer, &target, "test-game", &files, None, CommitReply::Legacy, || Ok(())).unwrap_err();
        assert!(error.contains(if last_sequence == 3 { "提交结果未确认" } else { "已中断" }));
        assert_no_replay(worker.join().unwrap());
    }
}

#[test]
fn maintenance_repair_transfers_only_scoped_offer_chunks_and_one_commit() {
    let listener=TcpListener::bind("127.0.0.1:0").unwrap();
    let peer=Peer{address:listener.local_addr().unwrap(),key:Zeroizing::new(TOKEN)};
    let files=vec![("buttons.json".into(),b"[]".to_vec()),("runtime/widget.json".into(),b"{}".to_vec())];
    let target=[BOOT.as_bytes(),BOARD.as_bytes(),&17u32.to_be_bytes(),&[5]].concat();
    let observed=target.clone();
    let server=std::thread::spawn(move||{
        let files=receive(&listener,&observed,7,"demo",None).unwrap();
        (listener,files)
    });
    repair_bundle(&peer,&target,"demo",&files,||Ok(())).unwrap();
    let (listener,got)=server.join().unwrap();
    assert_eq!(got,files.into_iter().collect());
    crate::lan_agent::confirmation_tests::assert_no_replay(listener);
}

#[test]
fn one_byte_replies_identify_the_operation_and_stage_without_replaying() {
    // The receiver sends the same authenticated zero at different points. Only
    // the sending client knows which file and which block that reply belongs to.
    let files = [("runtime/widget.json".to_owned(), vec![1; CHUNK + 1])];
    let repair_files = [
        (center_thumb::MATCH.to_owned(), vec![1; 32]),
        (center_thumb::PATH.to_owned(), vec![2; 1568]),
    ];
    for (opcodes, status, expected) in [
        (&[1][..], 0, "安装申请"),
        (&[4][..], 0, "更新申请"),
        (&[6][..], 0, "封面修复申请"),
        (&[1, 2, 2][..], 0, "安装写入 runtime/widget.json 第 2 块（字节偏移 4096，共 4097 字节）"),
        (&[1, 2, 2, 3][..], 0, "安装最终提交"),
        (&[1, 2, 2, 3][..], 2, "无线安装提交结果未确认，请查看设备组件列表"),
        (&[7, 2][..], 0, "资源修复写入 runtime/widget.json 第 1 块"),
        (&[5, 5][..], 0, "字库传输写入 字库 第 2 块（字节偏移 4096，共 4097 字节）"),
    ] {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let peer = Peer { address: listener.local_addr().unwrap(), key: Zeroizing::new(TOKEN) };
        let mode = if opcodes[0] == 7 { super::super::maintenance::MODE } else { MODE };
        let worker = std::thread::spawn(move || {
            let (mut socket, key) = hello(&listener, mode, BOOT.as_bytes(), &[1]);
            for (i, &opcode) in opcodes.iter().enumerate() {
                assert_eq!(read_record(&mut socket, &key, i as u32)[0], opcode);
                send_ack(&mut socket, &key, i as u32, if i + 1 == opcodes.len() { status } else { 1 });
            }
            assert_eq!(socket.read(&mut [0]).unwrap(), 0, "no further records after failure");
            listener
        });
        let result = match opcodes[0] {
            5 => send_font(&peer, BOOT, "01234567890123456789012345678901", &files[0].1, || Ok(()), || Ok(())),
            7 => repair_bundle(&peer, BOOT.as_bytes(), "demo", &files, || Ok(())),
            6 => install_bundle(&peer, BOOT.as_bytes(), "demo", &repair_files, None, CommitReply::Legacy, || Ok(())),
            action => install_bundle(&peer, BOOT.as_bytes(), "demo", &files, (action == 4).then_some("1.0.0"), CommitReply::Legacy, || Ok(())),
        };
        let error = result.unwrap_err();
        assert!(error.contains(expected), "{error}");
        assert_eq!(error.contains("设备拒绝"), status == 0);
        assert_eq!(error.contains("结果未确认"), status != 0);
        crate::lan_agent::confirmation_tests::assert_no_replay(worker.join().unwrap());
    }
}

#[test]
fn legacy_one_shot_reply_distinguishes_refusal_from_an_unconfirmed_install() {
    let files = [("buttons.json".to_owned(), b"[]".to_vec()), ("runtime/widget.json".to_owned(), b"{}".to_vec())];
    let body = legacy_body("demo", BOOT, &files).unwrap();
    for status in [0, 1, 2] {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let peer = Peer { address: listener.local_addr().unwrap(), key: Zeroizing::new(TOKEN) };
        let expected_body = body.clone();
        let worker = std::thread::spawn(move || {
            hello(&listener, LEGACY_MODE, &expected_body, &[status]);
            listener
        });
        let result = install_legacy(&peer, "demo", &body);
        match status {
            1 => result.unwrap(),
            0 => assert!(result.unwrap_err().contains("拒绝旧版无线组件「demo」的一次性安装请求")),
            _ => assert!(result.unwrap_err().contains("安装结果未确认。请查看设备组件列表")),
        }
        crate::lan_agent::confirmation_tests::assert_no_replay(worker.join().unwrap());
    }
}

#[test]
fn final_commit_reason_requires_negotiation_and_an_authenticated_final_ack() {
    let files = [("buttons.json".to_owned(), b"[]".to_vec())];
    for (advertised, status, corrupt, expected) in [
        (None, 0, false, "固件未返回具体原因"),
        (Some("future"), 16, false, "无线安装提交结果未确认"),
        (Some("commit-reason-v1"), 16, false, "组件存储正忙"),
        (Some("commit-reason-v1"), 17, false, "音频正忙"),
        (Some("commit-reason-v1"), 18, false, "存储操作中断"),
        (Some("commit-reason-v1"), 19, false, "组件目录需要恢复"),
        (Some("commit-reason-v1"), 27, false, "组件存储失败"),
        (Some("commit-reason-v1"), 0, false, "固件未返回具体原因"),
        (Some("commit-reason-v1"), 2, false, "无线安装提交结果未确认"),
        (Some("commit-reason-v1"), 255, false, "无线安装提交结果未确认"),
        (Some("commit-reason-v1"), 16, true, "无线安装提交结果未确认"),
        (Some("commit-reason-v1"), 1, false, ""),
    ] {
        let caps = super::super::ComponentTarget {
            board_device_id: BOARD.into(), boot_id: BOOT.into(), association: 7,
            capabilities: serde_json::json!({"widgetLanCommitReason":advertised}),
        };
        let negotiated = CommitReply::for_target(advertised.map(|_| &caps));
        let target = [BOOT.as_bytes(), &caps.association.to_be_bytes()].concat();
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let peer = Peer { address: listener.local_addr().unwrap(), key: Zeroizing::new(TOKEN) };
        let witness = target.clone();
        let server = std::thread::spawn(move || {
            let (mut socket, key) = hello(&listener, MODE, &witness, &[1]);
            for (seq, opcode) in [(0, 1), (1, 2)] {
                assert_eq!(read_record(&mut socket, &key, seq)[0], opcode);
                send_ack(&mut socket, &key, seq, 1);
            }
            let commit = read_record(&mut socket, &key, 2);
            assert_eq!(commit, if advertised == Some("commit-reason-v1") { vec![3, 1] } else { vec![3] });
            send_ack(&mut socket, &key, if corrupt { 3 } else { 2 }, status);
            assert_eq!(socket.read(&mut [0]).unwrap(), 0);
            listener
        });
        let result = install_bundle(&peer, &target, "demo", &files, None, negotiated, || Ok(()));
        if expected.is_empty() {
            result.unwrap();
        } else {
            let error = result.unwrap_err();
            assert!(error.contains(expected), "{error}");
        }
        crate::lan_agent::confirmation_tests::assert_no_replay(server.join().unwrap());
    }
}
