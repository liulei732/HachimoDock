#include "pet_p4_runtime_events.h"
#if PET_P4_NOTIFY
const char *pet_p4_apply_component_notice(pet_p4_runtime_state_t *state,const cJSON *payload,uint64_t now) {
  static pet_notify_limiter_t limiter;
  pet_notify_t notice;
  if (!pet_notify_parse(payload,&notice)) return "invalid_notify";
  if (!pet_notify_admit(&limiter,notice.component,now)) return "notify_cooldown";
  pet_notify_bubble_title(&notice,state->current_title,sizeof(state->current_title));
  copy_text(state->current_speech,sizeof(state->current_speech),notice.text);
  copy_text(state->current_status_text,sizeof(state->current_status_text),"通知");
  state->last_update_ms=now;
  return NULL;
}
#endif
