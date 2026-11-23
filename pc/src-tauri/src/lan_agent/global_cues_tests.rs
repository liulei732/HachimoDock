// Real encrypted loopback requests. The optional host firmware adapter uses the
// production WAV writer with disposable storage and a synthetic active slot.
use super::*;
use crate::lan_agent::{nonce, session_key};
use ring::aead;
use std::io::{BufRead, BufReader, Write};
use std::net::TcpListener;
use std::process::{Command, Stdio};
use zeroize::Zeroizing;

const BOARD: &str = "fixture-board";
const BOOT: &str = "0123456789abcdef";
const MASTER: [u8; 16] = [0x15; 16];
const FAMILIES: [&str; 24] = [
    "welcome",
    "working",
    "idle",
    "done",
    "error",
    "waiting_user",
    "permission_request",
    "permission_approved",
    "permission_denied",
    "permission_expired",
    "session_terminated",
    "idle.playing",
    "idle.wandering",
    "idle.default",
    "idle.jumping",
    "working.thinking",
    "working.typing",
    "working.browsing",
    "touch.lick",
    "touch.what",
    "touch.left",
    "touch.right",
    "idle.begging",
    "idle.daydreaming",
];

fn wav(size: usize) -> Vec<u8> {
    let mut b =
        b"RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x01\0\x80\x3e\0\0\0\x7d\0\0\x02\0\x10\0data\0\0\0\0"
            .to_vec();
    b[4..8].copy_from_slice(&((size - 8) as u32).to_le_bytes());
    b[40..44].copy_from_slice(&((size - 44) as u32).to_le_bytes());
    b.resize(size, 0x35);
    b
}

