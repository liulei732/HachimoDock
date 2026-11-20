//! Bounded, ephemeral projection of PTT recognition and delivery onto the device.
//! The single active capture owns one replaceable pending snapshot. Neither
//! publication nor consumption performs IO while holding the metadata lock.
use serde_json::{json, Value};
use std::sync::{Condvar, Mutex};

pub const ENGINE: &str = "p4-voice-feedback-v1";

#[derive(Clone, Copy)]
pub enum FailureStage {
    Recognition,
    Insertion,
    Submission,
}

impl FailureStage {
    fn as_str(self) -> &'static str {
        match self {
            Self::Recognition => "recognition",
            Self::Insertion => "insertion",
            Self::Submission => "submission",
        }
    }
}

pub struct Feedback<'a> {
    /// The device's audio/begin.sessionId and the PTT confirmation owner.
    pub device_session_id: &'a str,
    pub board: &'a str,
    pub boot: &'a str,
    pub revision: u64,
    pub phase: &'a str,
    pub text: &'a str,
    pub error: &'a str,
    pub failure_stage: FailureStage,
    /// Set only after the native writer's insertion acknowledgement.
    pub draft_inserted: bool,
}

fn phase_rank(phase: &str) -> Option<u8> {
    Some(match phase {
        "listening" | "partial" => 1,
        "recognizing" | "finalizing" => 2,
        "draft_ready" => 3,
        "submitting" => 4,
        "submitted" | "error" | "cancelled" | "submit_unknown" => 5,
        _ => return None,
    })
}

fn clipped(text: &str, limit: usize) -> &str {
    let mut end = text.len().min(limit);
    while !text.is_char_boundary(end) {
        end -= 1;
    }
    &text[..end]
}

pub fn payload(feedback: Feedback<'_>) -> Option<Value> {
    let Feedback {
        device_session_id,
        board,
        boot,
        revision,
        phase,
        text,
        error,
        failure_stage,
        draft_inserted,
    } = feedback;
    if [device_session_id, board, boot]
        .iter()
        .any(|s| s.is_empty() || s.len() >= 64)
        || revision > 9_007_199_254_740_991
        || phase_rank(phase).is_none()
    {
        return None;
    }
    // Provider/OS error strings can contain private data. Project fixed hints.
    let hint = match phase {
        "error" => match failure_stage {
            FailureStage::Submission if draft_inserted => "发送失败，已写入的草稿请在电脑确认",
            FailureStage::Submission => "发送失败，请查看电脑端",
            FailureStage::Insertion if error.contains("多个") => "请保留目标主窗口",
            FailureStage::Insertion => "写入失败，请查看电脑端",
            FailureStage::Recognition => "识别失败，请重试或查看电脑端",
        },
        "cancelled" => "本次语音已取消，请查看电脑端",
        "submit_unknown" => "发送结果未确认，请勿重复提交",
        "draft_ready" if draft_inserted => "短按语音键 / 按下摇杆发送",
        "draft_ready" => "草稿已就绪，等待确认",
        _ => "",
    };
    Some(json!({
        "captureId": device_session_id, "utteranceId": device_session_id, "boardDeviceId": board,
        "bootId": boot, "revision": revision, "phase": phase,
        "text": clipped(text, 511), "hint": hint, "draftInserted": draft_inserted,
        "failureStage": if phase == "error" { failure_stage.as_str() } else { "" },
    }))
}

struct Pending<T> {
    epoch: u64,
    revision: u64,
    rank: u8,
    message: Option<T>,
}

/// One active physical capture, one pending snapshot, one writer. New captures
/// retire old callbacks even if they arrive after a newer capture's terminal.
/// The watermark survives consumption so stale events cannot reopen a result.
pub struct Mailbox<T> {
    pending: Mutex<Pending<T>>,
    ready: Condvar,
}

impl<T> Default for Mailbox<T> {
    fn default() -> Self {
        Self::new()
    }
}

