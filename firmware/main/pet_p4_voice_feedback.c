/*
 * [Input] Real S16 PCM and scoped, bounded PTT status metadata.
 * [Output] Local level history, expiring feedback state and native strip titles.
 * [Pos] Pure C core; no codec, USB, heap allocation or transcript persistence.
 * [Sync] Update firmware/.folder.md, protocol.md and feedback tests together.
 */
#include "pet_p4_voice_feedback.h"

/* Truncate only at UTF-8 boundaries; ASR text is display-only and bounded. */
void pet_voice_text_copy(char *out, size_t cap, const char *text) {
  if (!cap) return;
  if (!text) text = "";
  size_t n = strlen(text);
  if (n >= cap) {
    n = cap - 1;
    while (n && ((unsigned char)text[n] & 0xc0) == 0x80) --n;
  }
  memcpy(out, text, n); out[n] = 0;
}
unsigned pet_voice_pcm_level(const int16_t pcm[PET_VOICE_PCM_SAMPLES]) {
  // Display output saturates at RMS 1000. Stop before more energy can
  // accumulate, keeping the sum and all subsequent arithmetic in 32 bits.
  const uint32_t saturation = PET_VOICE_PCM_SAMPLES * 1000U * 1000U;
  uint32_t energy = 0;
  for (size_t i = 0; i < PET_VOICE_PCM_SAMPLES; ++i) {
    int32_t sample = pcm[i];
    energy += (uint32_t)(sample * sample);
    if (energy >= saturation) return 100;
  }
  uint32_t mean = energy / PET_VOICE_PCM_SAMPLES;
  uint32_t rms = 0;
  // Unsaturated mean is below 1000 squared, so only ten root bits are needed.
  for (unsigned bit = 512; bit; bit >>= 1) {
    unsigned next = rms | bit;
    if (next * next <= mean) rms = next;
  }
  return rms <= 20 ? 0 : (rms - 20) * 100 / 980;
}
void pet_voice_meter_sample(pet_voice_meter_t *m,
    unsigned raw, uint64_t now) {
  if (!m || !m->active || (m->sample_ms && (now < m->sample_ms || now - m->sample_ms < 50))) return;
  if (raw > 100) raw = 100;
  m->level = (uint8_t)(raw >= m->level ? (m->level + raw * 3) / 4
    : m->level * 2 / 3);
  if (m->level < 2) m->level = 0;
  memmove(m->bars, m->bars + 1, PET_VOICE_BARS - 1);
  m->bars[PET_VOICE_BARS - 1] = m->level;
  m->sample_ms = now;
}
void pet_voice_feedback_sync(pet_voice_feedback_t *v,
    const pet_voice_meter_t *m, uint64_t now) {
  if (!v || !m || !m->capture_id[0]) return;
  if (strcmp(v->capture_id, m->capture_id)) {
    memset(v, 0, sizeof(*v));
    pet_voice_text_copy(v->capture_id, sizeof(v->capture_id), m->capture_id);
    v->phase = PET_VOICE_LISTENING; v->started_ms = m->started_ms;
    v->expires_ms = now + 35000;
  }
  memcpy(v->bars, m->bars, sizeof(v->bars));
  if (m->active && m->sample_ms && now >= m->sample_ms && now - m->sample_ms > 200)
    memset(v->bars, 0, sizeof(v->bars));
  if (!m->active && !v->capture_ended) {
    v->capture_ended = true;
    if (v->phase == PET_VOICE_LISTENING) {
      v->phase = m->failed ? PET_VOICE_ERROR : PET_VOICE_RECOGNIZING;
      pet_voice_text_copy(v->hint, sizeof(v->hint), m->failed ? "录音失败，请重试" : "");
      v->expires_ms = now + 15000;
    }
  }
  if (v->phase != PET_VOICE_IDLE && now >= v->expires_ms) {
    if (v->phase == PET_VOICE_RECOGNIZING || v->phase == PET_VOICE_SUBMITTING) {
      pet_voice_text_copy(v->hint, sizeof(v->hint), "结果未确认，请检查电脑端");
      v->phase = PET_VOICE_UNKNOWN; v->expires_ms = now + 10000;
    } else {
      v->phase = PET_VOICE_IDLE;
      memset(v->text, 0, sizeof(v->text)); memset(v->hint, 0, sizeof(v->hint));
    }
  }
}
bool pet_voice_feedback_can_confirm(const pet_voice_feedback_t *v, uint64_t now) {
  return v->phase == PET_VOICE_DRAFT && v->utterance_id[0] && now < v->expires_ms;
}
void pet_voice_feedback_confirm_requested(pet_voice_feedback_t *v, uint64_t now) {
  v->phase = PET_VOICE_SUBMITTING;
  v->expires_ms = now + 15000;
}
static pet_voice_phase_t pet_voice_phase(const char *phase) {
  if (!strcmp(phase, "listening") || !strcmp(phase, "partial")) return PET_VOICE_LISTENING;
  if (!strcmp(phase, "recognizing") || !strcmp(phase, "finalizing")) return PET_VOICE_RECOGNIZING;
  if (!strcmp(phase, "draft_ready")) return PET_VOICE_DRAFT;
  if (!strcmp(phase, "submitting")) return PET_VOICE_SUBMITTING;
  if (!strcmp(phase, "submitted")) return PET_VOICE_SUBMITTED;
  if (!strcmp(phase, "error")) return PET_VOICE_ERROR;
  if (!strcmp(phase, "cancelled")) return PET_VOICE_CANCELLED;
  if (!strcmp(phase, "submit_unknown")) return PET_VOICE_UNKNOWN;
  return PET_VOICE_IDLE;
}
bool pet_voice_feedback_update(pet_voice_feedback_t *v,
    const char *capture, const char *utterance, uint64_t revision,
    const char *phase, const char *text, const char *hint, uint64_t now) {
  if (!v || !capture || !utterance || !utterance[0] || strlen(utterance) >= sizeof(v->utterance_id)
      || v->phase == PET_VOICE_IDLE || strcmp(v->capture_id, capture)
      || (v->utterance_id[0] && strcmp(v->utterance_id, utterance))
      || revision < v->revision || v->phase >= PET_VOICE_SUBMITTED) return false;
  pet_voice_phase_t next = pet_voice_phase(phase);
  if (!next) return false;
  /* A late partial may refine text during finalization, never reopen recording. */
  if (next < v->phase && !(next == PET_VOICE_LISTENING && v->phase == PET_VOICE_RECOGNIZING)) return false;
  pet_voice_text_copy(v->utterance_id, sizeof(v->utterance_id), utterance);
  if (text && text[0] && strcmp(v->text, text)) {
    pet_voice_text_copy(v->text, sizeof(v->text), text); v->text_changed_ms = now;
  }
  if (next >= v->phase) v->phase = next;
  if (v->capture_ended && v->phase == PET_VOICE_LISTENING) v->phase = PET_VOICE_RECOGNIZING;
  v->revision = revision;
  pet_voice_text_copy(v->hint, sizeof(v->hint), hint);
  v->expires_ms = now + (v->phase == PET_VOICE_SUBMITTED ? 4000 :
    v->phase == PET_VOICE_LISTENING ? 35000 : 15000);
  return true;
}
const char *pet_voice_feedback_title(const pet_voice_feedback_t *v) {
  switch (v->phase) {
    case PET_VOICE_RECOGNIZING: return "识别中...";
    case PET_VOICE_DRAFT: return v->draft_inserted ? "已写入 · 待发送" : "草稿就绪 · 待发送";
    case PET_VOICE_SUBMITTING: return "正在发送...";
    case PET_VOICE_SUBMITTED: return "已发送";
    case PET_VOICE_ERROR:
      switch (v->failure_stage) {
        case PET_VOICE_FAILURE_RECOGNITION: return "识别失败";
        case PET_VOICE_FAILURE_INSERTION: return "写入失败";
        case PET_VOICE_FAILURE_SUBMISSION: return "发送失败";
        default: return "语音处理失败";
      }
    case PET_VOICE_CANCELLED: return "本次语音已取消";
    case PET_VOICE_UNKNOWN: return "结果未确认 · 请查看电脑";
    default: return "正在聆听";
  }
}
