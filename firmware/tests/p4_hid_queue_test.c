#include <assert.h>
#include "pet_p4_hid_queue.h"

int main(void) {
  pet_hid_queue_t q = {0};
  assert(pet_hid_enqueue(&q, 5, 1));
  assert(pet_hid_peek(&q, 1) == 5);
  pet_hid_complete(&q, HID_NOT_SUBMITTED);
  assert(pet_hid_peek(&q, 1) == 5);
  pet_hid_complete(&q, HID_SUBMITTED);
  assert(!pet_hid_peek(&q, 1) && !atomic_load(&q.dropped));
  assert(pet_hid_enqueue(&q, 6, 1));
  pet_hid_complete(&q, HID_NOT_SUBMITTED);
  assert(!pet_hid_peek(&q, 2) && atomic_load(&q.dropped) == 1);
  assert(pet_hid_enqueue(&q, 7, 2));
  for (unsigned i = 0; i < 3; i++) {
    assert(pet_hid_peek(&q, 2) == 7);
    pet_hid_complete(&q, HID_NOT_SUBMITTED);
  }
  assert(!pet_hid_peek(&q, 2) && atomic_load(&q.dropped) == 2);
  assert(pet_hid_enqueue(&q, 8, 2));
  pet_hid_complete(&q, HID_SUBMISSION_FAILED);
  assert(!pet_hid_peek(&q, 2) && atomic_load(&q.dropped) == 3);
  for (unsigned i = 0; i < PET_HID_QUEUE_CAPACITY; i++) assert(pet_hid_enqueue(&q, 10, 2));
  assert(!pet_hid_enqueue(&q, 11, 2) && atomic_load(&q.full) == 1);
  pet_hid_clear(&q);
  assert(!pet_hid_peek(&q, 2) && atomic_load(&q.dropped) == 3 + PET_HID_QUEUE_CAPACITY);
}
