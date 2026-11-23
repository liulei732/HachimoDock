#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"

#define PET_P4_AGENT_MAX 64
#define PET_P4_SESSION_QUEUE_ID_MAX 128
enum { PET_VOICE_UNKNOWN, PET_VOICE_IDLE };
typedef struct {
  char board_device_id[32], screen_page[32], active_agent[64], current_session_agent[64], current_session_id[128];
  bool approval_active, session_voice_active, conversation, asset_transfer_active;
  struct { int phase; } voice_feedback;
} pet_p4_runtime_state_t;
static const char *pet_p4_boot_identity(void) { return "0123456789abcdef"; }
static uint64_t now = 1000;
static uint64_t esp_timer_get_time(void) { return now * 1000; }
static bool pet_p4_conversation_active(const pet_p4_runtime_state_t *s) { return s->conversation; }
static const char *json_string(const cJSON *o,const char *k) {
  const char *v=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,k)); return v?v:"";
}
#include "../main/pet_p4_test_message.inc"

static pet_p4_runtime_state_t s = {.board_device_id="p4-fixture",.screen_page="main",
  .active_agent="agent",.current_session_agent="agent",.current_session_id="private-current-session"};
static uint32_t phase(const char *op,uint64_t route,uint32_t token,bool admitted,const char *expected) {
  cJSON *q=cJSON_CreateObject();
  cJSON_AddStringToObject(q,"boardDeviceId",s.board_device_id);
  cJSON_AddStringToObject(q,"bootId",pet_p4_boot_identity());
  cJSON_AddStringToObject(q,"intent","00000000000000000000000000000001");
  cJSON_AddStringToObject(q,"phase",op);
  if(!route)cJSON_AddStringToObject(q,"requestId","usb-test");
  if(!strcmp(op,"send"))cJSON_AddNumberToObject(q,"token",token);
  cJSON *r=pet_p4_test_message(&s,q,route,admitted);
  assert(r && !strcmp(json_string(r,"status"),expected));
  char *wire=cJSON_PrintUnformatted(r);
  assert(!strstr(wire,"private-current-session"));
  uint32_t result=(uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(r,"token"));
  cJSON_free(wire);cJSON_Delete(r);cJSON_Delete(q);return result;
}
static void test_rejected_envelopes(void) {
  const char *invalid[] = {
    "{\"boardDeviceId\":\"other\",\"bootId\":\"0123456789abcdef\",\"intent\":\"00000000000000000000000000000001\",\"phase\":\"prepare\"}",
    "{\"boardDeviceId\":\"p4-fixture\",\"bootId\":\"1111111111111111\",\"intent\":\"00000000000000000000000000000001\",\"phase\":\"prepare\"}",
    "{\"boardDeviceId\":\"p4-fixture\",\"bootId\":\"0123456789abcdef\",\"intent\":\"00000000000000000000000000000001\",\"phase\":\"prepare\",\"text\":\"custom\"}",
  };
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    cJSON *request = cJSON_Parse(invalid[i]);
    cJSON *reply = pet_p4_test_message(&s, request, 1, true);
    assert(reply && !strcmp(json_string(reply, "status"), "refused"));
    assert(!pet_p4_test_message_visible(&s, now));
    cJSON_Delete(reply);
    cJSON_Delete(request);
  }
}

int main(void) {
  test_rejected_envelopes();
  const uint64_t wifi=0x100000002ULL;
  phase("prepare",wifi,0,false,"busy");
  uint32_t id=phase("prepare",wifi,0,true,"ready");
  phase("send",0,id,true,"changed");
  phase("send",wifi+1,id,true,"changed");
  strcpy(s.current_session_id,"next");
  phase("send",wifi,id,true,"changed");
  id=phase("prepare",wifi,0,true,"ready");
  now+=5000;phase("send",wifi,id,true,"expired");
  id=phase("prepare",wifi,0,true,"ready");
  phase("send",wifi,id,true,"delivered");
  now+=100;phase("send",wifi,id,true,"delivered");
  assert(pet_p4_test_message_visible(&s,now));now+=2900;
  assert(!pet_p4_test_message_visible(&s,now));
  id=phase("prepare",0,0,true,"ready");
  phase("send",0,id,true,"delivered");
  strcpy(s.active_agent,"other");
  assert(!pet_p4_test_message_visible(&s,now));
  strcpy(s.active_agent,"agent");
  assert(!pet_p4_test_message_visible(&s,now));
  id=phase("prepare",wifi,0,true,"ready");
  s.approval_active=true;phase("send",wifi,id,true,"busy");
  puts("USB and Wi-Fi: scoped prepare/send, changed session/association, expiry, busy, duplicate receipt without second display and private session redaction passed.");
}
