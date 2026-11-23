//! Display-only projection of the existing USB push-to-talk round.

use serde_json::{json, Value};
use std::sync::{Condvar, Mutex};
use std::time::{Duration, Instant};

pub(crate) const ENGINE: &str = "p4-voice-feedback-v2";
pub(crate) const TOPIC: &str = "ui/voice-feedback";
const INTERVAL: Duration = Duration::from_millis(150);
const TEXT_BYTES: usize = 384;

// The device admits the physical press for 12 seconds after presentation.
// Allow bounded transport/scanout latency, while rejecting a delayed replay.
pub(crate) fn confirm_window_open(since_write: Duration) -> bool {
    since_write < Duration::from_secs(13)
}

pub(crate) fn supported(scope: &Value) -> bool {
    scope["runtime"] == "esp-p4" && scope["voiceFeedback"] == ENGINE
}

pub(crate) fn token(value: &Value) -> u32 {
    value
        .as_u64()
        .and_then(|value| u32::try_from(value).ok())
        .unwrap_or(0)
}

pub(crate) fn confirms(
    payload: &Value,
    capture: &str,
    token: u32,
    board: &str,
    boot: &str,
    connection: &str,
) -> bool {
    let scope = &payload["_nativeUsbScope"];
    token != 0
        && !capture.is_empty()
        && !board.is_empty()
        && !boot.is_empty()
        && !connection.is_empty()
        && supported(scope)
        && scope["boardDeviceId"] == board
        && scope["bootId"] == boot
        && scope["connectionId"] == connection
        && payload["context"] == "main"
        && payload["gesture"] == "short_press"
        && payload["handledLocally"] == false
        && payload["voiceDraftVisible"] == true
        && payload["voiceUtteranceId"] == capture
        && payload["voiceFeedbackToken"].as_u64() == Some(u64::from(token))
}

fn phase_rank(phase: &str) -> Option<u8> {
    Some(match phase {
        "listening" => 1,
        "partial" => 2,
        "finalizing" => 3,
        "draft_ready" => 4,
        "submitting" => 5,
        "submitted" | "queued" | "error" | "submit_unknown" | "cancelled" => 6,
        _ => return None,
    })
}

pub(crate) struct Projection<'a> {
    pub board: &'a str,
    pub boot: &'a str,
    pub capture: &'a str,
    pub token: u32,
    pub phase: &'a str,
    pub revision: u64,
    pub text: &'a str,
    pub inserted: bool,
}

fn display_text(text: &str) -> String {
    let mut result = String::new();
    for ch in text.chars() {
        // Keep the projection single-line and limit JSON escaping on the wire.
        let ch = if ch.is_control() || ch.is_whitespace() {
            ' '
        } else {
            ch
        };
        if result.len() + ch.len_utf8() > TEXT_BYTES {
            break;
        }
        result.push(ch);
    }
    result
}

impl Projection<'_> {
    pub(crate) fn payload(self) -> Option<Value> {
        if self.board.is_empty()
            || self.boot.is_empty()
            || self.capture.is_empty()
            || self.capture.len() >= 64
            || self.token == 0
            || self.revision > u64::from(u32::MAX)
        {
            return None;
        }
        phase_rank(self.phase)?;
        Some(json!({
            "boardDeviceId": self.board,
            "bootId": self.boot,
            "captureId": self.capture,
            "token": self.token,
            "revision": self.revision,
            "phase": self.phase,
            "text": display_text(self.text),
            "draftInserted": self.inserted,
        }))
    }
}

struct Pending<T> {
    epoch: u64,
    revision: u64,
    rank: u8,
    message: Option<T>,
    next: Instant,
}

pub(crate) struct Mailbox<T> {
    pending: Mutex<Pending<T>>,
    changed: Condvar,
}

impl<T> Mailbox<T> {
    pub(crate) fn new() -> Self {
        Self {
            pending: Mutex::new(Pending {
                epoch: 0,
                revision: 0,
                rank: 0,
                message: None,
                next: Instant::now(),
            }),
            changed: Condvar::new(),
        }
    }

    pub(crate) fn begin(&self) -> u64 {
        let Ok(mut pending) = self.pending.lock() else {
            return 0;
        };
        pending.epoch = pending.epoch.wrapping_add(1).max(1);
        pending.revision = 0;
        pending.rank = 0;
        pending.message = None;
        pending.epoch
    }

    pub(crate) fn publish(&self, epoch: u64, revision: u64, phase: &str, message: T) -> bool {
        let Some(rank) = phase_rank(phase) else {
            return false;
        };
        let Ok(mut pending) = self.pending.lock() else {
            return false;
        };
        if epoch == 0
            || pending.epoch != epoch
            || pending.rank == 6
            || rank < pending.rank
            || revision < pending.revision
            || (rank == pending.rank && revision == pending.revision)
        {
            return false;
        }
        pending.rank = rank;
        pending.revision = revision;
        pending.message = Some(message);
        self.changed.notify_one();
        true
    }

