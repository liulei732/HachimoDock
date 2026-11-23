/* Production device grant worker + catalog admission + registry/scheduler.
 * Substitute only storage, installed files, mutexes, board time and audio. */
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "pet_p4_catalog_profile.h"
#include "pet_p4_component_events.h"
#include "pet_p4_program_registry.h"
#include "pet_p4_program_host_device.h"
#include "pet_p4_resources_device.h"
#include "pet_p4_storage_lane.h"
#include "local_api.h"
#define EXT_RAM_BSS_ATTR
#define portMAX_DELAY 0
typedef int *SemaphoreHandle_t;
static int mutex,ext_depth;
static SemaphoreHandle_t g_background_mutex=&mutex;
static unsigned state_depth,model_depth;
static int state_mutex;
static int *g_state_mutex=&state_mutex;
static int xSemaphoreTake(int *m,int unused){
 (void)unused;assert(!*m);*m=1;if(m==g_state_mutex)state_depth=1;return 1;
}
static void xSemaphoreGive(int *m){assert(*m);*m=0;if(m==g_state_mutex)state_depth=0;}
static void miniapp_runtime_lock(void){assert(!model_depth&&!mutex);model_depth++;}
static void miniapp_runtime_unlock(void){assert(model_depth&&!mutex);model_depth--;}
static void ext_lock(void){assert(!ext_depth);ext_depth++;}
static void ext_unlock(void){assert(ext_depth);ext_depth--;}
static void copy_utf8(char *s,size_t cap,const char *v){snprintf(s,cap,"%s",v);}
#define set_error copy_utf8
static bool utc;
static uint64_t mono;
int64_t esp_timer_get_time(void){return (int64_t)mono*1000;}
static pet_cev_clock_snapshot_t miniapp_utc_snapshot(uint64_t t){return (pet_cev_clock_snapshot_t){.valid=utc,.utc_ms=1700000000000ULL+t};}
bool pet_p4_audio_active(void){return false;}
bool pet_p4_audio_playback_active(void){return false;}
bool pet_p4_audio_conversation_active(void){return false;}
bool pet_p4_audio_music_active(void){return false;}
#include "p4_alarm_nvs_fixture.inc"
typedef struct {char widget_id[48];unsigned slot,resource_state,widget_checksum,buttons_checksum,sprites_checksum;} miniapp_catalog_item_t;
typedef struct {
 bool active,program_background_owned,program_seed_ready,program_suspended,program_needs_start;
 char widget_id[48];struct{char title[64];}view;struct{pet_cev_alarm_set_t alarms;}cext;
 pet_component_program_config_t program;pet_program_snapshot_t program_seed;uint64_t program_content[3];
} miniapp_runtime_t;
static miniapp_catalog_item_t g_catalog[]={{"alpha",1,1,12,22,32},{"beta",2,1,13,23,33}};
static size_t g_catalog_count=2;
static uint32_t g_catalog_sequence=1,g_image_revision=1;
static bool g_catalog_recovery_blocked;
static miniapp_runtime_t g_runtime;
static bool g_ext_notice_pending;
static char g_ext_notice_id[48],g_ext_notice_title[64],g_ext_notice_text[41],g_ext_notice_alarm[16];
static pet_program_registry_t g_background_programs;
static bool g_background_ready,g_component_program_initialized;
static pet_component_program_session_t g_component_program;
static uint32_t g_background_last_us,g_background_max_us;
static pet_storage_lane_t lane;
static bool storage_scope_begin(pet_storage_scope_t *s,bool unused){(void)unused;return pet_storage_scope_begin(&lane,s);}
static void storage_scope_finish(pet_storage_scope_t *s){if(s->active)assert(pet_storage_scope_finish(s));}
static pet_resource_lease_t g_storage_resource_lease;
static bool storage_work_current(void){return true;}
static bool storage_audio_busy(void){return false;}
static bool pet_p4_miniapp_storage_busy(void){return lane.occupied;}
static bool save_app_runtime(void){return true;}
static int catalog_find(const miniapp_catalog_item_t *c,size_t n,const char *id){for(size_t i=0;i<n;i++)if(!strcmp(c[i].widget_id,id))return (int)i;return -1;}
static bool safe_widget_id(const char *id){return catalog_find(g_catalog,2,id)>=0;}
static bool component_parent_enter(void){xSemaphoreTake(&mutex,0);return true;}
static void component_parent_leave(bool *h){if(*h)xSemaphoreGive(&mutex);}
static void component_program_retire_locked(void){assert(!g_component_program_initialized);}
static bool component_program_apply_seed(miniapp_runtime_t *r){(void)r;return true;}
static void refresh_view(miniapp_runtime_t *r){(void)r;}
static void component_program_values(const miniapp_runtime_t *r,int32_t base[8]){(void)r;memset(base,0,8*sizeof(*base));base[1]=2;}
static pet_program_snapshot_t saved_program[2];
static bool pinned[2];
static unsigned disk_reads;
static bool revoke_during_load;
static void physical_press(const char *event_name);
static bool load_catalog_runtime_item(miniapp_catalog_item_t *item,miniapp_runtime_t *r,bool missing,char *err,size_t size){
 assert(!state_depth&&!model_depth&&!mutex&&!missing);(void)err;(void)size;disk_reads++;
 if(revoke_during_load) {
   revoke_during_load=false;
   physical_press("button.encoder.short_press");
 }
 const char *json=
#include "runtime.inc"
 ;
 cJSON *runtime=cJSON_Parse(json);
 bool valid=pet_component_program_parse(runtime,&r->program);cJSON_Delete(runtime);
 strcpy(r->widget_id,item->widget_id);r->active=true;
 r->program_content[0]=item->widget_checksum;r->program_content[1]=item->buttons_checksum;r->program_content[2]=item->sprites_checksum;
 return valid;
}
static int index_of(const char *id){int i=catalog_find(g_catalog,2,id);assert(i>=0);return i;}
pet_program_save_result_t pet_program_persistence_load_cached(const char *id,const pet_program_image_t *image,pet_program_snapshot_t *s){
 *s=saved_program[index_of(id)];if(s->schema)return PET_PROGRAM_SAVE_VALUE;
 s->schema=image->schema;s->cells=image->cells;return PET_PROGRAM_SAVE_EMPTY;
}
pet_program_save_result_t pet_program_persistence_load_with_lease(const char *id,const pet_program_image_t *im,pet_program_snapshot_t *s,pet_resource_lease_t l){
 (void)l;assert(!state_depth);return pet_program_persistence_load_cached(id,im,s);
}
bool pet_program_persistence_enqueue(const char *id,const pet_program_snapshot_t *s){saved_program[index_of(id)]=*s;return true;}
bool pet_app_store_pin_scoped(uint32_t d,const char *id,const void *owner,pet_app_store_pin_t *pin){
 int i=index_of(id);assert(d==PET_PROGRAM_SAVE_DOMAIN);if(pinned[i])return false;
 pinned[i]=true;*pin=(pet_app_store_pin_t){owner,1,(uint8_t)i};return true;
}
bool pet_app_store_unpin(pet_app_store_pin_t *p){assert(pinned[p->slot]);pinned[p->slot]=false;*p=(pet_app_store_pin_t){0};return true;}
static bool local_program_authorized(unsigned,const char *,const uint32_t[3]);
#include "background.inc"
#include "pet_p4_local_alarm.inc"
static bool alarm_restore_saved(int slot){assert(slot>=0);return true;}
static void unlock_state(void *c){(void)c;assert(state_depth==1);state_depth=0;}
static void lock_state(void *c){(void)c;assert(!state_depth);state_depth=1;}
static uintptr_t owner(void *c){(void)c;return 1;}
static void work(void){
 state_depth=1;assert(pet_storage_lane_enter(&lane));pet_local_alarm_run();
 assert(state_depth==1&&pet_storage_lane_leave(&lane));state_depth=0;
}
typedef struct {char screen_page[16];unsigned last_update_ms;bool approval_active,asset_transfer_active;} State;
static State g_state;
static bool idle;
#define pdTRUE 1
static bool volume_worker_idle(void){return idle;}
static bool pet_p4_ota_active(void){return false;}
static bool pet_p4_conversation_active(State *s){(void)s;return false;}
static bool pet_p4_miniapp_storage_lane_enter(void){return pet_storage_lane_enter(&lane);}
static bool pet_p4_miniapp_storage_lane_leave(void){return pet_storage_lane_leave(&lane);}
#define configASSERT assert
static void worker_iteration(void){
#include "local_worker.inc"
}
#define copy_text copy_utf8
static bool pet_p4_state_approval_pending(State *s,unsigned long long ts){(void)ts;return s->approval_active;}
static void physical_press(const char *event_name){
 State *state=&g_state;
 struct {unsigned ts_ms;} e={(unsigned)mono},*event=&e;
 const struct {char action[16];} *binding=NULL;
#include "local_input.inc"
}
static void open_program(unsigned i){
 memset(&g_runtime,0,sizeof g_runtime);assert(load_catalog_runtime_item(&g_catalog[i],&g_runtime,false,NULL,0));
 assert(pet_local_alarm_open());
}
static bool rpc_start(unsigned i){
 miniapp_catalog_item_t *c=&g_catalog[i];uint32_t content[]={c->widget_checksum,c->buttons_checksum,c->sprites_checksum};
 char error[128];uint64_t reg=0;state_depth=1;assert(pet_storage_lane_enter(&lane));
 bool ok=pet_p4_miniapp_background_start(c->widget_id,content,&reg,error,sizeof error);
 assert(state_depth==1&&pet_storage_lane_leave(&lane));state_depth=0;return ok;
}
static void reboot(void){
 for(int i=0;i<2;i++)assert(background_prepare_remove(g_catalog[i].widget_id));
 /* A physical reset recreates the scheduler's monotonic-clock domain. */
 memset(&g_background_programs,0,sizeof g_background_programs);
 g_background_ready=pet_program_registry_init(&g_background_programs,pet_program_host_device_init);
 assert(g_background_ready);
 memset(&g_runtime,0,sizeof g_runtime);memset(&local_edit,0,sizeof local_edit);memset(local_records,0,sizeof local_records);
 program_pending=program_retry=program_retry_after=local_restore=local_restore_retry=local_restore_after=0;
 mono=0;utc=false;pet_local_alarm_boot();
}
int main(void){
 assert(pet_storage_lane_configure(&lane,unlock_state,lock_state,owner,NULL));
 g_background_ready=pet_program_registry_init(&g_background_programs,pet_program_host_device_init);assert(g_background_ready);
 assert(!rpc_start(0)&&!pinned[0]&&!commits);
 open_program(0);assert(!pet_local_alarm_choose(PET_ALARM_ALERT));
 fail_commit=true;assert(pet_local_program_toggle());work();assert(!pinned[0]&&!program_pending);
 fail_commit=false;strcpy(g_state.screen_page,"app");
 physical_press("button.encoder.long_press");assert(!strcmp(g_state.screen_page,PET_ALARM_PAGE));
 physical_press("button.encoder.short_press");
 idle=false;worker_iteration();assert(!commits);
 idle=true;worker_iteration();
 assert(commits==1&&!pinned[0]&&!local_alarm_mode("alpha"));
 memset(&g_runtime,0,sizeof g_runtime);work();assert(!pinned[0]);
 utc=true;work();assert(pinned[0]&&!rpc_start(0));
 for(mono=1;mono<=112;mono++){miniapp_runtime_lock();background_process_locked(mono);miniapp_runtime_unlock();}
 assert(background_checkpoint_one() && saved_program[0].values[0]==1);
 open_program(1);assert(pet_local_program_toggle());work();work();
 assert(pinned[0]&&!pinned[1]&&program_retry && !rpc_start(1));
 reboot();work();assert(!pinned[0]&&!pinned[1]);
 utc=true;work();work();assert(pinned[0]&&!pinned[1]);
 for(mono=1;mono<=112;mono++)background_process_locked(mono);
 assert(background_checkpoint_one()&&saved_program[0].values[0]==2);
 open_program(0);pet_local_alarm_view_t view;assert(pet_local_alarm_view(&view)&&view.program_running);
 physical_press("button.encoder.short_press");
 for(unsigned i=0;i<50;i++)background_process_locked(mono+=100);
 fail_commit=true;work();assert(!pinned[0]&&saved_program[0].values[0]==2);
 assert(local_edit.failed && local_edit.mode==PET_LOCAL_PROGRAM_OFF);
 fail_commit=false;work();assert(local_edit.mode==PET_LOCAL_PROGRAM_OFF);
 mono=40000;work();assert(!local_edit.mode&&!local_edit.failed);
 work();assert(pinned[1]); /* queued consent can use the released lease */
 assert(local_alarm_retire(2,"beta",&g_catalog[1]));assert(!pinned[1]&&!local_alarm_mode("beta"));
 reboot();utc=true;work();assert(!pinned[0]&&!pinned[1]);
 assert(local_alarm_retire(2,"beta",NULL)&&!sizes[2]);
 assert(!pet_local_alarm_work()&&!rpc_start(0));
 /* The same package can make both declarations. Changing either decision
  * leaves the other intact through save and reboot. */
 open_program(0);g_runtime.cext.alarms.count=1;g_runtime.cext.alarms.items[0].background=true;
 physical_press("button.sw2.short_press");work();
 physical_press("button.encoder.short_press");work();work();
 assert(pinned[0]&&local_alarm_mode("alpha")==PET_ALARM_COUNT);
 physical_press("button.sw3.short_press");work();
 assert(pinned[0]&&local_alarm_mode("alpha")==PET_ALARM_OFF);
 physical_press("button.sw1.short_press");work();
 physical_press("button.encoder.short_press");work();
 assert(!pinned[0]&&local_alarm_mode("alpha")==PET_ALARM_ALERT);
 reboot();utc=true;work();assert(!pinned[0]&&!program_pending&&local_alarm_mode("alpha")==PET_ALARM_ALERT);
 /* Release the state mutex for package IO and physically revoke before the
  * prepared runtime is published. No stale lease may escape publication. */
 open_program(1);physical_press("button.encoder.short_press");work();
 assert(program_pending);revoke_during_load=true;work();
 assert(!pinned[1] && !program_pending && local_edit.mode==PET_LOCAL_PROGRAM_OFF);
 work();assert(!local_edit.mode);
 open_program(1);physical_press("button.encoder.short_press");work();assert(program_pending);
 ++g_catalog[1].buttons_checksum;reboot();utc=true;work();
 assert(!pinned[1]&&!program_pending&&!local_records[2].program);
 /* Exercise translation-unit-only alarm helper references in this program fixture. */
 (void)local_alarm_offer;(void)local_alarm_foreground;(void)pet_alarm_button_hit;
}
