#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "cJSON.h"

#define PET_BLE_GESTURE_COUNT 20
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define ESP_OK 0
typedef int nvs_handle_t;
static unsigned commits, opened, closed;
static bool wifi6 = true, fail_commit;
static atomic_bool ready;
static bool pet_p4_is_wifi6_hardware(void) { return wifi6; }
static int nvs_open(const char *ns, int mode, nvs_handle_t *n) { (void)ns; *n=mode; return 0; }
static int nvs_get_blob(int n,const char *key,void *out,size_t *len) { return -1; }
static int nvs_set_blob(int n,const char *key,const void *bytes,size_t len) { return 0; }
static int nvs_commit(int n) { ++commits; return fail_commit ? -1 : 0; }
static void nvs_close(int n) {}
static bool pet_ble_keyboard_enabled(void);
static bool pet_ble_shortcut_paired(void) { return false; }
static bool pet_ble_shortcut_pairing(void) { return false; }
static int pet_ble_shortcut_code(void) { return 0; }
static int pet_ble_shortcut_available(void) { return 1; }
static void pet_ble_shortcut_open_pair(void) { ++opened; }
static void pet_ble_shortcut_close_pair(void) { ++closed; }
static const char *json_string(const cJSON *o,const char *key) {
  const char *s=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o,key));
  return s ? s : "";
}

/* Production setters */
/* Production protocol */

static void exchange(const char *body, bool ok, bool conditional, bool conflict) {
  assert(strlen(body)<=64);
  cJSON *request=cJSON_Parse(body);
  cJSON *reply=pet_p4_keyboard_config(request, "p4-0123456789ab");
  assert(reply && !strcmp(json_string(reply,"boardDeviceId"),"p4-0123456789ab"));
  assert(cJSON_IsTrue(cJSON_GetObjectItem(reply,"ok"))==ok);
  assert(cJSON_IsTrue(cJSON_GetObjectItem(reply,"conditionalApplied"))==conditional);
  assert((strcmp(json_string(reply,"error"),"keyboard_conflict")==0)==conflict);
  if (ok) {
    assert((strcmp(json_string(reply,"conditionalUpdate"),"keyboard-cas-v1")==0)==(CONFIG_BT_NIMBLE_ENABLED && wifi6));
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(reply,"keys"))==20);
    for (int i=0;i<20;i++)
      assert(cJSON_GetArrayItem(cJSON_GetObjectItem(reply,"keys"),i)->valueint==pet_ble_keyboard_chord(i));
    assert(cJSON_IsTrue(cJSON_GetObjectItem(reply,"enabled"))==pet_ble_keyboard_enabled());
  }
  char frame[512];
  assert(cJSON_PrintPreallocated(reply,frame,sizeof(frame),false));
  cJSON_Delete(request);
  cJSON_Delete(reply);
}

int main(void) {
  exchange("{}",true,false,false);
  wifi6=false;
  exchange("{\"setEnabled\":true,\"expectedEnabled\":false}",false,false,false);
  wifi6=true;
#if CONFIG_BT_NIMBLE_ENABLED
  // Client reads Ctrl+C, then an older client replaces that same slot.
  exchange("{\"slot\":0,\"chord\":5}",true,false,false);
  exchange("{\"slot\":0,\"setChord\":7,\"expectedChord\":262}",false,false,true);
  assert(commits==1 && pet_ble_keyboard_chord(0)==5);
  // An independent edit in slot 1 survives a successful conditional clear.
  exchange("{\"slot\":1,\"chord\":9}",true,false,false);
  exchange("{\"slot\":0,\"setChord\":0,\"expectedChord\":5}",true,true,false);
  assert(commits==3 && pet_ble_keyboard_chord(1)==9);
  exchange("{\"slot\":0,\"setChord\":8,\"expectedChord\":5}",false,false,true);
  exchange("{\"slot\":0,\"setChord\":8,\"expectedChord\":0}",true,true,false);
  assert(commits==4 && pet_ble_keyboard_chord(0)==8);
  // The physical settings button calls this same setter between preflight and write.
  assert(pet_ble_keyboard_set_mode(true));
  exchange("{\"setEnabled\":true,\"expectedEnabled\":false}",false,false,true);
  assert(commits==5 && opened==1 && closed==0);
  exchange("{\"setEnabled\":false,\"expectedEnabled\":true}",true,true,false);
  assert(commits==6 && !pet_ble_keyboard_enabled() && closed==1);
  exchange("{\"enabled\":true}",true,false,false);
  assert(commits==7 && opened==2);
  const char *invalid[]={
    "{\"expectedEnabled\":false}", "{\"setEnabled\":true}", "{\"expectedChord\":0}",
    "{\"slot\":0,\"setChord\":5}", "{\"slot\":0,\"chord\":5,\"expectedChord\":8}",
    "{\"enabled\":false,\"expectedEnabled\":true}", "{\"setEnabled\":false,\"expectedEnabled\":1}",
    "{\"slot\":0,\"setChord\":4,\"expectedChord\":0.5}", "{\"slot\":10,\"setChord\":4,\"expectedChord\":0}",
    "{\"slot\":20,\"setChord\":4,\"expectedChord\":0}", "{\"slot\":0,\"setChord\":123,\"expectedChord\":8}",
    "{\"slot\":0,\"setChord\":8,\"expectedChord\":65536}"};
  for (unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);++i) exchange(invalid[i],false,false,false);
  assert(commits==7 && opened==2 && closed==1 && pet_ble_keyboard_chord(0)==8);
  fail_commit=true;
  exchange("{\"slot\":0,\"setChord\":4,\"expectedChord\":8}",false,false,false);
  assert(pet_ble_keyboard_chord(0)==8);
  fail_commit=false;
  for (unsigned i=0;i<20;++i) if (keyboard_slot_valid(i)) assert(pet_ble_keyboard_set_chord(i,0xff65));
  exchange("{\"slot\":0,\"setChord\":65381,\"expectedChord\":65381}",true,true,false);
#else
  exchange("{\"slot\":0,\"setChord\":4,\"expectedChord\":262}",false,false,false);
  assert(!commits);
#endif
  return 0;
}
