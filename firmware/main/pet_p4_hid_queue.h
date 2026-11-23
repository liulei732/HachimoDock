#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

// Single input producer and single NimBLE consumer. Each entry belongs to one link.
#define PET_HID_QUEUE_CAPACITY 16u
typedef struct {
  uint16_t chord;
  unsigned generation;
} pet_hid_press_t;

typedef struct {
  pet_hid_press_t entries[PET_HID_QUEUE_CAPACITY];
  atomic_uint read, write, full, dropped;
  unsigned attempts;
} pet_hid_queue_t;

typedef enum { HID_NOT_SUBMITTED, HID_SUBMISSION_FAILED, HID_SUBMITTED } pet_hid_result_t;

static inline bool pet_hid_enqueue(pet_hid_queue_t *q, uint16_t chord, unsigned generation) {
  unsigned w = atomic_load(&q->write);
  if (w - atomic_load(&q->read) >= PET_HID_QUEUE_CAPACITY) {
    atomic_fetch_add(&q->full, 1);
    return false;
  }
  q->entries[w % PET_HID_QUEUE_CAPACITY] = (pet_hid_press_t){chord, generation};
  atomic_store(&q->write, w + 1);
  return true;
}

static inline void pet_hid_clear(pet_hid_queue_t *q) {
  unsigned w = atomic_load(&q->write);
  unsigned count = w - atomic_exchange(&q->read, w);
  if (count) atomic_fetch_add(&q->dropped, count);
  q->attempts = 0;
}

static inline uint16_t pet_hid_peek(pet_hid_queue_t *q, unsigned generation) {
  unsigned r = atomic_load(&q->read);
  if (r == atomic_load(&q->write)) return 0;
  pet_hid_press_t press = q->entries[r % PET_HID_QUEUE_CAPACITY];
  if (press.generation == generation) return press.chord;
  atomic_fetch_add(&q->dropped, 1);
  atomic_store(&q->read, r + 1);
  q->attempts = 0;
  return 0;
}

static inline void pet_hid_complete(pet_hid_queue_t *q, pet_hid_result_t result) {
  // Only pre-submission allocation failure is safe to retry, on the next host tick.
  if (result == HID_NOT_SUBMITTED && ++q->attempts < 3) return;
  if (result != HID_SUBMITTED) atomic_fetch_add(&q->dropped, 1);
  q->attempts = 0;
  atomic_fetch_add(&q->read, 1);
}
