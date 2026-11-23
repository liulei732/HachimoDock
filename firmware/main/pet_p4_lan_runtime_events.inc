/* Included by the authenticated LAN dispatcher. Outbox belongs to RAM only.
 * Emitters hold the component lock; neither direction waits on the other lock. */
#include "pet_p4_runtime_events.h"
#include "pet_p4_component_features.h"
#include "pet_p4_notify.h"

typedef struct {
  char *json;
  uint64_t until, view;
  uint32_t pairing, association, usb_epoch;
  uint8_t tag;
} runtime_event_t;
static runtime_event_t runtime_outbox[2];
static uint32_t runtime_press_association, runtime_press_usb_epoch, runtime_press_pairing;
static atomic_bool rtc_wire_busy; /* also observed by the realtime socket */

bool pet_p4_runtime_wireless(void) {
  return !atomic_load(&usb_client_active) && atomic_load(&control_trusted);
}

bool pet_p4_runtime_emit(const char *topic, const cJSON *payload, uint64_t view) {
  const int tag = !strcmp(topic, "widget/notify") ? 2 : !strcmp(topic,"widget/data/refresh") ? 3 : 0;
  if (!tag || !lock || !pet_p4_runtime_wireless() || !view) return false;
  char *encoded = cJSON_PrintUnformatted(payload);
  if (!encoded || strlen(encoded)>PET_RUNTIME_EVENT_JSON_MAX || xSemaphoreTake(lock,0)!=pdTRUE) {
    cJSON_free(encoded); return false;
  }
  const uint64_t now=clock_ms();
  const uint32_t association=pet_device_wifi_association();
  bool queued=false;
  if (association && volume_available() && pet_p4_runtime_wireless()) {
    for (size_t n=0;n<2;n++) {
      runtime_event_t *event=&runtime_outbox[n];
      if (event->json && event->until>now && event->association==association &&
          event->pairing==generation && event->usb_epoch==atomic_load(&runtime_usb_epoch) && event->view==view) continue;
      cJSON_free(event->json);
      *event=(runtime_event_t){encoded,now+6000,view,generation,association,atomic_load(&runtime_usb_epoch),(uint8_t)tag};
      queued=true;
      break;
    }
  }
  xSemaphoreGive(lock);
  if (!queued) cJSON_free(encoded);
  return queued;
}

static uint32_t runtime_event_be32(const uint8_t *p) {
  return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}

/* Caller holds state and LAN lock. Consume before sealing the reply. */
static size_t runtime_event_exchange(pet_p4_runtime_state_t *state, const uint8_t *prefix,
                                     const cJSON *p, uint8_t *reply, uint64_t now) {
  const uint32_t association=pet_device_wifi_association();
  if (!association || runtime_event_be32(prefix+31)!=association ||
      strlen(state->board_device_id)!=15 || memcmp(prefix+16,state->board_device_id,15) ||
      strlen(pet_p4_boot_identity())!=16 || memcmp(prefix,pet_p4_boot_identity(),16) ||
      !volume_available() || !pet_p4_runtime_wireless() || pet_p4_state_usb_client_connected(state,now) ||
      atomic_load(&rtc_wire_busy) || state->asset_transfer_active || state->approval_active) return 0;
  switch(prefix[35]) {
    case 0:
      if (p) return 0;
      for (size_t n=0;n<2;n++) {
        runtime_event_t *event=&runtime_outbox[n];
        if (!event->json) continue;
        const bool live=event->association==association && event->pairing==generation && event->usb_epoch==atomic_load(&runtime_usb_epoch) && event->until>now &&
          !strcmp(state->screen_page,"app") && pet_p4_miniapp_runtime_event_current(event->view);
        size_t size=0;
        if (live) {
          reply[0]=event->tag;
          size=strlen(event->json);
          memcpy(reply+1,event->json,size);
        }
        cJSON_free(event->json);*event=(runtime_event_t){0};
        if (live) return 1+size;
      }
      break;
#if PET_P4_NOTIFY
    case 1:
      if (pet_p4_apply_component_notice(state,p,now)) return 0;
      break;
#endif
    case 2: {
      const cJSON *seq=cJSON_GetObjectItemCaseSensitive(p,"sequence");
      const char *owner=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(p,"owner"));
      if (!cJSON_IsObject(p) || cJSON_GetArraySize(p)!=2 || !owner || strlen(owner)!=32 || !cJSON_IsNumber(seq) ||
          seq->valuedouble<1 || seq->valuedouble>UINT32_MAX ||
          (double)(uint32_t)seq->valuedouble!=seq->valuedouble) return 0;
      const uint32_t id=(uint32_t)seq->valuedouble;
      static const char hex[]="0123456789abcdef";
      if (runtime_press_association!=association || runtime_press_pairing!=generation ||
          runtime_press_usb_epoch!=atomic_load(&runtime_usb_epoch)) return 0;
      for (size_t i=0;i<16;i++) {
        const uint8_t b=realtime_press.owner[i];
        if (owner[2*i]!=hex[b>>4] || owner[2*i+1]!=hex[b&15]) return 0;
      }
      static uint32_t last_failure;
      if (!pet_lan_press_failure_current(&realtime_press,now,id) || id==last_failure) return 0;
      cJSON *speech=cJSON_CreateObject();
      if (!speech || !cJSON_AddStringToObject(speech,"text","实时对话未能启动，请查看 PC 提示")) {
        cJSON_Delete(speech);return 0;
      }
      last_failure=id;
      pet_p4_lan_apply(state,NULL,NULL,speech,NULL);
      cJSON_Delete(speech);
      break;
    }
    default: return 0;
  }
  reply[0]=1;
  return 1;
}
