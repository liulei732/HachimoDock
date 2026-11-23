#include "pet_p4_voice_feedback.h"

#include <assert.h>
#include <string.h>

int main(void) {
  uint8_t silence[640] = {0}, voice[640] = {0}, original[640];
  for (size_t i = 0; i < sizeof(voice); i += 2) voice[i + 1] = 0xe0;
  memcpy(original, voice, sizeof(voice));
  assert(pet_voice_pcm_level(silence, sizeof(silence)) == 0);
  assert(pet_voice_pcm_level(voice, sizeof(voice)) > 0);
  assert(!memcmp(original, voice, sizeof(voice)));

  pet_voice_feedback_t view = {0};
  assert(!view.blocks_unscoped_confirm);
  uint32_t token = pet_voice_feedback_begin(&view, 10);
  assert(view.blocks_unscoped_confirm);
  pet_voice_meter_t meter = {.token = token, .capture_id = "capture-a", .bars = {1, 23}};
  assert(!pet_voice_feedback_sync(&view, &meter, 11));
  assert(view.bars[1] == 23);
  assert(!pet_voice_feedback_update(&view, token, "other", 1, "partial", "你好", false, 12));
  assert(pet_voice_feedback_update(&view, token, "capture-a", 2, "partial", "你好", false, 12));
  assert(!strcmp(view.text, "你好"));
  assert(!pet_voice_feedback_update(&view, token, "capture-a", 1, "partial", "旧", false, 13));
  assert(!pet_voice_feedback_update(&view, token, "capture-a", 3, "draft_ready", "草稿", true, 14));
  meter.ended = true;
  assert(pet_voice_feedback_sync(&view, &meter, 15));
  assert(view.phase == PET_VOICE_FINALIZING);
  assert(!pet_voice_feedback_update(&view, token, "capture-a", 4, "partial", "迟到", false, 16));
  assert(pet_voice_feedback_update(&view, token, "capture-a", 3, "draft_ready", "草稿", true, 17));
  assert(!pet_voice_feedback_can_confirm(&view, 17, 18));
  assert(!pet_voice_feedback_sync(&view, &meter, 16) && view.phase == PET_VOICE_DRAFT);
  view.presented_ms = 20;
  assert(!pet_voice_feedback_can_confirm(&view, 19, 22));
  assert(pet_voice_feedback_can_confirm(&view, 21, 22));
  assert(!pet_voice_feedback_update(&view, token, "capture-a", 3, "submitted", "过早结果", true, 23));
  pet_voice_feedback_confirmed(&view, 23);
  assert(!pet_voice_feedback_can_confirm(&view, 24, 25));
  assert(pet_voice_feedback_update(&view, token, "capture-a", 3, "submit_unknown", "草稿", true, 25));
  assert(!pet_voice_feedback_update(&view, token, "capture-a", 4, "submitted", "迟到结果", true, 26));
  assert(!pet_voice_feedback_sync(&view, &meter, 3525) && view.phase == PET_VOICE_IDLE);
  assert(view.blocks_unscoped_confirm);

  uint32_t next = pet_voice_feedback_begin(&view, 4000);
  assert(next != token && !pet_voice_feedback_sync(&view, &meter, 4001) && !view.capture_id[0]);
  meter.token = next;
  strcpy(meter.capture_id, "capture-b");
  assert(pet_voice_feedback_sync(&view, &meter, 4002));
  assert(!pet_voice_feedback_update(&view, token, "capture-b", 1, "draft_ready", "旧稿", true, 4003));
  assert(pet_voice_feedback_update(&view, next, "capture-b", 2, "draft_ready", "草稿", true, 4004));
  view.presented_ms = 4005;
  assert(pet_voice_feedback_can_confirm(&view, 16004, 16004));
  assert(!pet_voice_feedback_can_confirm(&view, 16004, 16005));
  assert(!pet_voice_feedback_sync(&view, &meter, 16005) && !view.text[0]);
  assert(!pet_voice_feedback_update(&view, next, "capture-b", 3, "draft_ready", "旧稿", true, 16006));

  uint32_t connected = pet_voice_feedback_begin(&view, 17000);
  pet_voice_feedback_clear(&view);  // A fresh USB bind retires the previous capture.
  assert(view.blocks_unscoped_confirm);
  uint32_t newest = pet_voice_feedback_begin(&view, 17001);
  assert(newest != connected);
  meter.token = newest;
  assert(pet_voice_feedback_sync(&view, &meter, 17002));
  assert(pet_voice_feedback_update(&view, newest, "capture-b", 2, "draft_ready", "草稿", true, 17003));
  pet_voice_feedback_confirmed(&view, 17004);
  assert(pet_voice_feedback_update(&view, newest, "capture-b", 2, "submitted", "", true, 17005));
  assert(!pet_voice_feedback_sync(&view, &meter, 20505) && !view.blocks_unscoped_confirm);
  return 0;
}
