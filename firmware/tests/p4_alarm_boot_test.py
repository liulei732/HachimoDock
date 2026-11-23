"""Extract production boot grant + periodic delivery; real storage-lane/UTC core.

Disk/package APIs are injected at their boundary to force load/checksum/OOM and
concurrent lifecycle failures. They assert IO never holds state/model/ext locks.
"""
from p4_alarm_utc_test import ROOT, _run_utc_test


def test_coldboot_hidden_alarm_grant_atomicity_and_no_hidden_execution(tmp_path):
    main = ROOT / 'main'
    public_header = (main/'pet_p4_miniapp.h').read_text()
    permission_api = public_header[public_header.index('enum { PET_ALARM_OFF'):public_header.index('/* Existing protocol storage worker only')]
    prefix = permission_api + r'''
#include "pet_p4_storage_lane.h"
#include "pet_p4_catalog_profile.h"
typedef struct {char widget_id[48];char title[64];uint8_t slot,resource_state;uint32_t widget_checksum;} miniapp_catalog_item_t;
typedef enum {PET_APP_STORE_EMPTY,PET_APP_STORE_VALUE,PET_APP_STORE_BUSY,PET_APP_STORE_UNAVAILABLE} pet_app_store_status_t;
static miniapp_catalog_item_t g_catalog[PET_P4_MINIAPP_CATALOG_MAX];static size_t g_catalog_count;
static uint32_t g_catalog_sequence;static bool g_catalog_recovery_blocked;
static pet_storage_lane_t lane;
static unsigned g_storage_resource_lease=1,package_reads,disk_reads;
static unsigned bad_package,bad_save,load_error,change_catalog,disable_during_load,create_live,fail_later;
static bool storage_audio_busy(void){return false;}
static bool storage_work_current(void){return true;}
static bool storage_scope_begin(pet_storage_scope_t*s,bool activation){(void)activation;return pet_storage_scope_begin(&lane,s);}
static void storage_scope_finish(pet_storage_scope_t*s){if(s->active)assert(pet_storage_scope_finish(s));}
static bool pet_p4_miniapp_storage_busy(void){return lane.occupied;}
static int catalog_find(const miniapp_catalog_item_t *c,size_t count,const char *id){
 for(size_t i=0;i<count;i++)if(!strcmp(c[i].widget_id,id))return (int)i;
 return -1;
}
''' + (ROOT/'tests/p4_alarm_nvs_fixture.inc').read_text() + r'''
#define PET_P4_SYSTEM_WIFI 1
enum { PET_WIFI_TZ_AUTO=841 };
static bool network_clock;
static int32_t pet_wifi_clock_timezone(void){return PET_WIFI_TZ_AUTO;}
static void pet_wifi_clock_note_offset(int32_t offset){(void)offset;}
static bool pet_wifi_clock_read(uint64_t now,uint64_t *utc,int32_t *offset,uint64_t *age){
 if(!network_clock)return false;
 *utc=1700000060000ULL+now;*offset=0;*age=0;return true;
}
static bool safe_widget_id(const char*s){return s&&s[0];}
static pet_app_store_status_t pet_app_store_load_with_lease(const char*,char*,unsigned);
static bool load_catalog_runtime_item(miniapp_catalog_item_t*,miniapp_runtime_t*,bool,char*,size_t);
static bool restore_app_runtime_buffer(miniapp_runtime_t*,bool,char*);
'''
    body = r'''
static void unlocked(void){
 assert(!state_depth&&!model_depth);
 SemaphoreHandle_t m=atomic_load(&g_ext_lock);if(m){assert(pthread_mutex_trylock(m)==0);pthread_mutex_unlock(m);}
}
static pet_cev_alarm_set_t boot_config(void){
 pet_cev_alarm_set_t a={0};a.count=1;strcpy(a.items[0].id,"timer");strcpy(a.items[0].text,"wake");strcpy(a.items[0].action,"NEVER_HIDDEN_ACTION");a.items[0].at_var=0;a.items[0].background=true;return a;
}
static pet_app_store_status_t pet_app_store_load_with_lease(const char*id,char*out,unsigned lease){
 unlocked();assert(lease==1);disk_reads++;
 if(load_error)return PET_APP_STORE_UNAVAILABLE;
 if(!strcmp(id,"no-save"))return PET_APP_STORE_EMPTY;
 if(!strcmp(id,"no-alarms")){strcpy(out,"{\"version\":1,\"vars\":{}}");return PET_APP_STORE_VALUE;}
 snprintf(out,PET_APP_SAVE_BYTES,"{\"version\":1,\"vars\":{},\"alarms\":\"timer:%s\"}",!strcmp(id,"cancelled")?"0:v60000:c":"1700000060000:u:v60000");
 if(!strcmp(id,"break-count"))strcpy(out,"{\"version\":1,\"vars\":{},\"alarms\":\"stretch:1700000660000:u\"}");
 if(bad_save)strcpy(out,"{bad");
 if(change_catalog)g_catalog_sequence++;
 if(disable_during_load)pet_p4_miniapp_alarm_policy(false);
 if(create_live){ext_lock();ext_alarm_entry_t*e=ext_alarm_entry(id,true);e->alarms=boot_config();pet_cev_alarms_cancel(&e->alarms,0);ext_unlock();}
 return PET_APP_STORE_VALUE;
}
static bool load_catalog_runtime_item(miniapp_catalog_item_t*i,miniapp_runtime_t*r,bool missing,char*err,size_t size){
 unlocked();assert(!missing&&err&&size);package_reads++;if(bad_package||(fail_later&&package_reads==2))return false;
 assert(i->widget_checksum==123);strcpy(r->widget_id,i->widget_id);strcpy(r->view.title,"Saved hidden component");r->cext.events=true;r->cext.alarms=boot_config();r->save_version=1;
 if(!strcmp(i->widget_id,"break-count")) {
   pet_cev_alarm_t *a=&r->cext.alarms.items[0];
   strcpy(a->id,"stretch");strcpy(a->text,"Stretch");a->at_var=-1;a->every_ms=60000;
 }
 return true;
}
static bool restore_app_runtime_buffer(miniapp_runtime_t*r,bool cached,char*json){
 unlocked();assert(cached);cJSON*j=cJSON_Parse(json);bool ok=j&&miniapp_ext_restore_text(r,cJSON_GetObjectItemCaseSensitive(j,"alarms"));cJSON_Delete(j);return ok;
}
#include "pet_p4_miniapp_alarm_boot.inc"
static void state_unlock(void*ctx){(void)ctx;assert(state_depth==1);state_depth=0;}
static void state_lock(void*ctx){(void)ctx;assert(!state_depth);state_depth=1;}
static uintptr_t owner(void*ctx){(void)ctx;return 1;}
static bool grant(bool value){
 state_depth=1;assert(pet_storage_lane_enter(&lane));bool ok=pet_p4_miniapp_alarm_policy_prepare(value);
 assert(state_depth==1&&pet_storage_lane_leave(&lane));state_depth=0;assert(!live_alloc);return ok;
}
static void boot_reset(void){
 memset(g_ext_alarms,0,sizeof g_ext_alarms);memset(&g_runtime,0,sizeof g_runtime);g_runtime.active=true;strcpy(g_runtime.widget_id,"other-active");
 memset(g_catalog,0,sizeof g_catalog);g_catalog_count=5;
 const char*ids[]={"other-active","hidden","cancelled","no-save","no-alarms"};
 for(unsigned i=0;i<5;i++){strcpy(g_catalog[i].widget_id,ids[i]);g_catalog[i].resource_state=1;g_catalog[i].slot=i;g_catalog[i].widget_checksum=123;}
 g_wall_valid=false;g_clock_generation=0;mono=0;g_ext_notice_pending=false;g_ext_background_notify=false;g_ext_alarm_policy_revision=0;g_ext_alarm_serial=0;
 bad_package=bad_save=load_error=change_catalog=disable_during_load=create_live=fail_later=package_reads=disk_reads=0;
}
static void boot_lifecycle(void){
 assert(pet_storage_lane_configure(&lane,state_unlock,state_lock,owner,NULL));
 boot_reset();assert(grant(true)&&g_ext_background_notify&&package_reads==2);
 assert(ext_alarm_entry("hidden",false)&&ext_alarm_entry("cancelled",false));
 assert(!ext_alarm_entry("no-save",false)&&!ext_alarm_entry("no-alarms",false)&&!ext_alarm_entry("other-active",false));
 assert(ext_alarm_entry("hidden",false)->alarms.items[0].waiting_for_clock);
 char title[64],notice[PET_CEV_ALARM_TEXT_MAX];mono=900000;miniapp_ext_process(mono,false);
 assert(!pet_p4_miniapp_alarm_notification(title,notice)&&!dispatches);
 cJSON*c=cJSON_Parse("{\"unixSeconds\":1700000060,\"utcOffsetMinutes\":480}");assert(pet_p4_miniapp_clock_sync(c,mono));cJSON_Delete(c);
 miniapp_ext_process(mono,false);assert(pet_p4_miniapp_alarm_notification(title,notice)&&!strcmp(notice,"wake")&&!dispatches);
 assert(!strcmp(g_runtime.widget_id,"other-active")&&ext_alarm_entry("cancelled",false)->alarms.items[0].cancelled);
 unsigned read_count=disk_reads;assert(grant(true)&&disk_reads==read_count+2); /* only absent/no-alarm records revisited */
 assert(grant(false)&&!g_ext_background_notify&&!g_ext_notice_pending);
 boot_reset();oom=true;assert(!grant(true)&&!g_ext_background_notify&&!ext_alarm_entry("hidden",false));oom=false;
 for(unsigned failure=0;failure<4;failure++){
  boot_reset();bad_package=failure==0;bad_save=failure==1;load_error=failure==2;change_catalog=failure==3;
  assert(!grant(true)&&!g_ext_background_notify&&!ext_alarm_entry("hidden",false));
 }
 boot_reset();fail_later=1;assert(!grant(true)&&package_reads==2&&!g_ext_background_notify&&!ext_alarm_entry("hidden",false)); /* no prefix commit */
 boot_reset();pet_p4_miniapp_alarm_policy(true);bad_save=1;assert(!grant(true)&&g_ext_background_notify); /* preserve old grant */
 boot_reset();disable_during_load=1;assert(!grant(true)&&!g_ext_background_notify&&!ext_alarm_entry("hidden",false));
 boot_reset();create_live=1;assert(grant(true)&&ext_alarm_entry("hidden",false)->alarms.items[0].cancelled); /* fresh RAM wins */
 boot_reset();g_catalog_count=1;strcpy(g_catalog[0].widget_id,"hidden");g_catalog[0].resource_state=3;assert(grant(true)&&!disk_reads&&!ext_alarm_entry("hidden",false)); /* disabled/damaged skipped */
 boot_reset();g_ext_alarm_serial=UINT64_MAX;assert(!grant(true)&&!g_ext_background_notify&&!ext_alarm_entry("hidden",false));
 boot_reset();for(unsigned i=0;i<PET_CEV_COMPONENTS;i++){char id[48];snprintf(id,sizeof id,"retained-%u",i);assert(ext_alarm_entry(id,true));}
 char last[48];snprintf(last,sizeof last,"retained-%u",(unsigned)PET_CEV_COMPONENTS-1);
 assert(!grant(true)&&!g_ext_background_notify&&!ext_alarm_entry("hidden",false)&&ext_alarm_entry(last,false));
 assert(!updates&&!dispatches); /* boot prepare never writes a default/save, dispatches, or changes active selection */
 boot_reset();memset(g_ext_alarms,0,sizeof g_ext_alarms);g_catalog_count=0;memset(&g_runtime,0,sizeof g_runtime);
}
'''
    body += (ROOT/'tests/p4_local_alarm_cases.inc').read_text()
    _run_utc_test(tmp_path, prefix, body, 'boot_lifecycle();local_lifecycle();',
                  (ROOT/'main/pet_p4_storage_lane.c',), local_grants=True)


