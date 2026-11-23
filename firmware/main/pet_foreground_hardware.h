// Private UI/audio broker. Caller serializes mutations; no guest or transport entry point.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint32_t owner, generation;
  uint64_t shown_ms, last_frame_ms;
  bool down, running, busy;
  unsigned level;
} pet_foreground_hardware_t;

static inline void pet_hw_revoke(pet_foreground_hardware_t *s, bool busy) {
  ++s->generation;
  s->running = s->down = false;
  s->level = 0;
  s->busy = busy;
}

static inline void pet_hw_select(pet_foreground_hardware_t *s, uint32_t owner) {
  if (s->owner == owner) return;
  pet_hw_revoke(s, false);
  s->owner = owner;
  s->shown_ms = 0;
}

// Ignore queued key-downs that predate presentation and releases from another owner.
static inline bool pet_hw_key(pet_foreground_hardware_t *s, bool down, bool click,
                              uint64_t event_ms, bool available) {
  if (!s->owner || !s->shown_ms || event_ms < s->shown_ms + 300) return false;
  if (down) s->down = true;
  if (!down && !click) s->down = false;
  if (!click || !s->down || event_ms > s->last_frame_ms + 1200) return false;
  pet_hw_revoke(s, !available);
  return available;
}

// Unsigned normalized peak amplitude. -32768 is representable in int32_t.
static inline unsigned pet_mic_peak(const int16_t *pcm, size_t count) {
  unsigned peak = 0;
  for (size_t i = 0; i < count; ++i) {
    int32_t sample = pcm[i];
    unsigned magnitude = (unsigned)(sample < 0 ? -sample : sample);
    if (magnitude > peak) peak = magnitude;
  }
  return (peak * 100u + 16384u) / 32768u;
}
