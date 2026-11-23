#include "pet_p4_voice_feedback.h"

#include <string.h>

#define PET_VOICE_DRAFT_MS 12000
#define PET_VOICE_PROCESSING_MS 20000
#define PET_VOICE_RESULT_MS 3500

void pet_voice_feedback_clear(pet_voice_feedback_t *view) {
  uint32_t sequence = view->sequence;
  const bool blocks_unscoped_confirm = view->blocks_unscoped_confirm;
  memset(view, 0, sizeof(*view));
  view->sequence = sequence;
  view->blocks_unscoped_confirm = blocks_unscoped_confirm;
}

uint32_t pet_voice_feedback_begin(pet_voice_feedback_t *view, uint64_t now) {
  pet_voice_feedback_clear(view);
  if (++view->sequence == 0) ++view->sequence;
  view->token = view->sequence;
  view->blocks_unscoped_confirm = true;
  view->phase = PET_VOICE_LISTENING;
  view->since_ms = now;
  return view->token;
}

/* Read a subset of the existing little-endian PCM without mutating the frame. */
uint8_t pet_voice_pcm_level(const uint8_t *pcm, size_t bytes) {
  unsigned sum = 0, samples = 0;
  for (size_t i = 0; i + 1 < bytes; i += 8) {
    int sample = pcm[i] | (pcm[i + 1] << 8);
    if (sample >= 32768) sample -= 65536;
    sum += sample < 0 ? -sample : sample;
    ++samples;
  }
  unsigned level = samples ? sum / samples / 24 : 0;
  return level > 100 ? 100 : level;
}

static bool expired(const pet_voice_feedback_t *view, uint64_t now) {
  uint64_t since = view->phase == PET_VOICE_DRAFT && view->presented_ms
    ? view->presented_ms : view->since_ms;
  /* The main loop can sample now before waiting for the protocol state lock. */
  if (now < since) return false;
  uint64_t elapsed = now - since;
  if (view->phase >= PET_VOICE_SUBMITTED) return elapsed >= PET_VOICE_RESULT_MS;
  if (view->phase == PET_VOICE_DRAFT) return elapsed >= PET_VOICE_DRAFT_MS;
  if (view->phase >= PET_VOICE_FINALIZING || !view->capture_id[0])
    return elapsed >= PET_VOICE_PROCESSING_MS;
  return false;
}

bool pet_voice_feedback_sync(pet_voice_feedback_t *view, const pet_voice_meter_t *meter, uint64_t now) {
  if (!view->token) return false;
  if (expired(view, now)) {
    pet_voice_feedback_clear(view);
    return false;
  }
  if (view->token != meter->token || !meter->capture_id[0]) return false;
  memcpy(view->capture_id, meter->capture_id, sizeof(view->capture_id));
  memcpy(view->bars, meter->bars, sizeof(view->bars));
  if (view->ended || !meter->ended) return false;
  view->ended = true;
  if (view->phase < PET_VOICE_FINALIZING) {
    view->phase = meter->failed ? PET_VOICE_ERROR : PET_VOICE_FINALIZING;
    view->since_ms = now;
  }
  return true;
}

static pet_voice_phase_t parse_phase(const char *phase) {
  static const char *const names[] = {
    "", "listening", "partial", "finalizing", "draft_ready", "submitting",
    "submitted", "queued", "error", "submit_unknown", "cancelled"
  };
  for (unsigned i = PET_VOICE_LISTENING; i <= PET_VOICE_CANCELLED; ++i)
    if (!strcmp(phase, names[i])) return (pet_voice_phase_t)i;
  return PET_VOICE_IDLE;
}

bool pet_voice_feedback_update(pet_voice_feedback_t *view, uint32_t token, const char *capture,
    uint32_t revision, const char *phase, const char *text, bool inserted, uint64_t now) {
  pet_voice_phase_t next = parse_phase(phase);
  size_t bytes = strlen(text);
  if (!token || view->token != token || !view->capture_id[0] || strcmp(view->capture_id, capture)
      || expired(view, now) || next == PET_VOICE_IDLE || next < view->phase
      || view->phase >= PET_VOICE_SUBMITTED || revision < view->revision
      || bytes >= sizeof(view->text) || (next == PET_VOICE_DRAFT && (!view->ended || !bytes))
      || ((next == PET_VOICE_SUBMITTING || next == PET_VOICE_SUBMITTED || next == PET_VOICE_QUEUED)
        && view->phase != PET_VOICE_SUBMITTING)) return false;
  if (view->phase != next) view->since_ms = now;
  view->phase = next;
  view->revision = revision;
  view->inserted |= inserted;
  if (next == PET_VOICE_SUBMITTED) view->blocks_unscoped_confirm = false;
  memcpy(view->text, text, bytes + 1);
  return true;
}

bool pet_voice_feedback_can_confirm(const pet_voice_feedback_t *view, uint64_t press_ms, uint64_t now) {
  return view->phase == PET_VOICE_DRAFT && view->ended && view->token
    && view->capture_id[0] && view->presented_ms && press_ms > view->presented_ms
    && press_ms <= now && !expired(view, now);
}

void pet_voice_feedback_confirmed(pet_voice_feedback_t *view, uint64_t now) {
  view->confirmed = true;
  view->phase = PET_VOICE_SUBMITTING;
  view->since_ms = now;
}