def test_policy_protocol_waits_for_worker_prepare_before_success_ack():
    source=(ROOT/'main/pet_p4_protocol.c').read_text()
    policy=source[source.index('strcmp(topic, "component/alarm_policy")'):source.index('strcmp(topic, "ui/conversation")',source.index('strcmp(topic, "component/alarm_policy")'))]
    assert 'pet_p4_miniapp_alarm_policy_prepare(cJSON_IsTrue(background))' in policy
    assert policy.index('"alarm_restore_unavailable"') < policy.index('send_protocol_ack_if_requested')
    helper=(ROOT/'main/pet_p4_miniapp_alarm_boot.inc').read_text()
    for forbidden in ('pet_p4_miniapp_dispatch_action(', 'miniapp_ext_publish(',
                      'pet_program_registry_start(', 'pet_app_store_update('):
        assert forbidden not in helper
    assert 'storage.may_unlock' in helper


def test_device_alarm_page_physical_touch_and_modal_ownership(tmp_path):
    import subprocess
    main = ROOT / 'main'
    def section(path, begin, end):
        text = (main/path).read_text()
        start = text.index(begin)
        return text[start:text.index(end, start)]

    api = section('pet_p4_miniapp.h','enum { PET_ALARM_OFF','bool pet_local_alarm_view')
    buttons = section('pet_p4_input.c','  if (state && !strcmp(state->screen_page, PET_ALARM_PAGE))','#endif')
    touch = section('pet_p4_touch.c','  if (state && !strcmp(state->screen_page, PET_ALARM_PAGE))','#endif')
    c = tmp_path/'ui.c'
    c.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