fn slot_proof(request: &Value) -> Value {
    let mut ack = json!({"ok":true, "transferId":request["transferId"], "phase":"slot-query",
        "boardDeviceId":BOARD, "bootId":BOOT, "quiescent":true,"serialized":true,"nativeIoEpoch":12,
        "queryNonce":request["queryNonce"], "observationSchema":request["observationSchema"],
        "activeSlot":0,"slots":[{"slot":0,"valid":true,"packId":"0".repeat(64)},{"slot":1,"valid":false}]});
    for key in [
        "snapshotChanged",
        "assetTransferActive",
        "rawReceiving",
        "appearanceOwnerActive",
        "assetFileOpen",
        "assetSlotPending",
        "nativeTransferActive",
        "nativeReceiving",
        "nativeWorkerPending",
        "protocolWorkerPending",
        "storageWorkerPending",
        "firmwareOtaActive",
        "rebootPending",
    ] {
        ack[key] = json!(false);
    }
    ack
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Scenario {
    Maximum,
    StaleProof,
    BadAck,
    BadCommit,
    LostCommit,
    Cancel,
}

fn run(scenario: Scenario) {
    let tmp = tempfile::tempdir().unwrap();
    let dir = tmp.path().canonicalize().unwrap();
    let files = dir.join("global-audio-cues/audio-overrides");
    std::fs::create_dir_all(&files).unwrap();
    let mut overrides = serde_json::Map::new();
    let families = if scenario == Scenario::Maximum {
        &FAMILIES[..]
    } else {
        &FAMILIES[..1]
    };
    for (i, family) in families.iter().enumerate() {
        let size = match (scenario, i) {
            (Scenario::Maximum, 0) => 1024 * 1024,
            (Scenario::Maximum, 1) => 1024 * 1024 - 22 * 46,
            _ => 46,
        };
        std::fs::write(files.join(format!("{family}.wav")), wav(size)).unwrap();
        overrides.insert(
            family.to_string(),
            json!(format!("global-audio-cues/audio-overrides/{family}.wav")),
        );
    }
    std::fs::write(
        files.parent().unwrap().join("audio-overrides.json"),
        json!(overrides).to_string(),
    )
    .unwrap();
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let peer = Peer {
        address: listener.local_addr().unwrap(),
        key: Zeroizing::new(MASTER),
    };
    let usb = UsbSerialManager::new();
    usb.begin_appearance_sync().unwrap();
    let usb_for_cancel = usb.clone();
    let device_dir = dir.join("device");
    std::fs::create_dir_all(device_dir.join("spiffs")).unwrap();
    let worker = std::thread::spawn(move || {
        let mut process = std::env::var_os("PET_TEST_LAN_AUDIO_WORKER").map(|exe| {
            Command::new(exe)
                .current_dir(&device_dir)
                .stdin(Stdio::piped())
                .stdout(Stdio::piped())
                .spawn()
                .unwrap()
        });
        let mut output = process
            .as_mut()
            .map(|p| BufReader::new(p.stdout.take().unwrap()));
        let mut phases = Vec::new();
        let mut received = 0;
        let mut max_chunk = 0;
        loop {
            let mut stream = crate::lan_agent::confirmation_tests::accept(&listener);
            let mut client = [0; 16];
            stream.read_exact(&mut client).unwrap();
            let server = [0x24; 16];
            stream.write_all(&server).unwrap();
            let mut header = [0; 3];
            stream.read_exact(&mut header).unwrap();
            assert_eq!(header[0], MODE);
            let len = u16::from_be_bytes([header[1], header[2]]) as usize;
            assert!((19..16384).contains(&len));
            let key = session_key(&MASTER, &client, &server);
            let mut data = vec![0; len + 16];
            stream.read_exact(&mut data).unwrap();
            let body = key
                .open_in_place(nonce(0), aead::Aad::from(header), &mut data)
                .unwrap();
            assert_eq!(&body[..16], BOOT.as_bytes());
            let phase = body[16];
            phases.push(phase);
            let p: Value = serde_json::from_slice(&body[17..]).unwrap();
            assert_eq!(p["scope"], "global");
            let mut ack = if phase == 0 {
                slot_proof(&p)
            } else {
                assert_eq!(p["boardDeviceId"], BOARD);
                assert_eq!(p["bootId"], BOOT);
                assert_eq!(p["slot"], 0);
                assert_eq!(p["packId"], "0".repeat(64));
                let mut a = p.clone();
                a["ok"] = json!(true);
                a["phase"] = json!(match phase {
                    6 => "audio-begin",
                    7 => "audio-chunk",
                    8 => "audio-commit",
                    9 => "audio-abort",
                    _ => panic!("unexpected mutating phase {phase}"),
                });
                if phase == 6 {
                    assert_eq!(p["files"].as_array().unwrap().len(), families.len());
                    a.as_object_mut().unwrap().remove("files");
                } else if phase == 7 {
                    let n = STANDARD.decode(p["data"].as_str().unwrap()).unwrap().len();
                    assert!((1..=4096).contains(&n));
                    received += n;
                    max_chunk = max_chunk.max(n);
                    a["nextIndex"] = json!(p["index"].as_u64().unwrap() + 1);
                    a["receivedBytes"] = json!(p["offset"].as_u64().unwrap() + n as u64);
                    a.as_object_mut().unwrap().remove("data");
                } else if phase == 8 {
                    a["audioGeneration"] = json!(1);
                }
                if let Some(process) = process.as_mut() {
                    writeln!(process.stdin.as_mut().unwrap(), "{phase} {p}").unwrap();
                    let mut line = String::new();
                    output.as_mut().unwrap().read_line(&mut line).unwrap();
                    a = serde_json::from_str(&line).unwrap();
                    assert_eq!(a["ok"], true, "worker rejected phase {phase}");
                }
                a
            };
            if scenario == Scenario::StaleProof {
                ack["bootId"] = json!("different");
            }
            if scenario == Scenario::BadAck && phase == 7 {
                ack["receivedBytes"] = json!(0);
            }
            if scenario == Scenario::BadCommit && phase == 8 {
                ack["audioGeneration"] = json!(u64::from(u32::MAX) + 1);
            }
            if scenario == Scenario::LostCommit && phase == 8 {
                break;
            }
            if scenario == Scenario::Cancel && phase == 6 {
                assert!(usb_for_cancel.cancel_appearance_sync());
            }
            let mut reply =
                serde_json::to_vec(&json!({"topic":"asset/ack","payload":ack})).unwrap();
            assert!(reply.len() < REPLY_MAX);
            let length = (reply.len() as u16).to_be_bytes();
            let mut aad = header.to_vec();
            aad.extend_from_slice(&length);
            key.seal_in_place_append_tag(nonce(1), aead::Aad::from(aad), &mut reply)
                .unwrap();
            stream.write_all(&length).unwrap();
            stream.write_all(&reply).unwrap();
            if matches!(phase, 8 | 9)
                || scenario == Scenario::StaleProof
                || (scenario == Scenario::BadAck && phase == 7)
            {
                break;
            }
        }
        if let Some(mut child) = process {
            drop(child.stdin.take());
            assert!(child.wait().unwrap().success());
            if scenario == Scenario::Maximum {
                let read_wavs = |directory: &Path| {
                    let mut contents: Vec<Vec<u8>> = std::fs::read_dir(directory)
                        .unwrap()
                        .map(|entry| entry.unwrap().path())
                        .filter(|path| path.extension().and_then(|s| s.to_str()) == Some("wav"))
                        .map(|path| std::fs::read(path).unwrap())
                        .collect();
                    contents.sort();
                    contents
                };
                // The real writer uses bank/index filenames. Compare all bytes
                // independently of those private storage names and ordering.
                let stored = read_wavs(&device_dir.join("spiffs"));
                assert_eq!(stored.len(), 24);
                assert!(stored == read_wavs(&files));
                eprintln!(
                    "Firmware adapter persisted 24 WAVs, 2097152 bytes; all contents matched."
                );
            }
        }
        (listener, phases, received, max_chunk)
    });
    let transfer = Transfer {
        pairing: crate::lan_agent::component_events::pairing_revision(),
        peer: &peer,
        board: BOARD,
        boot: BOOT,
        capacity: 0,
        id: "fixture".into(),
        usb: &usb,
    };
    let result = transfer.global_cues(&dir);
    usb.finish_appearance_sync();
    let (listener, phases, received, max_chunk) = worker.join().unwrap();
    crate::lan_agent::confirmation_tests::assert_no_replay(listener);
    if scenario == Scenario::Maximum {
        assert_eq!(result.unwrap(), (24, 2 * 1024 * 1024, false));
        assert_eq!(received, 2 * 1024 * 1024);
        assert_eq!(max_chunk, 4096);
        assert_eq!(&phases[..2], &[0, 6]);
        assert_eq!(phases.last(), Some(&8));
        assert!(phases[2..phases.len() - 1].iter().all(|p| *p == 7));
    } else {
        assert!(result.is_err());
        let expected: &[u8] = match scenario {
            Scenario::StaleProof => &[0],
            Scenario::BadAck => &[0, 6, 7],
            Scenario::BadCommit | Scenario::LostCommit => &[0, 6, 7, 8],
            Scenario::Cancel => &[0, 6, 9],
            _ => unreachable!(),
        };
        assert_eq!(phases, expected);
    }
}

#[test]
fn global_cue_maximum_authenticated_transaction() {
    run(Scenario::Maximum);
}
#[test]
fn global_cue_rejects_stale_proof() {
    run(Scenario::StaleProof);
}
#[test]
fn global_cue_rejects_mismatched_chunk_ack() {
    run(Scenario::BadAck);
}
#[test]
fn global_cue_rejects_invalid_committed_generation() {
    run(Scenario::BadCommit);
}
#[test]
fn global_cue_lost_commit_has_no_replay() {
    run(Scenario::LostCommit);
}
#[test]
fn global_cue_explicit_cancel_aborts_admitted_transaction() {
    run(Scenario::Cancel);
}

#[test]
fn global_cue_admission_requires_negotiation_and_exact_trusted_target() {
    let mut wire = json!({"firmware":"fixture","boardDeviceId":BOARD,"bootId":BOOT,
        "clockSynchronized":true,"httpsSuccesses":0,"httpsFailures":0,"wifiConnected":true,
        "controlTrusted":true,"rtcKey":13,"ac":16777216,"globalAudioCues":true});
    let decode =
        |w: &Value| crate::lan_agent::decode_status(&serde_json::to_vec(w).unwrap()).unwrap();
    assert!(!global_cue_target(&decode(&wire), BOARD, BOOT));
    wire["rtcKey"] = json!(29);
    assert!(global_cue_target(&decode(&wire), BOARD, BOOT));
    assert_eq!(
        serde_json::to_value(decode(&wire)).unwrap()["globalAudioCues"],
        true
    );
    assert!(!global_cue_target(&decode(&wire), "other", BOOT));
    assert!(!global_cue_target(
        &decode(&wire),
        BOARD,
        "1123456789abcdef"
    ));
    wire["controlTrusted"] = json!(false);
    assert!(!global_cue_target(&decode(&wire), BOARD, BOOT));
}
