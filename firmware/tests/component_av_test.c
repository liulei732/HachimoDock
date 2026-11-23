#include <assert.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "pet_p4_component_av.h"
#include "pet_p4_json_source.h"

typedef int *SemaphoreHandle_t;
#define pdTRUE 1
#define portMAX_DELAY -1
#define ESP_MAC_BASE 1
#define PET_P4_AUDIO_FRAME_BYTES 640
static int locks[4], used=1;
static int *lock=&locks[0];
static int *xSemaphoreCreateMutex(void){assert(used<4);return &locks[used++];}
static int xSemaphoreTake(int *l,int wait){(void)wait;assert(l && !*l);*l=1;return 1;}
static void xSemaphoreGive(int *l){assert(*l);*l=0;}
static void mbedtls_platform_zeroize(void *p,size_t n){memset(p,0,n);}
static int64_t now=1;
static int64_t esp_timer_get_time(void){return now;}
static unsigned generation=2,association=3,starts,stops;
static _Atomic bool rtc_wire_busy,usb_client_active,forget_pending,control_trusted=true;
static bool online=true,visible=true;
static const char g_board_device_id[]="p4-010203040506";
static const char *pet_p4_boot_identity(void){return "0000000000000001";}
static int esp_read_mac(uint8_t *b,int kind){assert(kind==ESP_MAC_BASE);for(int i=0;i<6;i++)b[i]=i+1;return 0;}
static bool volume_available(void){return online;}
static bool pet_p4_ota_active(void){return false;}
static unsigned pet_device_wifi_association(void){return association;}
static uint32_t bundle_u32(const uint8_t *p){return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|(p[2]<<8)|p[3];}
static bool pet_app_host_instance_matches(const char *id,const char *token){return visible && !strcmp(id,"journal") && !strcmp(token,"ab-1");}
bool pet_p4_media_radio_matches(const char *s,const char *i){return !strcmp(s,"pc.radio")&&!strcmp(i,"1-1");}
cJSON *pet_p4_media_radio_poll(uint32_t gen,uint32_t assoc){assert(gen==generation&&assoc==association);return NULL;}
cJSON *pet_p4_media_lan(const char *topic,const cJSON *p,const char *s,const char *i,bool cleanup){
  (void)topic;(void)p;(void)s;(void)i;(void)cleanup;return cJSON_Parse("{\"ok\":true}");
}
#include "pet_p4_note_frames.inc"
esp_err_t pet_p4_note_start(const char *owner){if(!note_reserve(owner))return -1;starts++;return 0;}
static int pet_p4_audio_capture_stop_owned(const char *owner){assert(*owner);stops++;return 0;}
#include "pet_p4_lan_component_av.inc"

static bool call(const char *source,const char *instance,const char *topic,const char *payload,
                 unsigned gen,unsigned assoc,int corrupt){
    uint8_t b[LAN_AV_MAX+2];
    memcpy(b,pet_p4_boot_identity(),16);memcpy(b+16,g_board_device_id,15);
    memset(b+31,0,4);b[34]=assoc;
    if(corrupt>=0)b[corrupt]^=1;
    int n=snprintf((char*)b+35,sizeof(b)-35,"{\"source\":\"%s\",\"instance\":\"%s\",\"topic\":\"%s\",\"payload\":%s}",source,instance,topic,payload);
    size_t out=lan_component_av_request(b,35+n,gen);
    assert(out==strlen((char*)b));cJSON *r=cJSON_Parse((char*)b);assert(r);
    bool ok=cJSON_IsTrue(cJSON_GetObjectItem(r,"ok"));cJSON_Delete(r);return ok;
}
#define NOTE "{\"captureOwner\":\"lan-note-a\"}"
#define CALL(op,data) call("app.journal","ab-1",op,data,generation,association,-1)
int main(void){
    assert(CALL("caps","{}"));
    assert(!call("app.other","ab-1","notes/start",NOTE,2,3,-1));
    assert(!call("app.journal","ab-0","notes/start",NOTE,2,3,-1));
    assert(!call("app.journal","ab-1","notes/start",NOTE,1,3,-1));
    assert(!call("app.journal","ab-1","notes/start",NOTE,2,5,-1));
    assert(!call("app.journal","ab-1","notes/start",NOTE,2,3,0));
    assert(!call("app.journal","ab-1","notes/start",NOTE,2,3,18));
    usb_client_active=true;assert(!CALL("notes/start",NOTE));usb_client_active=false;
    assert(!call("pc.radio","1-1","notes/start",NOTE,2,3,-1));
    assert(CALL("notes/start",NOTE));assert(!CALL("notes/start",NOTE));assert(starts==1);
    assert(!CALL("notes/read","{\"captureOwner\":\"lan-note-other\",\"cursor\":0}"));
    for(int i=0;i<10;i++)assert(note_publish("lan-note-a","audio/chunk",cJSON_CreateObject()));
    cJSON *a=pet_p4_note_read("lan-note-a",0),*b=pet_p4_note_read("lan-note-a",0);
    assert(a && b && cJSON_Compare(a,b,true));assert(cJSON_GetArraySize(cJSON_GetObjectItem(a,"frames"))==8);
    cJSON_Delete(a);cJSON_Delete(b);
    assert(!pet_p4_note_read("lan-note-a",9));cJSON_Delete(pet_p4_note_read("lan-note-a",8));
    assert(!pet_p4_note_read("lan-note-a",0));
    while(note_tail-note_ack<NOTE_SLOTS)assert(note_publish("lan-note-a","audio/chunk",cJSON_CreateObject()));
    assert(!note_publish("lan-note-a","audio/chunk",cJSON_CreateObject()));
    assert(!pet_p4_note_read("lan-note-a",8));
    visible=false;lan_component_av_tick();assert(stops==1 && !note_owner[0]);assert(CALL("notes/stop",NOTE));assert(stops==1);
    assert(!note_publish("lan-note-a","audio/chunk",cJSON_CreateObject()));
    visible=true;assert(CALL("notes/start","{\"captureOwner\":\"lan-note-b\"}"));
    now+=LAN_AV_LEASE_US+1;
    assert(!note_publish("lan-note-b","audio/chunk",cJSON_CreateObject()));lan_component_av_tick();
    assert(stops==2);
    assert(CALL("notes/start","{\"captureOwner\":\"lan-note-c\"}"));
    association++;lan_component_av_tick();assert(stops==3);
    assert(CALL("media/begin","{\"sessionId\":\"music-X\"}"));
    assert(!CALL("media/chunk","{\"sessionId\":\"music-Y\"}"));
    assert(CALL("media/chunk","{\"sessionId\":\"music-X\"}"));
    assert(!CALL("media/stop","{}"));online=false;lan_component_av_tick();assert(!av.session[0]);
    return 0;
}