impl<T> Mailbox<T> {
    pub fn new() -> Self {
        Self {
            pending: Mutex::new(Pending {
                epoch: 0,
                revision: 0,
                rank: 0,
                message: None,
            }),
            ready: Condvar::new(),
        }
    }

    /// Called once when publishing a new capture, never from an ASR callback.
    pub fn begin_capture(&self) -> u64 {
        let Ok(mut pending) = self.pending.lock() else {
            return 0;
        };
        let Some(epoch) = pending.epoch.checked_add(1) else {
            return 0;
        };
        *pending = Pending {
            epoch,
            revision: 0,
            rank: 0,
            message: None,
        };
        epoch
    }

    /// Replace superseded partials rather than dropping a terminal on overflow.
    /// The only wait is a short metadata lock, never the serial writer or IO.
    pub fn publish(&self, epoch: u64, revision: u64, phase: &str, message: T) -> bool {
        let Some(rank) = phase_rank(phase) else {
            return false;
        };
        let Ok(mut pending) = self.pending.lock() else {
            return false;
        };
        if epoch == 0
            || pending.epoch != epoch
            || revision < pending.revision
            || rank < pending.rank
            || pending.rank == 5
        {
            return false;
        }
        pending.revision = revision;
        pending.rank = rank;
        pending.message = Some(message);
        self.ready.notify_one();
        true
    }

