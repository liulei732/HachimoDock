/* Mode 25, entered only after the paired AEAD request has opened. */
#include "pet_p4_component_av.h"
#include "pet_p4_audio.h"
#include "pet_p4_app_host.h"

#define LAN_AV_MAX 16382
#define LAN_AV_PREFIX 35
#define LAN_AV_LEASE_US 3500000LL
static _Atomic(SemaphoreHandle_t) av_guard;
static struct {
  char source[52], instance[32], session[64], retired[64];
  uint32_t generation, association;
  int64_t touched, started;
  bool note, stopped;
} av;

static const char *av_string(const cJSON *o,const char *key) {
  cJSON *s=cJSON_GetObjectItemCaseSensitive(o,key);
  return cJSON_IsString(s) ? s->valuestring : "";
}

static bool av_network(uint32_t gen,uint32_t association) {
  if (pet_p4_ota_active() || atomic_load(&rtc_wire_busy)) return false;
  xSemaphoreTake(lock,portMAX_DELAY);
  bool ok=volume_available() && generation==gen && atomic_load(&control_trusted) &&
    !atomic_load(&forget_pending) && !atomic_load(&usb_client_active) &&
    association && association==pet_device_wifi_association();
  xSemaphoreGive(lock);
  return ok;
}

static void av_clear(void) {
  if (!av.session[0]) return;
  if (av.note) {
    pet_p4_audio_capture_stop_owned(av.session);
    pet_p4_note_discard(av.session);
  } else {
    cJSON *p=cJSON_CreateObject();
    if (p && cJSON_AddStringToObject(p,"sessionId",av.session))
      cJSON_Delete(pet_p4_media_lan("media/stop",p,av.source,av.instance,true));
    cJSON_Delete(p);
  }
  strcpy(av.retired,av.session);
  av.session[0]=0;
}

static bool av_view(const char *source,const char *instance) {
  if (!strncmp(source,"pc.",3)) return pet_p4_media_radio_matches(source,instance);
  return !strncmp(source,"app.",4) && pet_app_host_instance_matches(source+4,instance);
}
static void av_expire(void) {
  if (!av.session[0]) return;
  int64_t now=esp_timer_get_time();
  if (now-av.touched>LAN_AV_LEASE_US || !av_network(av.generation,av.association) ||
      !av_view(av.source,av.instance)) av_clear();
  else if (av.note && !av.stopped && now-av.started>=30000000LL) {
    pet_p4_audio_capture_stop_owned(av.session);
    av.stopped=true;
  }
}

static void lan_component_av_tick(void) {
  SemaphoreHandle_t guard=atomic_load(&av_guard);
  if (!guard || xSemaphoreTake(guard,0)!=pdTRUE) return;
  av_expire();
  xSemaphoreGive(guard);
}

static cJSON *av_ok(void) {
  cJSON *result=cJSON_CreateObject();
  if (!result || !cJSON_AddBoolToObject(result,"ok",true)) { cJSON_Delete(result); return NULL; }
  return result;
}