    pub(crate) fn receive(&self) -> Option<T> {
        let mut pending = self.pending.lock().ok()?;
        loop {
            if pending.message.is_none() {
                pending = self.changed.wait(pending).ok()?;
            } else {
                let remaining = pending.next.saturating_duration_since(Instant::now());
                if remaining.is_zero() {
                    pending.next = Instant::now() + INTERVAL;
                    return pending.message.take();
                }
                pending = self.changed.wait_timeout(pending, remaining).ok()?.0;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_visible_physical_confirm_from_the_exact_peer_and_capture_is_admitted() {
        let valid = json!({
            "_nativeUsbScope": {
                "boardDeviceId": "board", "bootId": "boot", "connectionId": "42",
                "runtime": "esp-p4", "voiceFeedback": ENGINE
            },
            "context": "main", "gesture": "short_press", "handledLocally": false,
            "voiceDraftVisible": true, "voiceUtteranceId": "p4-audio-123-4",
            "voiceFeedbackToken": 9,
        });
        let admits = |input: &Value| confirms(input, "p4-audio-123-4", 9, "board", "boot", "42");
        assert!(admits(&valid));
        for (key, stale) in [
            ("bootId", "previous-boot"),
            ("connectionId", "41"),
            ("boardDeviceId", "another-board"),
            ("voiceFeedback", "p4-voice-feedback-v1"),
            ("runtime", "linux"),
        ] {
            let mut input = valid.clone();
            input["_nativeUsbScope"][key] = json!(stale);
            assert!(!admits(&input));
        }
        for (key, value) in [
            ("voiceDraftVisible", json!(false)),
            ("voiceUtteranceId", json!("p4-audio-older")),
            ("voiceFeedbackToken", json!(8)),
            ("handledLocally", json!(true)),
            ("gesture", json!("long_press")),
            ("context", json!("app")),
            ("_nativeUsbScope", Value::Null),
        ] {
            let mut input = valid.clone();
            input[key] = value;
            assert!(!admits(&input));
        }
        assert!(!supported(&json!({"runtime": "esp-p4"})));
        assert_eq!(token(&json!(u64::MAX)), 0);
        assert!(confirm_window_open(Duration::from_secs(12)));
        assert!(!confirm_window_open(Duration::from_secs(13)));
    }

    #[test]
    fn payload_keeps_the_firmware_id_and_caps_unicode_without_splitting_it() {
        let payload = Projection {
            board: "board",
            boot: "boot",
            capture: "p4-audio-123-4",
            token: 7,
            phase: "draft_ready",
            revision: 2,
            text: &format!("台\n{}", "语".repeat(200)),
            inserted: true,
        }
        .payload()
        .unwrap();
        assert_eq!(payload["captureId"], "p4-audio-123-4");
        assert_eq!(payload["draftInserted"], true);
        let text = payload["text"].as_str().unwrap();
        assert!(text.len() <= TEXT_BYTES && text.ends_with('语'));
        assert!(text.starts_with("台 "));
    }

    #[test]
    fn admitted_pcm_begin_keeps_its_board_capture_id_for_feedback() {
        use crate::usb_audio::UsbAudioRelay;

        let mut audio = UsbAudioRelay::default();
        let (board, boot, connection) = ("test-board", "0123456789abcdef", "42");
        let begin = json!({
            "sessionId": "p4-audio-123-4", "boardDeviceId": board, "voiceFeedbackToken": 7,
            "format": "pcm_s16le", "sampleRate": 16000, "channels": 1, "bitsPerSample": 16,
        });
        assert_eq!(
            audio.configure_scoped(true, 50001, false, board, connection, boot)["ok"],
            true
        );
        assert!(audio
            .handle_scoped("audio/begin", &begin, board, "41", boot)
            .is_none());
        let event = audio
            .handle_scoped("audio/begin", &begin, board, connection, boot)
            .unwrap();
        assert_eq!(event["ok"], true);
        let wire = Projection {
            board,
            boot,
            capture: begin["sessionId"].as_str().unwrap(),
            token: token(&begin["voiceFeedbackToken"]),
            phase: "partial",
            revision: 1,
            text: "你好",
            inserted: false,
        }
        .payload()
        .unwrap();
        assert_eq!(wire["captureId"], event["utteranceId"]);
        assert_ne!(wire["captureId"], event["captureId"]);
        assert_eq!(wire["token"], 7);
    }

    #[test]
    fn mailbox_bounds_and_paces_wire_work_while_retaining_latest_terminal() {
        let mailbox = Mailbox::new();
        let old = mailbox.begin();
        assert!(mailbox.publish(old, 1, "partial", "first"));
        assert_eq!(mailbox.receive(), Some("first"));
        let first = Instant::now();
        assert!(mailbox.publish(old, 2, "draft_ready", "retired"));
        let current = mailbox.begin();
        assert!(!mailbox.publish(old, 3, "error", "old"));
        for revision in 0..1000 {
            assert!(mailbox.publish(current, revision, "partial", "partial"));
        }
        assert!(mailbox.publish(current, 999, "draft_ready", "draft"));
        assert!(mailbox.publish(current, 999, "submitted", "terminal"));
        assert!(!mailbox.publish(current, 1000, "error", "late failure"));
        assert_eq!(mailbox.receive(), Some("terminal"));
        assert!(first.elapsed() >= Duration::from_millis(140));
    }
}