    pub fn receive(&self) -> Option<T> {
        let mut pending = self.pending.lock().ok()?;
        loop {
            if let Some(message) = pending.message.take() {
                return Some(message);
            }
            pending = self.ready.wait(pending).ok()?;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn feedback<'a>(phase: &'a str, text: &'a str) -> Feedback<'a> {
        Feedback {
            device_session_id: "p4-audio-123456-7",
            board: "board",
            boot: "boot",
            revision: 1,
            phase,
            text,
            error: "",
            failure_stage: FailureStage::Recognition,
            draft_inserted: false,
        }
    }

    #[test]
    fn unicode_is_bounded_and_errors_do_not_disclose_secrets() {
        let text = "语".repeat(300);
        let mut f = feedback("error", &text);
        f.error = "多个窗口 PRIVATE_KEY";
        f.failure_stage = FailureStage::Insertion;
        let p = payload(f).unwrap();
        assert!(p["text"].as_str().unwrap().len() <= 511);
        assert!(p["text"].as_str().unwrap().ends_with('语'));
        assert_eq!(p["hint"], "请保留目标主窗口");
        assert!(!p.to_string().contains("PRIVATE_KEY"));
    }

    #[test]
    fn unsupported_and_unbound_messages_are_not_sent() {
        let mut f = feedback("partial", "text");
        f.device_session_id = "";
        assert!(payload(f).is_none());
        assert!(payload(feedback("invented", "text")).is_none());
    }

    #[test]
    fn a_draft_requires_an_insertion_acknowledgement() {
        assert_eq!(
            payload(feedback("draft_ready", "text")).unwrap()["draftInserted"],
            false
        );
        let mut f = feedback("draft_ready", "text");
        f.draft_inserted = true;
        assert_eq!(payload(f).unwrap()["draftInserted"], true);
    }

    #[test]
    fn wire_feedback_uses_the_firmware_session_and_confirmation_owner() {
        use crate::usb_audio::UsbAudioRelay;

        let mut relay = UsbAudioRelay::default();
        let board = "p4-test";
        let boot = "0123456789abcdef";
        let connection = "42";
        let mut f = feedback("draft_ready", "测试");
        let session_id = f.device_session_id;
        assert_eq!(relay.configure_scoped(true, 50001, false, board, connection, boot)["ok"], true);
        let begin = json!({
            "sessionId": session_id, "boardDeviceId": board, "format": "pcm_s16le",
            "sampleRate": 16000, "channels": 1, "bitsPerSample": 16,
        });
        let event = relay.handle_scoped("audio/begin", &begin, board, connection, boot).unwrap();
        assert_eq!(event["ok"], true);
        assert_ne!(event["captureId"], session_id);
        f.device_session_id = event["sessionId"].as_str().unwrap();
        f.board = board;
        f.boot = boot;
        f.draft_inserted = true;
        let wire = payload(f).unwrap();
        assert_eq!(wire["captureId"], begin["sessionId"]);
        assert_eq!(wire["utteranceId"], event["utteranceId"]);
        assert_ne!(wire["captureId"], event["captureId"]);
        assert_eq!(wire["draftInserted"], true);
    }

    #[test]
    fn submission_failure_preserves_the_insertion_receipt() {
        let mut f = feedback("error", "draft already in the editor");
        f.failure_stage = FailureStage::Submission;
        f.draft_inserted = true;
        let p = payload(f).unwrap();
        assert_eq!(p["draftInserted"], true);
        assert_eq!(p["failureStage"], "submission");
        assert!(p["hint"].as_str().unwrap().contains("发送失败"));
        assert!(!p["hint"].as_str().unwrap().contains("未写入"));
    }

    #[test]
    fn partial_text_is_not_an_insertion_or_final_recognition_receipt() {
        let p = payload(feedback("error", "unfinished recognition")).unwrap();
        assert_eq!(p["failureStage"], "recognition");
        assert!(p["hint"].as_str().unwrap().starts_with("识别失败"));
    }

    #[test]
    fn slow_consumer_retains_terminal_after_a_burst() {
        let mailbox = Mailbox::new();
        let epoch = mailbox.begin_capture();
        for revision in 0..1000 {
            assert!(mailbox.publish(epoch, revision, "partial", "partial"));
        }
        assert!(mailbox.publish(epoch, 999, "submitted", "submitted"));
        assert!(!mailbox.publish(epoch, 1000, "partial", "stale"));
        assert_eq!(mailbox.receive(), Some("submitted"));
        assert!(!mailbox.publish(epoch, 1001, "error", "late error"));
    }

    #[test]
    fn retired_captures_and_revisions_cannot_replace_a_new_result() {
        let mailbox = Mailbox::new();
        let old = mailbox.begin_capture();
        assert!(mailbox.publish(old, 10, "draft_ready", "old draft"));
        let current = mailbox.begin_capture();
        assert!(!mailbox.publish(old, 11, "error", "old failure"));
        assert!(mailbox.publish(current, 2, "partial", "current partial"));
        assert!(!mailbox.publish(current, 1, "partial", "old revision"));
        assert!(mailbox.publish(current, 2, "draft_ready", "current draft"));
        assert!(!mailbox.publish(current, 3, "listening", "late ready"));
        assert_eq!(mailbox.receive(), Some("current draft"));
    }

    #[test]
    fn writer_does_not_own_the_mailbox_during_io() {
        let mailbox = std::sync::Arc::new(Mailbox::new());
        let epoch = mailbox.begin_capture();
        let (entered, waiting) = std::sync::mpsc::channel();
        let (release, resumed) = std::sync::mpsc::channel();
        let worker_mailbox = mailbox.clone();
        let writer = std::thread::spawn(move || {
            assert_eq!(worker_mailbox.receive(), Some("partial"));
            entered.send(()).unwrap();
            resumed
                .recv_timeout(std::time::Duration::from_secs(2))
                .unwrap(); // Simulates slow IO outside the lock.
            assert_eq!(worker_mailbox.receive(), Some("submitted"));
        });
        assert!(mailbox.publish(epoch, 1, "partial", "partial"));
        waiting
            .recv_timeout(std::time::Duration::from_secs(2))
            .unwrap();
        assert!(mailbox.publish(epoch, 1, "submitted", "submitted"));
        release.send(()).unwrap();
        writer.join().unwrap();
    }
}
