#pragma once
/* Ephemeral PTT feedback. No IO, allocation, persistence or synthetic animation. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define PET_VOICE_BARS 34
#define PET_VOICE_PCM_SAMPLES 320
#define PET_VOICE_TEXT_BYTES 512
#define PET_VOICE_CAPTURE_BYTES 64
typedef enum {
  PET_VOICE_IDLE, PET_VOICE_LISTENING, PET_VOICE_RECOGNIZING,
  PET_VOICE_DRAFT, PET_VOICE_SUBMITTING, PET_VOICE_SUBMITTED,
  PET_VOICE_ERROR, PET_VOICE_CANCELLED, PET_VOICE_UNKNOWN
} pet_voice_phase_t;
typedef enum {
  PET_VOICE_FAILURE_UNSPECIFIED, PET_VOICE_FAILURE_RECOGNITION,
  PET_VOICE_FAILURE_INSERTION, PET_VOICE_FAILURE_SUBMISSION
} pet_voice_failure_t;
typedef struct {
  char capture_id[PET_VOICE_CAPTURE_BYTES];
  char utterance_id[64];
  char text[PET_VOICE_TEXT_BYTES];
  char hint[128];
  pet_voice_phase_t phase;
  pet_voice_failure_t failure_stage;
  uint64_t revision, started_ms, expires_ms, text_changed_ms;
  uint8_t bars[PET_VOICE_BARS];
  bool capture_ended;
  bool draft_inserted;
} pet_voice_feedback_t;
typedef struct {
  char capture_id[PET_VOICE_CAPTURE_BYTES];
  uint8_t bars[PET_VOICE_BARS], level;
  uint64_t started_ms, sample_ms;
  bool active, failed;
} pet_voice_meter_t;

/* IO-free core, compiled once rather than inlined into protocol/audio/render. */
void pet_voice_text_copy(char *out, size_t cap, const char *text);
unsigned pet_voice_pcm_level(const int16_t pcm[PET_VOICE_PCM_SAMPLES]);
void pet_voice_meter_sample(pet_voice_meter_t *meter, unsigned raw, uint64_t now);
void pet_voice_feedback_sync(pet_voice_feedback_t *feedback,
    const pet_voice_meter_t *meter, uint64_t now);
bool pet_voice_feedback_can_confirm(const pet_voice_feedback_t *feedback, uint64_t now);
void pet_voice_feedback_confirm_requested(pet_voice_feedback_t *feedback, uint64_t now);
bool pet_voice_feedback_update(pet_voice_feedback_t *feedback,
    const char *capture, const char *utterance, uint64_t revision,
    const char *phase, const char *text, const char *hint, uint64_t now);
const char *pet_voice_feedback_title(const pet_voice_feedback_t *feedback);
