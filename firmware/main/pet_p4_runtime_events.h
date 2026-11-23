#pragma once
#include "pet_p4_protocol.h"
#include "cJSON.h"

#define PET_RUNTIME_EVENT_PREFIX 36u
#define PET_RUNTIME_EVENT_JSON_MAX 512u
#define PET_RUNTIME_EVENT_REPLY_MAX (PET_RUNTIME_EVENT_JSON_MAX + 1u)
#if PET_P4_SYSTEM_WIFI
bool pet_p4_runtime_wireless(void);
bool pet_p4_runtime_emit(const char *topic, const cJSON *payload, uint64_t view);
bool pet_p4_miniapp_runtime_event_current(uint64_t view);
#endif
#if PET_P4_NOTIFY
/* Both transports share the protocol's limiter and bubble writer. */
const char *pet_p4_apply_component_notice(pet_p4_runtime_state_t *state,
                                          const cJSON *payload, uint64_t now);
#endif