''' + api + r'''
typedef struct {char screen_page[16]; unsigned last_update_ms; bool modal;} State;
typedef struct {unsigned ts_ms;int x,y,gesture;} Event;
typedef struct {const char *action;} Binding;
enum {PET_P4_TOUCH_TAP=1};
static unsigned choice,calls;static bool eligible;
static bool pet_p4_state_approval_pending(State *s,unsigned now){(void)now;return s->modal;}
static bool pet_local_alarm_open(void){return eligible;}
static bool pet_local_program_toggle(void){return false;}
static bool pet_local_alarm_choose(unsigned mode){choice=mode;calls++;return true;}
static void copy_text(char *o,size_t n,const char *s){snprintf(o,n,"%s",s);}
static void press(State *state,const char *event_name,const Binding *binding){
 Event e={.ts_ms=11};Event *event=&e;
''' + buttons + r'''
}
static void tap(State *state,int x,int y,int gesture){
 Event e={.ts_ms=12,.x=x,.y=y,.gesture=gesture};Event *event=&e;
''' + touch + r'''
}
int main(void){
 State s={.screen_page="app"};
 const char *open="button.encoder.long_press";
 press(&s,open,NULL);assert(!strcmp(s.screen_page,"app"));
 eligible=true;Binding bound={"page_back"};
 press(&s,open,&bound);assert(!strcmp(s.screen_page,"app"));
 s.modal=true;press(&s,open,NULL);assert(!strcmp(s.screen_page,"app"));
 s.modal=false;press(&s,open,NULL);assert(!strcmp(s.screen_page,PET_ALARM_PAGE));
 press(&s,"button.sw1.short_press",NULL);assert(choice==PET_ALARM_ALERT);
 press(&s,"button.sw2.short_press",NULL);assert(choice==PET_ALARM_COUNT);
 press(&s,"button.sw3.short_press",NULL);assert(choice==PET_ALARM_OFF);
 s.modal=true;tap(&s,100,280,1);press(&s,"button.sw1.short_press",NULL);assert(calls==3);
 s.modal=false;tap(&s,23,280,1);tap(&s,100,265,1);tap(&s,100,434,1);tap(&s,100,280,2);
 assert(calls==3);
 tap(&s,24,266,1);assert(choice==PET_ALARM_ALERT);
 tap(&s,24,322,1);assert(choice==PET_ALARM_COUNT);
 tap(&s,615,433,1);assert(choice==PET_ALARM_OFF && calls==6);
 tap(&s,48,32,1);assert(!strcmp(s.screen_page,"app"));
 assert(pet_alarm_button_hit(260,24)&&!pet_alarm_button_hit(435,24));
 press(&s,open,NULL);press(&s,open,NULL);assert(!strcmp(s.screen_page,"app"));
}
''')
    binary=tmp_path/'ui'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
