/* Real mode-24 queue/dispatcher and the USB-shared notification bubble writer. */
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#define PET_P4_SYSTEM_WIFI 1
#define PET_P4_NOTIFY 1
#include "pet_p4_runtime_events.h"
#include "pet_p4_lan_press.h"
#include "pet_p4_notify.h"

static bool lock=true, online=true;
static atomic_bool control_trusted=true,usb_client_active;
static atomic_uint runtime_usb_epoch;
static uint32_t generation=4,association=7;
static uint64_t now=100,view=3;
static pet_lan_press_t realtime_press;
#define pdTRUE 1
#define xSemaphoreTake(s,t) ((s)==true)
#define xSemaphoreGive(s) ((void)(s))
static uint64_t clock_ms(void){return now;}
static bool volume_available(void){return online;}
static uint32_t pet_device_wifi_association(void){return association;}
const char *pet_p4_boot_identity(void){return "0123456789abcdef";}
bool pet_p4_miniapp_runtime_event_current(uint64_t v){return v==view;}
bool pet_p4_state_usb_client_connected(const pet_p4_runtime_state_t *s,unsigned long long t){(void)s;(void)t;return usb_client_active;}
static void copy_text(char *out,size_t n,const char *text){snprintf(out,n,"%s",text);}
#include "pet_p4_runtime_notice.inc"
static unsigned speech_count;
void pet_p4_lan_apply(pet_p4_runtime_state_t *s,const char *src,const cJSON *status,const cJSON *speech,const cJSON *sessions){
  (void)s;assert(!src && !status && !sessions && cJSON_GetArraySize(speech)==1);
  assert(!strcmp(cJSON_GetObjectItem(speech,"text")->valuestring,"实时对话未能启动，请查看 PC 提示"));
  speech_count++;
}
#include "pet_p4_lan_runtime_events.inc"

int main(void){
  uint8_t prefix[36]={0},reply[514]={0};
  memcpy(prefix,"0123456789abcdefp4-001122334455",31);prefix[34]=7;
  static pet_p4_runtime_state_t s;
  strcpy(s.board_device_id,"p4-001122334455");strcpy(s.screen_page,"app");
  cJSON *notice=cJSON_Parse("{\"componentId\":\"clock\",\"title\":\"Ready\",\"badge\":4}");
  cJSON *refresh=cJSON_Parse("{\"componentId\":\"clock\",\"source\":\"computer.status\"}");
  assert(pet_p4_runtime_emit("widget/notify",notice,view));
  assert(pet_p4_runtime_emit("widget/data/refresh",refresh,view));
  assert(!pet_p4_runtime_emit("widget/notify",notice,view));
  assert(!pet_p4_runtime_emit("control/command",notice,view));
  size_t n=runtime_event_exchange(&s,prefix,NULL,reply,now);
  assert(n>1 && reply[0]==2 && n<sizeof(reply));
  reply[n]=0;cJSON *parsed=cJSON_Parse((char*)reply+1);
  assert(cJSON_Compare(notice,parsed,true));cJSON_Delete(parsed);
  n=runtime_event_exchange(&s,prefix,NULL,reply,now);assert(n>1 && reply[0]==3);
  reply[n]=0;parsed=cJSON_Parse((char*)reply+1);
  assert(cJSON_Compare(refresh,parsed,true));cJSON_Delete(parsed);
  assert(runtime_event_exchange(&s,prefix,NULL,reply,now)==1 && reply[0]==1);
  for(unsigned i=0;i<6;i++){
    assert(pet_p4_runtime_emit("widget/data/refresh",refresh,view));
    switch(i){
      case 0:view++;break;
      case 1:generation++;break;
      case 2:association++;prefix[34]++;break;
      case 3:runtime_usb_epoch++;break;
      case 4:now+=6000;break;
      case 5:strcpy(s.screen_page,"main");break;
    }
    assert(runtime_event_exchange(&s,prefix,NULL,reply,now)==1 && reply[0]==1);
  }
  prefix[35]=1;
  assert(runtime_event_exchange(&s,prefix,notice,reply,now)==1 && reply[0]==1);
  assert(!strcmp(s.current_title,"Ready · 4"));
  assert(!runtime_event_exchange(&s,prefix,notice,reply,now)); /* shared cooldown */
  assert(!runtime_event_exchange(&s,prefix,refresh,reply,now));
  prefix[35]=2;
  cJSON *key=cJSON_Parse("{\"sequence\":1,\"owner\":\"01000000000000000000000000000000\"}");
  uint8_t owner[16]={1};assert(pet_lan_press_poll(&realtime_press,now,owner,0));
  assert(!runtime_event_exchange(&s,prefix,key,reply,now));
  assert(pet_lan_press_push(&realtime_press,now,now,0));
  assert(!runtime_event_exchange(&s,prefix,key,reply,now)); /* foreign association */
  runtime_press_association=association;runtime_press_usb_epoch=runtime_usb_epoch;runtime_press_pairing=generation;
  realtime_press.owner[0]=2;assert(!runtime_event_exchange(&s,prefix,key,reply,now));realtime_press.owner[0]=1;
  assert(runtime_event_exchange(&s,prefix,key,reply,now)==1 && speech_count==1);
  assert(!runtime_event_exchange(&s,prefix,key,reply,now));
  assert(pet_lan_press_poll(&realtime_press,now,owner,1));
  assert(pet_lan_press_push(&realtime_press,now,now,0));
  assert(pet_lan_press_poll(&realtime_press,now,owner,2));
  cJSON_SetNumberValue(cJSON_GetObjectItem(key,"sequence"),2);
  assert(!runtime_event_exchange(&s,prefix,key,reply,now+3000));
  assert(runtime_event_exchange(&s,prefix,key,reply,now+2999)==1 && speech_count==2);
  prefix[35]=0;
  prefix[0]^=1;assert(!runtime_event_exchange(&s,prefix,NULL,reply,now));prefix[0]^=1;
  prefix[18]^=1;assert(!runtime_event_exchange(&s,prefix,NULL,reply,now));prefix[18]^=1;
  prefix[34]++;assert(!runtime_event_exchange(&s,prefix,NULL,reply,now));prefix[34]--;
  usb_client_active=true;assert(!runtime_event_exchange(&s,prefix,NULL,reply,now));usb_client_active=false;
  control_trusted=false;assert(!runtime_event_exchange(&s,prefix,NULL,reply,now));control_trusted=true;
  rtc_wire_busy=true;assert(!runtime_event_exchange(&s,prefix,NULL,reply,now));
  cJSON_Delete(key);cJSON_Delete(notice);cJSON_Delete(refresh);
}
