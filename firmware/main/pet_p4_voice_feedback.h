#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PET_VOICE_BARS 10
#define PET_VOICE_CAPTURE_BYTES 64
#define PET_VOICE_TEXT_BYTES 385
#define PET_VOICE_METER_INTERVAL_MS 80

typedef enum {
  PET_VOICE_IDLE, PET_VOICE_LISTENING, PET_VOICE_PARTIAL, PET_VOICE_FINALIZING,
  PET_VOICE_DRAFT, PET_VOICE_SUBMITTING, PET_VOICE_SUBMITTED,
  PET_VOICE_QUEUED, PET_VOICE_ERROR, PET_VOICE_UNKNOWN, PET_VOICE_CANCELLED
} pet_voice_phase_t;

typedef struct {
  uint32_t token;
  char capture_id[PET_VOICE_CAPTURE_BYTES];
  uint8_t bars[PET_VOICE_BARS];
  bool ended;
  bool failed;
} pet_voice_meter_t;

typedef struct {
  uint32_t sequence;
  uint32_t token;
  char capture_id[PET_VOICE_CAPTURE_BYTES];
  char text[PET_VOICE_TEXT_BYTES];
  uint8_t bars[PET_VOICE_BARS];
  uint32_t revision;
  uint64_t since_ms;
  uint64_t presented_ms;
  pet_voice_phase_t phase;
  bool ended;
  bool inserted;
  bool confirmed;
  bool blocks_unscoped_confirm;
} pet_voice_feedback_t;

void pet_voice_feedback_clear(pet_voice_feedback_t *view);
uint32_t pet_voice_feedback_begin(pet_voice_feedback_t *view, uint64_t now);
uint8_t pet_voice_pcm_level(const uint8_t *pcm, size_t bytes);
bool pet_voice_feedback_sync(pet_voice_feedback_t *view, const pet_voice_meter_t *meter, uint64_t now);
bool pet_voice_feedback_update(pet_voice_feedback_t *view, uint32_t token, const char *capture,
    uint32_t revision, const char *phase, const char *text, bool inserted, uint64_t now);
bool pet_voice_feedback_can_confirm(const pet_voice_feedback_t *view, uint64_t press_ms, uint64_t now);
void pet_voice_feedback_confirmed(pet_voice_feedback_t *view, uint64_t now);