static cJSON *av_command(cJSON *request,uint32_t gen,uint32_t assoc) {
  const char *source=av_string(request,"source"), *instance=av_string(request,"instance");
  const char *topic=av_string(request,"topic");
  cJSON *p=cJSON_GetObjectItemCaseSensitive(request,"payload");
  /* A queued exact stop can arrive after Back already released this owner. */
  if (!av.session[0] && av.note && av.retired[0] && gen==av.generation && assoc==av.association &&
      !strcmp(source,av.source) && !strcmp(instance,av.instance) && !strcmp(av.retired,av_string(p,"captureOwner")) &&
      (!strcmp(topic,"notes/stop") || !strcmp(topic,"notes/close"))) return av_ok();
  if (!strcmp(topic,"media/poll") && !*source && !*instance) return pet_p4_media_radio_poll(gen,assoc);
  if (!cJSON_IsObject(p) || strlen(source)>=sizeof(av.source) ||
      !*instance || strlen(instance)>=sizeof(av.instance) ||
      !av_view(source,instance)) return NULL;
  if (!strcmp(topic,"caps")) {
    cJSON *r=av_ok();
    if (r) {
      cJSON_AddNumberToObject(r,"version",1);
      cJSON_AddNumberToObject(r,"maxBody",LAN_AV_MAX);
      cJSON_AddNumberToObject(r,"maxCaptureBytes",960000);
    }
    return r;
  }
  if (!av.session[0] && !strncmp(source,"pc.",3) && !strcmp(topic,"media/query"))
    return pet_p4_media_lan(topic,p,source,instance,false);
  bool note=!strncmp(topic,"notes/",6);
  if (note && strncmp(source,"app.",4)) return NULL;
  if (!strcmp(topic,"media/library") || !strcmp(topic,"media/notice")) {
    if (av.session[0] && (av.note || strcmp(av.source,source) || strcmp(av.instance,instance))) return NULL;
    return pet_p4_media_lan(topic,p,source,instance,false);
  }
  const char *session=av_string(p,note?"captureOwner":"sessionId");
  if (!*session || strlen(session)>=sizeof(av.session) ||
      (note ? strncmp(session,PET_NOTE_PREFIX,sizeof(PET_NOTE_PREFIX)-1) : strncmp(session,"music-",6))) return NULL;
  bool begin=!strcmp(topic,"notes/start") || !strcmp(topic,"media/begin");
  bool matches=av.note==note && av.generation==gen && av.association==assoc &&
    !strcmp(av.session,session) && !strcmp(source,av.source) && !strcmp(instance,av.instance);
  if (begin ? (av.session[0] || !strcmp(av.retired,session)) : !matches) return NULL;
  cJSON *reply=NULL;
  if (!strcmp(topic,"notes/start")) {
    if (pet_p4_note_start(session)==ESP_OK) reply=av_ok();
    if (!reply) {pet_p4_audio_capture_stop_owned(session); pet_p4_note_discard(session);}
  } else if (!strcmp(topic,"notes/read")) {
    cJSON *c=cJSON_GetObjectItemCaseSensitive(p,"cursor");
    if (cJSON_IsNumber(c) && c->valueint>=0 && c->valueint<=1600 && c->valuedouble==c->valueint)
      reply=pet_p4_note_read(session,(unsigned)c->valueint);
  } else if (!strcmp(topic,"notes/stop")) {
    if (!av.stopped) pet_p4_audio_capture_stop_owned(session);
    av.stopped=true; reply=av_ok();
  } else if (!strcmp(topic,"notes/close")) {
    av_clear(); return av_ok();
  } else if (!note) {
    static const char *const allowed[]={"media/begin","media/chunk","media/query","media/end",
      "media/stop","media/control","media/metadata","media/lyrics","media/cover","media/library"};
    for (size_t i=0;i<sizeof(allowed)/sizeof(*allowed);i++)
      if (!strcmp(topic,allowed[i])) {reply=pet_p4_media_lan(topic,p,source,instance,false);break;}
  }
  if (reply && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply,"ok"))) {
    if (begin) {
      strcpy(av.source,source); strcpy(av.instance,instance); strcpy(av.session,session);
      av.note=note; av.stopped=false; av.generation=gen; av.association=assoc; av.started=esp_timer_get_time();
    }
    av.touched=esp_timer_get_time();
    if (!strcmp(topic,"media/stop")) { strcpy(av.retired,av.session); av.session[0]=0; }
  }
  return reply;
}

static size_t lan_component_av_request(uint8_t *body,size_t size,uint32_t gen) {
  cJSON *reply=NULL, *request=NULL;
  const char *boot=pet_p4_boot_identity();
  if (size>LAN_AV_PREFIX && size<=LAN_AV_MAX && boot && strlen(boot)==16 &&
      !memcmp(boot,body,16) && pet_json_source_text_limit_bytes(body+LAN_AV_PREFIX,size-LAN_AV_PREFIX,8)) {
    uint8_t mac[6]; char board[16];
    uint32_t assoc=bundle_u32(body+31);
    if (esp_read_mac(mac,ESP_MAC_BASE)==ESP_OK) {
      snprintf(board,sizeof(board),"p4-%02x%02x%02x%02x%02x%02x",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
      if (!memcmp(board,body+16,15) && av_network(gen,assoc)) {
        body[size]=0;
        request=cJSON_ParseWithOpts((const char *)body+LAN_AV_PREFIX,NULL,true);
        SemaphoreHandle_t guard=atomic_load(&av_guard);
        if (!guard) { guard=xSemaphoreCreateMutex(); atomic_store(&av_guard,guard); }
        if (guard && request && xSemaphoreTake(guard,portMAX_DELAY)==pdTRUE) {
          av_expire();
          if (av_network(gen,assoc)) reply=av_command(request,gen,assoc);
          xSemaphoreGive(guard);
        }
      }
    }
  }
  cJSON_Delete(request);
  mbedtls_platform_zeroize(body,size);
  if (!reply || !cJSON_PrintPreallocated(reply,(char *)body,LAN_AV_MAX,false))
    strcpy((char *)body,"{\"ok\":false}");
  cJSON_Delete(reply);
  return strlen((char *)body);
}
