"""Compile production catalog serialization/CAS/seed code against real cJSON.

Only flash/NVS, scheduling and package materialization are local adapters. The
membership decision, CAS validation, physical projection, writer/check/loader,
and cursor code are the production functions, not duplicate policy models.
"""
from pathlib import Path
import json
import re
import subprocess
import zlib
import pytest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'main/pet_p4_miniapp.c'

def function(code, name):
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', lambda m: ' ' * len(m[0]), code, flags=re.S)
    for match in re.finditer(r'(?m)^[\w *]+\b' + re.escape(name) + r'\([^;{}]*\)\s*\{', masked):
        start = match.start(); brace = masked.index('{', start); depth = 1; end = brace + 1
        while depth:
            depth += (masked[end] == '{') - (masked[end] == '}'); end += 1
        return code[start:end] + '\n'
    raise AssertionError(name)

@pytest.fixture(scope='module', params=[0, 1], ids=['atomic-rename', 'sealed-inplace'])
def binary(request, tmp_path_factory):
    code = SOURCE.read_text().replace('__attribute__((optimize("Oz"))) ', '')
    out = tmp_path_factory.mktemp('catalog-host')
    record = code[code.index('typedef struct {\n  char widget_id[PET_P4_MINIAPP_WIDGET_ID_MAX];\n  char title[64];'):code.index('static const char *TAG =', code.index('} miniapp_catalog_item_t;'))]
    cursor = code[code.index('typedef struct {uint32_t magic,sequence;char id[48];} catalog_cursor_t;'):code.index('static bool write_catalog_snapshot_at(')]
    bundle = json.dumps({'version': 1, 'components': [{'id': 'seed-one', 'resourcePackage': True, 'widget': {}, 'buttons': [], 'spriteFiles': [], 'spritesChecksum': '00000000'}, {'id': 'seed-two', 'resourcePackage': True, 'widget': {}, 'buttons': [], 'spriteFiles': [], 'spritesChecksum': '00000000'}]}, separators=(',', ':')).encode() + b'\0'
    compressed = zlib.compress(bundle)
    prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <zlib.h>
#include "cJSON.h"
#include "pet_p4_component_metadata.h"
#include "pet_p4_builtin_index.h"
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define heap_caps_malloc(n,c) (assert((c)==3),malloc(n))
#define heap_caps_calloc(n,s,c) (assert((c)==3),calloc(n,s))
#include "pet_p4_center_layout.h"
static pet_center_filter_t g_center_filter = PET_CENTER_ALL;
#define PET_P4_MINIAPP_WIDGET_ID_MAX 48
#define PET_COMPONENT_BACKGROUND_ENABLED 1
#define PET_COMPONENT_PROGRAM_ENABLED 1
#define PET_COMPONENT_PROGRAM_EXECUTION_REVIEWED 1
#define PET_COMPONENT_PROGRAM_PARENT_UNIFIED 1
#define PET_P4_MINIAPP_BACKGROUND_MAX 4
typedef struct {char widget_id[48];} pet_p4_miniapp_background_status_t;
static char registered_program[48];
static size_t pet_p4_miniapp_background_list(pet_p4_miniapp_background_status_t *out){if(!registered_program[0])return 0;strcpy(out[0].widget_id,registered_program);return 1;}
#include "pet_p4_catalog_profile.h"
#define MINIAPP_CATALOG_GENERATION_COUNT 2
#define MINIAPP_PACKAGE_GENERATION_COUNT 2
#define center_thumb_catalog(g,items,count,committed) ((void)0)
#define MINIAPP_CATALOG_VERSION 3
#define MINIAPP_CATALOG_JSON_MAX (32 * 1024)
#define MINIAPP_WIDGET_JSON_MAX 20000
#define MINIAPP_BUTTONS_JSON_MAX 4000
#define PET_P4_BUILD_ID "host-next-build"
#define MINIAPP_BUILTIN_MARKER_PATH "marker"
#define MINIAPP_BUILTIN_MARKER_TMP_PATH "marker.tmp"
#define MINIAPP_LAST_ID_PATH "legacy-active"
#define ESP_ERR_NVS_NOT_FOUND -6
#define PET_WIDGET_TRACE_SCOPE(...) ((void)0)
#define ESP_LOGW(tag,...) ((void)fprintf(stderr,__VA_ARGS__))
#define ESP_LOGI(tag,...) ((void)fprintf(stderr,__VA_ARGS__))
#define miniapp_runtime_lock() ((void)0)
#define miniapp_runtime_unlock() ((void)0)
#define pdMS_TO_TICKS(n) (n)
#define vTaskDelay(n) ((void)(n))
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE -2
#define ESP_ERR_INVALID_SIZE -3
#define ESP_ERR_NO_MEM -4
#define ESP_ERR_INVALID_RESPONSE -5
#define NVS_READONLY 0
#define NVS_READWRITE 1
typedef int nvs_handle_t;
static unsigned char nvs_blob[64],selection_blob[64];static size_t nvs_size,selection_size;static int nvs_writes;static bool nvs_fail;
static esp_err_t nvs_open(const char *ns,int mode,nvs_handle_t *h){(void)ns;(void)mode;*h=1;return nvs_fail?-1:0;}
static esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *n){(void)h;bool cursor=!strcmp(key,"cursor");size_t size=cursor?nvs_size:selection_size;if(!size)return ESP_ERR_NVS_NOT_FOUND;assert(*n>=size);memcpy(out,cursor?nvs_blob:selection_blob,size);*n=size;return 0;}
static esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *in,size_t n){(void)h;assert(n<=sizeof(nvs_blob));bool cursor=!strcmp(key,"cursor");memcpy(cursor?nvs_blob:selection_blob,in,n);if(cursor)nvs_size=n;else selection_size=n;nvs_writes++;return 0;}
static esp_err_t nvs_commit(nvs_handle_t h){(void)h;return 0;}
static void nvs_close(nvs_handle_t h){(void)h;}
static bool io_fail,audio_busy,owner_current=true,require_unlocked;
static bool write_lane_unlocked(void);
static unsigned writes,materialized;static int fail_package=-1;
static void set_error(char *s,size_t n,const char *fmt,...){va_list a;va_start(a,fmt);vsnprintf(s,n,fmt,a);va_end(a);}
static const char *json_string(const cJSON *j,const char *k){const cJSON *v=cJSON_GetObjectItemCaseSensitive(j,k);return cJSON_IsString(v)?v->valuestring:"";}
static bool write_bytes(const char *path,const void *s,size_t n){assert(!require_unlocked||write_lane_unlocked());writes++;if(io_fail)return false;FILE *f=fopen(path,"wb");if(!f)return false;bool ok=fwrite(s,1,n,f)==n;ok=fclose(f)==0&&ok;return ok;}
static bool write_file_atomic(const char *tmp,const char *path,const void *s,size_t n){if(!write_bytes(tmp,s,n))return false;return rename(tmp,path)==0;}
static bool write_catalog_in_place(const char *path,const char *s,size_t n,bool *changed){*changed=true;return write_bytes(path,s,n);}
static bool write_catalog_atomic(const char *tmp,const char *path,const char *s,size_t n,bool *changed){*changed=true;return write_file_atomic(tmp,path,s,n);}
static bool read_file(const char *path,char *s,size_t n){FILE *f=fopen(path,"rb");if(!f)return false;size_t got=fread(s,1,n-1,f);bool ok=!ferror(f)&&feof(f);s[got]=0;fclose(f);return ok;}
static bool read_json_file(const char *p,char *s,size_t n){return read_file(p,s,n);}
static void catalog_snapshot_path(uint8_t gen,bool tmp,char *p,size_t n){snprintf(p,n,"catalog.%u%s",gen,tmp?".tmp":"");}
'''
    body = record + r'''
static miniapp_catalog_item_t g_catalog[PET_P4_MINIAPP_CATALOG_MAX];
static size_t g_catalog_count,g_catalog_selected;
static int g_catalog_file_generation=-1;static uint32_t g_catalog_sequence;
static bool g_catalog_seed_pending,g_catalog_recovery_blocked,g_builtin_sync_in_progress;
static char g_storage_failure[128];
typedef bool (*pet_p4_miniapp_catalog_cursor_schedule_fn)(uint64_t,void *);
static pet_p4_miniapp_catalog_cursor_schedule_fn g_catalog_cursor_schedule;
static void *g_catalog_cursor_context;static uint64_t g_catalog_cursor_serial,queued_token;
static uint32_t g_catalog_cursor_sequence;
static bool g_catalog_cursor_dirty;static uint64_t g_catalog_cursor_retry_ms,mono_us;
static uint64_t esp_timer_get_time(void){return mono_us;}
static unsigned queued_count;
static bool queue_ok=true;
static bool enqueue_cursor(uint64_t token,void *ctx){(void)ctx;if(!queue_ok)return false;queued_token=token;queued_count++;return true;}

static struct{bool active;char widget_id[48];} g_runtime,g_staging;
'''
    for name in ['safe_widget_id', 'copy_utf8', 'parse_checksum_hex', 'miniapp_checksum', 'catalog_find', 'catalog_id_visible', 'catalog_item_visible', 'catalog_entry_visible', 'catalog_entry_matches', 'catalog_filtered_index', 'catalog_visible_index','catalog_visible_preferred_in','catalog_visible_preferred']:
        body += function(code, name)
    body += '#define MINIAPP_CATALOG_CHECK_KEY "\\\"check\\\":\\\""\n'
    for name in ['catalog_hex_digit', 'catalog_check_digits', 'catalog_check_value', 'catalog_check_seal', 'catalog_check_verify']:
        body += function(code, name)
    selection=(ROOT/'main/pet_p4_selection.c').read_text()
    body += 'typedef struct {uint32_t magic,sequence;char id[48];} selection_t;\n#define SELECTION_MAGIC 0x50534c31u\n'
    for name in ['safe_id','pet_selection_read','pet_selection_write']:
        body += function(selection,name)
    body += cursor
    # Retain firmware path, checksum and exact-length validation on cold reads.
    # Only the mount point is redirected to the test's private working directory.
    header = (ROOT/'main/pet_p4_miniapp.h').read_text()
    entry_end = header.index('} pet_p4_miniapp_catalog_entry_t;') + len('} pet_p4_miniapp_catalog_entry_t;')
    body += header[header.rindex('typedef struct {', 0, entry_end):entry_end] + '\n'
    body += re.search(r'^#define PET_CENTER_THUMB_BYTES .*$', header, re.M)[0] + '\n'
    body += r'''
static FILE *open_test_thumb(const char *path,const char *mode){
 assert(!strncmp(path,"/spiffs/",8));return fopen(path+8,mode);
}
#define fopen open_test_thumb
'''
    sidecar = (ROOT/'main/pet_p4_center_thumb.inc').read_text()
    for name in ['center_thumb_path', 'center_thumb_hash', 'pet_p4_miniapp_center_read_thumb']:
        body += function(sidecar,name)
    body += '#undef fopen\n'
    for name in ['write_catalog_snapshot_at', 'persist_catalog_snapshot', 'load_catalog_snapshot']:
        body += function(code, name)
    # Use the actual native storage lane state machine; only its resources
    # lease/admission and scheduling adapters are local to this memory host.
    body += r'''
#include "pet_p4_storage_lane.h"
static pet_storage_lane_t lane;
#define g_storage_lane lane
static bool pet_p4_miniapp_storage_busy(void){return lane.occupied;}
static uintptr_t lane_owner(void *p){(void)p;return 1;}
static unsigned unlocks,relocks;
static void lane_unlock(void *p){(void)p;unlocks++;}
static void lane_lock(void *p){(void)p;relocks++;}
static bool write_lane_unlocked(void){return lane.scope_owner&&lane.scope_owner->unlocked;}
static bool storage_scope_begin(pet_storage_scope_t *s,bool unused){(void)unused;return pet_storage_scope_begin(&lane,s);}
static void storage_scope_finish(pet_storage_scope_t *s){if(s->active)assert(pet_storage_scope_finish(s));}
static bool storage_work_current(void){return owner_current;}
static bool storage_audio_busy(void){return audio_busy;}
'''
    for name in ['catalog_select_id_durable','pet_p4_miniapp_catalog_cursor_configure','catalog_select_id_queued','catalog_cursor_retry','pet_p4_miniapp_catalog_cursor_run','pet_p4_miniapp_catalog_select_durable','pet_p4_miniapp_catalog_select', 'pet_p4_miniapp_catalog_move']:
        body += function(code, name)
    body += function(code,'catalog_restore_navigation')
    body += function(code,'catalog_latest_snapshot')
    body += function(code,'recover_catalog_items')
    body += function(code,'pet_p4_miniapp_catalog_recovery_blocked')
    body += r'''
#undef PET_P4_EVENTS
#define PET_P4_EVENTS 1
static bool refuse_retirement;
static unsigned retire_calls, forgotten;
static bool local_alarm_retire(unsigned slot,const char *id,const miniapp_catalog_item_t *replacement){
 int i=catalog_find(g_catalog,g_catalog_count,id);
 assert(i>=0 && g_catalog[i].slot==slot && !replacement);
 assert(!require_unlocked || write_lane_unlocked());
 retire_calls++;return !refuse_retirement;
}
static void miniapp_ext_forget_events(const char *id){
 assert(catalog_find(g_catalog,g_catalog_count,id)>=0 && retire_calls>forgotten);forgotten++;
}
'''
    body += (ROOT / 'main/pet_p4_catalog_admin.inc').read_text()
    body += r'''
typedef struct {char board_device_id[64];unsigned last_update_ms;} pet_p4_runtime_state_t;
typedef void (*pet_p4_send_line_fn)(const char *,void *);
static const char *pet_p4_boot_identity(void){return "0123456789abcdef";}
#define copy_text copy_utf8
static cJSON *reply;static char reply_topic[64];
static void send_topic_payload(pet_p4_send_line_fn send,void *ctx,const char *topic,cJSON *payload){(void)send;(void)ctx;cJSON_Delete(reply);reply=payload;copy_utf8(reply_topic,sizeof(reply_topic),topic);}
'''
    body += function((ROOT/'main/pet_p4_protocol.c').read_text(),'send_catalog_admin')
    body += '\nconst unsigned char pet_p4_builtin_components_zlib[]={' + ','.join(map(str, compressed)) + '};\n'
    body += f'const size_t pet_p4_builtin_components_zlib_size={len(compressed)},pet_p4_builtin_components_json_size={len(bundle)};\n'
    body += r'''
typedef unsigned char mz_uint8;typedef int tinfl_decompressor;typedef int tinfl_status;
#define TINFL_STATUS_DONE 0
#define TINFL_FLAG_PARSE_ZLIB_HEADER 1
#define TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF 2
#define tinfl_init(p) (*(p)=0)
static int tinfl_decompress(tinfl_decompressor *d,const void *in,size_t *n,void *base,void *out,size_t *len,int flags){(void)d;(void)base;(void)flags;uLongf size=*len;int r=uncompress(out,&size,in,*n);*len=size;return r;}
static const char *const g_retired_builtin_ids[]={"old-retired"};
static bool pet_p4_miniapp_active_id(char *out,size_t n){copy_utf8(out,n,g_runtime.active?g_runtime.widget_id:"");return g_runtime.active;}
static bool pet_p4_miniapp_remove_preserving_save(const char *id,char *err,size_t n){(void)id;(void)err;(void)n;abort();}
static bool activate_catalog_index(size_t index){assert(index<g_catalog_count);g_runtime.active=true;copy_utf8(g_runtime.widget_id,48,g_catalog[index].widget_id);g_catalog_selected=index;return pet_selection_write(g_catalog_sequence,g_runtime.widget_id)&&catalog_cursor_write(g_catalog_sequence,g_runtime.widget_id);}
/* Materialize a valid package; only production sync chooses IF it is installed.
 * This adapter uses the actual catalog writer, never models seed/tombstone policy. */
static bool commit_builtin_package(const char *id,const char *widget,const char *buttons,const cJSON *sprites,char *error,size_t n){
 (void)sprites;(void)error;(void)n;materialized++;
 if(fail_package>=0&&(int)materialized==fail_package)return false;
 int at=catalog_find(g_catalog,g_catalog_count,id);if(at<0)at=(int)g_catalog_count++;
 miniapp_catalog_item_t *e=&g_catalog[at];copy_utf8(e->widget_id,48,id);e->slot=(uint8_t)at;e->resource_state=1;
 e->widget_checksum=miniapp_checksum(widget,strlen(widget));e->buttons_checksum=miniapp_checksum(buttons,strlen(buttons));
 return persist_catalog_snapshot(g_catalog,g_catalog_count,g_runtime.active?g_runtime.widget_id:NULL);
}
'''
    body += function(code, 'restore_active_after_builtin_sync')
    body += function(code, 'reorder_catalog_for_builtin_bundle')
    # Generated index must describe this fixture's packages, not the production bundle.
    import sys
    sys.path.insert(0,str(ROOT/'tools'))
    from build_builtin_bundle import render_data_source
    generated = render_data_source(bundle[:-1].decode())
    body += generated[generated.index('const char pet_p4_builtin_index_json'):generated.index('const uint8_t *pet_p4_builtin_wasm')]
    body += function(code, 'builtin_raw_free') + function(code, 'builtin_package_free')
    body += function(code, 'pet_p4_miniapp_sync_builtins')
    tail = r'''
static cJSON *plan(const char *remove,const char *order){
 char digest[9];catalog_digest(digest);cJSON *p=cJSON_CreateObject();
 cJSON_AddNumberToObject(p,"expectedCatalogSeq",g_catalog_sequence);cJSON_AddStringToObject(p,"expectedCatalogDigest",digest);
 cJSON_AddBoolToObject(p,"preserveSave",true);cJSON_AddItemToObject(p,"removeVisibleIds",cJSON_Parse(remove));cJSON_AddItemToObject(p,"survivorVisibleOrder",cJSON_Parse(order));return p;
}
static void reset(void){
 memset(g_catalog,0,sizeof(g_catalog));g_catalog_count=4;g_catalog_selected=2;g_catalog_sequence=40;g_catalog_file_generation=0;
 g_catalog_seed_pending=false;g_catalog_recovery_blocked=false;g_runtime.active=true;copy_utf8(g_runtime.widget_id,48,"keep-a");
 const char *ids[]={"keep-a","music-player","keep-b","extra-default"};
 for(size_t i=0;i<4;i++){copy_utf8(g_catalog[i].widget_id,48,ids[i]);g_catalog[i].slot=(uint8_t)(8+i);g_catalog[i].package_generation=1;g_catalog[i].widget_checksum=100+(uint32_t)i;g_catalog[i].resource_state=1;}
 io_fail=false;owner_current=true;audio_busy=false;writes=0;materialized=0;nvs_size=0;selection_size=0;nvs_fail=false;fail_package=-1;
 assert(pet_storage_lane_configure(&lane,lane_unlock,lane_lock,lane_owner,NULL));
 assert(pet_p4_miniapp_catalog_cursor_configure(enqueue_cursor,NULL));
}
static void readback(miniapp_catalog_snapshot_t *s){assert(load_catalog_snapshot((uint8_t)g_catalog_file_generation,s));assert(s->parsed);}
int main(int argc,char **argv){
 assert(argc==2);reset();char err[160]={0};const char *test=argv[1];
 if(!strcmp(test,"thumb-upgrade") || !strcmp(test,"thumb-reboot")){
  bool reboot=!strcmp(test,"thumb-reboot");
  miniapp_catalog_snapshot_t old,updated={0};
  assert(load_catalog_snapshot(0,&old) && old.count==4 && old.sequence==70);
  for(size_t i=0;i<old.count;i++)assert(!old.items[i].center_thumb_checksum);
  if(reboot)assert(load_catalog_snapshot(1,&updated));
  uint32_t sequence=0;
  miniapp_catalog_snapshot_t *latest=catalog_latest_snapshot(&old,&updated,&sequence);
  assert(sequence==(reboot?71u:70u) && latest->count==4 && !strcmp(latest->active_widget_id,"legacy-3"));
  assert(!strcmp(latest->selected_widget_id,old.selected_widget_id));
  for(size_t i=0;i<latest->count;i++){
   miniapp_catalog_item_t *item=&latest->items[i];
   char id[sizeof(item->widget_id)];snprintf(id,sizeof(id),"legacy-%u",(unsigned)i);
   assert(!strcmp(item->widget_id,id) && item->slot==12+i && item->package_generation==1);
   assert(item->widget_checksum==0x100+i && item->buttons_checksum==0x200+i);
   if(i==3){assert(!item->center_thumb_checksum);continue;}
   uint8_t expected[PET_CENTER_THUMB_BYTES],actual[PET_CENTER_THUMB_BYTES];
   memset(expected,(int)(0x60+i),sizeof(expected));
   uint32_t checksum=miniapp_checksum((const char *)expected,sizeof(expected));
   if(!reboot){
    item->center_thumb_checksum=checksum;item->center_thumb_generation=(uint8_t)i;
   }else{
    assert(item->center_thumb_checksum==checksum && item->center_thumb_generation==i);
    pet_p4_miniapp_catalog_entry_t entry={.center_title_slot=item->slot,
       .center_thumb_checksum=item->center_thumb_checksum,.center_thumb_generation=item->center_thumb_generation};
    assert(pet_p4_miniapp_center_read_thumb(&entry,actual,sizeof(actual)) && !memcmp(actual,expected,sizeof(actual)));
   }
  }
  assert(writes==0);
  if(!reboot){
   miniapp_catalog_commit_t result={0};
   assert(write_catalog_snapshot_at(latest->items,latest->count,latest->active_widget_id,
    latest->selected_widget_id,latest->seed_pending,0,sequence,&result));
   assert(result.sequence==71 && result.generation==1);
  }
 }else if(!strcmp(test,"atomic")){
#if PET_P4_SYSTEM_WIFI
  memset(g_catalog[2].community_save,'b',32);
  memset(g_catalog[0].community_save,'a',64);
  strcpy(g_catalog[0].package_metadata.version,"1.2.3");
#endif
  const unsigned char save[]={0,255,34,17};assert(write_bytes("private-save",save,sizeof(save)));
  miniapp_catalog_item_t hidden=g_catalog[1];
  cJSON *p=plan("[\"extra-default\"]","[\"keep-b\",\"keep-a\"]");
  assert(!strcmp(pet_p4_miniapp_catalog_reconcile(p,err,sizeof(err)),"committed"));cJSON_Delete(p);
  assert(g_catalog_sequence==41&&g_catalog_count==3&&g_catalog_selected==0&&!strcmp(g_runtime.widget_id,"keep-a"));
  assert(!memcmp(&g_catalog[1],&hidden,sizeof(hidden)));assert(!strcmp(g_catalog[2].widget_id,"keep-a"));
  miniapp_catalog_snapshot_t s;readback(&s);assert(!s.seed_pending&&s.count==3&&s.sequence==41);
#if PET_P4_SYSTEM_WIFI
  assert(strlen(s.items[0].community_save)==32 && !strcmp(s.items[0].community_save,g_catalog[0].community_save) && !s.items[1].community_save[0]);
  assert(!strcmp(s.items[2].community_save,g_catalog[2].community_save) && strlen(s.items[2].community_save)==64);
  assert(!strcmp(s.items[2].package_metadata.version,"1.2.3"));
#endif
  assert(!strcmp(s.active_widget_id,"keep-a")&&!strcmp(s.selected_widget_id,"keep-b"));
  assert(!strcmp(s.items[0].widget_id,"keep-b")&&!strcmp(s.items[1].widget_id,"music-player"));
  unsigned char actual[4];FILE *f=fopen("private-save","rb");assert(f&&fread(actual,1,4,f)==4);fclose(f);assert(!memcmp(actual,save,4));
  /* A new boot/firmware marker cannot resurrect the missing default. */
  memcpy(g_catalog,s.items,sizeof(g_catalog));g_catalog_count=s.count;g_catalog_seed_pending=s.seed_pending;
  unsigned before=writes;assert(pet_p4_miniapp_sync_builtins()==ESP_OK);assert(materialized==0&&g_catalog_count==3&&writes==before+1);
  assert(!strcmp(g_catalog[0].widget_id,"keep-b")&&!strcmp(g_catalog[1].widget_id,"music-player"));
 }else if(!strcmp(test,"invalid")){
  const char *removes[]={"[\"music-player\"]","[\"keep-a\"]","[\"keep-b\"]","[\"unknown\"]","[\"extra-default\",\"extra-default\"]","[]","[]"};
  const char *orders[]={"[\"keep-a\",\"keep-b\",\"extra-default\"]","[\"keep-b\",\"extra-default\"]","[\"keep-a\",\"extra-default\"]","[\"keep-a\",\"keep-b\",\"extra-default\"]","[\"keep-a\",\"keep-b\"]","[\"keep-a\",\"keep-a\",\"extra-default\"]","[\"keep-a\",\"keep-b\"]"};
  for(size_t i=0;i<sizeof(removes)/sizeof(removes[0]);i++){cJSON *p=plan(removes[i],orders[i]);assert(!strcmp(pet_p4_miniapp_catalog_reconcile(p,err,sizeof(err)),"not_committed"));cJSON_Delete(p);}
  assert(writes==0&&g_catalog_count==4&&g_catalog_sequence==40);
 }else if(!strcmp(test,"background-protect")){
  cJSON *p=plan("[\"extra-default\"]","[\"keep-b\",\"keep-a\"]");
  strcpy(registered_program,"extra-default");assert(!strcmp(pet_p4_miniapp_catalog_reconcile(p,err,sizeof(err)),"not_committed")&&!writes);
  assert(!retire_calls&&!forgotten);
  registered_program[0]=0;refuse_retirement=true;
  assert(!strcmp(pet_p4_miniapp_catalog_reconcile(p,err,sizeof(err)),"not_committed")&&!writes&&g_catalog_count==4&&!forgotten);
  refuse_retirement=false;assert(!strcmp(pet_p4_miniapp_catalog_reconcile(p,err,sizeof(err)),"committed"));
  assert(retire_calls==2 && forgotten==1);cJSON_Delete(p);
 }else if(!strcmp(test,"dispatcher-unlocked")){
  cJSON *p=plan("[\"extra-default\"]","[\"keep-b\",\"keep-a\"]");
  assert(pet_storage_lane_enter(&lane));require_unlocked=true;
  assert(!strcmp(pet_p4_miniapp_catalog_reconcile(p,err,sizeof(err)),"committed"));
  require_unlocked=false;assert(unlocks==1&&relocks==1&&!lane.occupied&&pet_storage_lane_leave(&lane));cJSON_Delete(p);
 }else if(!strcmp(test,"stale")){
  cJSON *p=plan("[\"extra-default\"]","[\"keep-b\",\"keep-a\"]");g_catalog_sequence++;
  assert(!strcmp(pet_p4_miniapp_catalog_reconcile(p,err,sizeof(err)),"not_committed"));cJSON_Delete(p);
  p=plan("[\"extra-default\"]","[\"keep-b\",\"keep-a\"]");g_catalog[3].widget_checksum++;
  assert(!strcmp(pet_p4_miniapp_catalog_reconcile(p,err,sizeof(err)),"not_committed"));cJSON_Delete(p);assert(!writes);
 }else if(!strcmp(test,"unknown")){
  cJSON *p=plan("[\"extra-default\"]","[\"keep-b\",\"keep-a\"]");io_fail=true;
  assert(!strcmp(pet_p4_miniapp_catalog_reconcile(p,err,sizeof(err)),"unknown"));assert(g_catalog_recovery_blocked&&g_catalog_count==4&&g_catalog_sequence==40);
  unsigned before=writes;io_fail=false;assert(!strcmp(pet_p4_miniapp_catalog_reconcile(p,err,sizeof(err)),"not_committed"));assert(writes==before);cJSON_Delete(p);
 }else if(!strcmp(test,"noop")){
  cJSON *p=plan("[]","[\"keep-a\",\"keep-b\",\"extra-default\"]");assert(!strcmp(pet_p4_miniapp_catalog_reconcile(p,err,sizeof(err)),"unchanged"));assert(!writes&&g_catalog_sequence==40);cJSON_Delete(p);
 }else if(!strcmp(test,"inspect")){
  cJSON *p=pet_p4_miniapp_catalog_inspect();assert(p&&cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(p,"items"))==4);
  cJSON *hidden=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(p,"items"),1);assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(hidden,"visible")));
  assert(!strcmp(json_string(p,"activeWidgetId"),"keep-a")&&!strcmp(json_string(p,"selectedWidgetId"),"keep-b"));cJSON_Delete(p);
 }else if(!strcmp(test,"cursor")){
  assert(pet_p4_miniapp_catalog_select(2));assert(g_catalog_selected==3&&nvs_writes==0);assert(!strcmp(g_runtime.widget_id,"keep-a"));
  uint64_t old_token=queued_token;assert(pet_p4_miniapp_catalog_move(-1)&&g_catalog_selected==2&&nvs_writes==0);
  assert(pet_storage_lane_enter(&lane));assert(!pet_p4_miniapp_catalog_cursor_run(old_token));assert(pet_p4_miniapp_catalog_cursor_run(queued_token));assert(pet_storage_lane_leave(&lane));
  char id[48];assert(catalog_cursor_read(40,id)&&!strcmp(id,"keep-b"));assert(!catalog_cursor_read(41,id));
  // Audio-busy cursor persistence keeps only the latest token, retries at
  // 1 Hz through the existing queue and never changes the active component.
  assert(pet_p4_miniapp_catalog_select(2));uint64_t busy_token=queued_token;
  audio_busy=true;assert(pet_storage_lane_enter(&lane));assert(!pet_p4_miniapp_catalog_cursor_run(busy_token));assert(pet_storage_lane_leave(&lane));
  assert(g_catalog_cursor_dirty);unsigned queued_before=queued_count;catalog_cursor_retry(999);assert(queued_count==queued_before);
  assert(pet_p4_miniapp_catalog_select(1));uint64_t newest=queued_token;assert(newest!=busy_token);
  catalog_cursor_retry(1000);assert(queued_token==newest&&queued_count==queued_before+2);
  audio_busy=false;assert(pet_storage_lane_enter(&lane));assert(!pet_p4_miniapp_catalog_cursor_run(busy_token));assert(pet_p4_miniapp_catalog_cursor_run(newest));assert(pet_storage_lane_leave(&lane));
  assert(!g_catalog_cursor_dirty&&!strcmp(g_runtime.widget_id,"keep-a"));assert(catalog_cursor_read(40,id)&&!strcmp(id,"keep-b"));
  queued_before=queued_count;catalog_cursor_retry(9000);assert(queued_count==queued_before);
  nvs_fail=true;assert(!pet_p4_miniapp_catalog_select_durable(2)&&g_catalog_selected==2);nvs_fail=false;
  assert(pet_p4_miniapp_catalog_select_durable(2)&&g_catalog_selected==3);
  queue_ok=false;assert(!pet_p4_miniapp_catalog_move(-1)&&g_catalog_selected==3);queue_ok=true;
  assert(!pet_p4_miniapp_catalog_cursor_run(queued_token));
  assert(pet_p4_miniapp_catalog_move(-1));g_catalog_sequence++;
  assert(pet_storage_lane_enter(&lane));assert(!pet_p4_miniapp_catalog_cursor_run(queued_token));assert(pet_storage_lane_leave(&lane));
 }else if(!strcmp(test,"navigation")){
  miniapp_catalog_snapshot_t snapshot={0};copy_utf8(snapshot.active_widget_id,48,"keep-a");copy_utf8(snapshot.selected_widget_id,48,"keep-b");
  assert(pet_selection_write(40,"extra-default"));assert(catalog_cursor_write(40,"keep-a"));
  char active[48],cursor[48];bool exists=false;catalog_restore_navigation(&snapshot,active,cursor,&exists);
  assert(exists&&!strcmp(active,"extra-default")&&!strcmp(cursor,"keep-a"));
  g_catalog_sequence=41;catalog_restore_navigation(&snapshot,active,cursor,&exists);
  assert(exists&&!strcmp(active,"keep-a")&&!strcmp(cursor,"keep-b"));
  nvs_size=selection_size=0;snapshot.selected_widget_id[0]=0;catalog_restore_navigation(&snapshot,active,cursor,&exists);
  assert(!exists&&!strcmp(active,"keep-a")&&!strcmp(cursor,"keep-a"));
 }else if(!strcmp(test,"sequence-upgrade")){
  assert(pet_p4_miniapp_catalog_select_durable(1));assert(!strcmp(g_catalog[g_catalog_selected].widget_id,"keep-b"));
  assert(persist_catalog_snapshot(g_catalog,g_catalog_count,"keep-a"));assert(g_catalog_sequence==41);
  miniapp_catalog_snapshot_t snapshot;readback(&snapshot);g_catalog_selected=0;
  char active[48],cursor[48];bool exists;catalog_restore_navigation(&snapshot,active,cursor,&exists);
  assert(!strcmp(active,"keep-a")&&!strcmp(cursor,"keep-b")&&g_catalog_selected==2);
 }else if(!strcmp(test,"seventeen-ab")||!strcmp(test,"twentyeight-ab")){
  size_t count=!strcmp(test,"seventeen-ab")?17:28;
  g_catalog_count=count;g_catalog_selected=7;memset(g_catalog,0,sizeof(g_catalog));
  for(size_t i=0;i<count;i++){snprintf(g_catalog[i].widget_id,48,"user-%02u",(unsigned)(count-1-i));g_catalog[i].slot=(uint8_t)i;g_catalog[i].widget_checksum=123+(uint32_t)i;g_catalog[i].resource_state=1;}
  copy_utf8(g_runtime.widget_id,48,g_catalog[3].widget_id);
  assert(persist_catalog_snapshot(g_catalog,g_catalog_count,g_runtime.widget_id));
  assert(persist_catalog_snapshot(g_catalog,g_catalog_count,g_runtime.widget_id));
  /* Both previous schema generations lack new fields. B is version 2, A v3;
   * neither is an invitation to seed missing defaults or merge old membership. */
  for(uint8_t gen=0;gen<2;gen++){
   char path[48],raw[16384];catalog_snapshot_path(gen,false,path,sizeof(path));assert(read_file(path,raw,sizeof(raw)));
   cJSON *p=cJSON_Parse(raw);cJSON_DeleteItemFromObjectCaseSensitive(p,"seedPending");cJSON_DeleteItemFromObjectCaseSensitive(p,"selectedWidgetId");cJSON_DeleteItemFromObjectCaseSensitive(p,"check");
   if(gen==1)cJSON_ReplaceItemInObjectCaseSensitive(p,"version",cJSON_CreateNumber(2));
   char *json=cJSON_PrintUnformatted(p);assert(write_bytes(path,json,strlen(json)));cJSON_free(json);cJSON_Delete(p);
  }
  miniapp_catalog_snapshot_t a,b;assert(load_catalog_snapshot(0,&a)&&load_catalog_snapshot(1,&b));
  uint32_t seq=0;miniapp_catalog_snapshot_t *latest=catalog_latest_snapshot(&a,&b,&seq);assert(seq==42&&latest->count==count&&!latest->seed_pending);
#if PET_P4_SYSTEM_WIFI
  for(size_t i=0;i<count;i++)assert(!a.items[i].community_save[0]&&!b.items[i].community_save[0]);
#endif
  assert(catalog_cursor_write(seq,g_catalog[7].widget_id));
  char active[48],cursor[48];bool exists;catalog_restore_navigation(latest,active,cursor,&exists);
  assert(!strcmp(active,g_catalog[3].widget_id)&&!strcmp(cursor,g_catalog[7].widget_id));
  assert(pet_p4_miniapp_sync_builtins()==ESP_OK&&g_catalog_count==count&&materialized==0);
  assert(write_bytes("marker","changed-build-and-bundle",24));
  assert(pet_p4_miniapp_sync_builtins()==ESP_OK&&g_catalog_count==count&&materialized==0);
  for(size_t i=0;i<count;i++){char id[48];snprintf(id,sizeof(id),"user-%02u",(unsigned)(count-1-i));assert(!strcmp(g_catalog[i].widget_id,id)&&g_catalog[i].slot==i);}
  assert(!strcmp(g_runtime.widget_id,g_catalog[3].widget_id)&&g_catalog_selected==7);
 }else if(!strcmp(test,"seed-interrupted")){
  g_catalog_count=0;g_catalog_selected=0;g_runtime.active=false;g_catalog_seed_pending=true;fail_package=2;
  assert(pet_p4_miniapp_sync_builtins()==ESP_FAIL&&materialized==2&&g_catalog_count==1);
  miniapp_catalog_snapshot_t s;readback(&s);assert(s.seed_pending&&s.count==1);
  memcpy(g_catalog,s.items,sizeof(g_catalog));g_catalog_count=s.count;g_catalog_seed_pending=s.seed_pending;
  fail_package=-1;assert(pet_p4_miniapp_sync_builtins()==ESP_OK&&materialized==3&&g_catalog_count==2&&!g_catalog_seed_pending);
  readback(&s);assert(!s.seed_pending&&s.count==2);
 }else if(!strcmp(test,"wire")){
  pet_p4_runtime_state_t state={.board_device_id="board-one"};
  cJSON *p=cJSON_Parse("{\"requestId\":\"inspect-1\"}");
  assert(send_catalog_admin(&state,"widget/catalog_inspect",p,NULL,NULL));cJSON_Delete(p);
  assert(!strcmp(reply_topic,"widget/catalog_snapshot")&&!strcmp(json_string(reply,"bootId"),"0123456789abcdef"));
  p=plan("[\"extra-default\"]","[\"keep-b\",\"keep-a\"]");
  cJSON_AddStringToObject(p,"requestId","restore-1");cJSON_AddStringToObject(p,"expectedBoardDeviceId","wrong-board");cJSON_AddStringToObject(p,"expectedBootId","0123456789abcdef");
  assert(!send_catalog_admin(&state,"widget/catalog_reconcile",p,NULL,NULL));assert(!writes&&!strcmp(json_string(reply,"commitState"),"not_committed"));
  cJSON_ReplaceItemInObjectCaseSensitive(p,"expectedBoardDeviceId",cJSON_CreateString("board-one"));
  cJSON_ReplaceItemInObjectCaseSensitive(p,"expectedBootId",cJSON_CreateString("old-boot"));
  assert(!send_catalog_admin(&state,"widget/catalog_reconcile",p,NULL,NULL)&&!writes);
  cJSON_ReplaceItemInObjectCaseSensitive(p,"expectedBootId",cJSON_CreateString("0123456789abcdef"));
  cJSON_AddBoolToObject(p,"unknown",true);assert(!send_catalog_admin(&state,"widget/catalog_reconcile",p,NULL,NULL)&&!writes);cJSON_DeleteItemFromObjectCaseSensitive(p,"unknown");
  cJSON_AddStringToObject(p,"requestId","duplicate");assert(!send_catalog_admin(&state,"widget/catalog_reconcile",p,NULL,NULL)&&!writes);cJSON_DeleteItemFromObjectCaseSensitive(p,"requestId");cJSON_DeleteItemFromObjectCaseSensitive(p,"requestId");cJSON_AddStringToObject(p,"requestId","restore-1");
  assert(send_catalog_admin(&state,"widget/catalog_reconcile",p,NULL,NULL));assert(!strcmp(reply_topic,"widget/catalog_reconcile_result"));assert(!strcmp(json_string(reply,"commitState"),"committed"));assert(cJSON_GetObjectItemCaseSensitive(reply,"catalogSeq")->valuedouble==41);assert(!cJSON_HasObjectItem(reply,"items"));cJSON_Delete(p);cJSON_Delete(reply);reply=NULL;
 }else if(!strcmp(test,"old-schema")){
  assert(persist_catalog_snapshot(g_catalog,g_catalog_count,"keep-a"));char raw[16384],path[48];catalog_snapshot_path((uint8_t)g_catalog_file_generation,false,path,sizeof(path));assert(read_file(path,raw,sizeof(raw)));
  cJSON *p=cJSON_Parse(raw);cJSON_DeleteItemFromObjectCaseSensitive(p,"seedPending");cJSON_DeleteItemFromObjectCaseSensitive(p,"selectedWidgetId");cJSON_DeleteItemFromObjectCaseSensitive(p,"check");
  char *s=cJSON_PrintUnformatted(p);assert(write_bytes(path,s,strlen(s)));cJSON_free(s);cJSON_Delete(p);
  miniapp_catalog_snapshot_t loaded;readback(&loaded);assert(!loaded.seed_pending&&!loaded.selected_widget_id[0]);
  p=cJSON_Parse(raw);cJSON_DeleteItemFromObjectCaseSensitive(p,"check");cJSON_ReplaceItemInObjectCaseSensitive(p,"seedPending",cJSON_CreateString("true"));s=cJSON_PrintUnformatted(p);assert(write_bytes(path,s,strlen(s)));cJSON_free(s);cJSON_Delete(p);assert(!load_catalog_snapshot((uint8_t)g_catalog_file_generation,&loaded));
 }else if(!strcmp(test,"empty")){
  g_catalog_count=0;g_catalog_selected=0;g_runtime.active=false;assert(persist_catalog_snapshot(g_catalog,0,NULL));miniapp_catalog_snapshot_t s;readback(&s);assert(!s.seed_pending&&!s.count);
  assert(pet_p4_miniapp_sync_builtins()==ESP_OK&&materialized==0&&g_catalog_count==0);
 }else if(!strcmp(test,"fresh")){
  g_catalog_count=0;g_catalog_selected=0;g_runtime.active=false;g_catalog_seed_pending=true;
  assert(pet_p4_miniapp_sync_builtins()==ESP_OK&&materialized==2&&g_catalog_count==2&&!g_catalog_seed_pending);
  miniapp_catalog_snapshot_t s;readback(&s);assert(!s.seed_pending&&s.count==2);
  assert(pet_p4_miniapp_sync_builtins()==ESP_OK&&materialized==2);
 }else abort();
 return 0;
}
'''
    src=out/'catalog.c';exe=out/'catalog'
    src.write_text(prelude + f'\n#define PET_P4_COMMIT_LEAN_IO {request.param}\n#define PET_P4_SYSTEM_WIFI {request.param}\n' + body + tail)
    vendor=ROOT/'tests/vendor/cjson'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-error=deprecated-declarations','-fsanitize=address,undefined','-I',str(vendor),'-I',str(ROOT/'main'),str(src),str(vendor/'cJSON.c'),str(ROOT/'main/pet_p4_storage_lane.c'),'-lz','-o',str(exe)],check=True,capture_output=True,text=True)
    return exe

@pytest.mark.parametrize('scenario',['atomic','invalid','stale','unknown','noop','inspect','cursor','old-schema','empty','fresh','navigation','wire','sequence-upgrade','seventeen-ab','twentyeight-ab','seed-interrupted','dispatcher-unlocked','background-protect'])
def test_production_catalog_persistence_and_reconcile(binary,tmp_path,scenario):
    subprocess.run([str(binary),scenario],cwd=tmp_path,check=True,capture_output=True,text=True)


@pytest.mark.parametrize('version,partial', [(2, False), (3, False), (3, True)],
                         ids=['pre-sprites-v2', 'pre-covers-v3', 'invalid-optional-metadata'])
def test_old_catalog_and_mixed_thumbnail_banks_across_cold_processes(binary, tmp_path, version, partial):
    # Historical bytes independent of the current writer, including a package
    # generation that differs from thumbnail banks zero and two.
    items = [dict(widgetId=f'legacy-{i}', title=f'Legacy {i}', slot=12+i,
                  packageGeneration=1, widgetChecksum=f'{0x100+i:08x}',
                  buttonsChecksum=f'{0x200+i:08x}') for i in range(4)]
    legacy = dict(version=version, sequence=70, activeWidgetId='legacy-3', items=items)
    if version == 3:
        legacy.update(selectedWidgetId='legacy-1', seedPending=False)
        for i, item in enumerate(items):
            item['spritesChecksum'] = f'{0x300+i:08x}'

    if partial:
        items[0].update(thumb='12345678')                   # Missing bank.
        items[1].update(thumb='12345678', thumbBank=3)      # Out of range.
        items[2].update(thumb='1234xxxx', thumbBank=1)      # Invalid digest.

    old = tmp_path/'catalog.0'
    old_bytes = json.dumps(legacy, separators=(',', ':')).encode()
    old.write_bytes(old_bytes)
    covers = {tmp_path/f'p4th{12+i:02d}-{i}.p4t': bytes([0x60+i])*1568 for i in range(3)}
    for path, data in covers.items():
        path.write_bytes(data)

    subprocess.run([binary, 'thumb-upgrade'], cwd=tmp_path, check=True)
    updated = tmp_path/'catalog.1'
    updated_bytes = updated.read_bytes()
    records = json.loads(updated_bytes)['items']
    for i, item in enumerate(items):
        assert all(records[i][key] == value for key, value in item.items()
                   if key not in ('thumb', 'thumbBank'))
    assert 'thumb' not in records[3] and 'thumbBank' not in records[3]

    subprocess.run([binary, 'thumb-reboot'], cwd=tmp_path, check=True)
    assert old.read_bytes() == old_bytes and updated.read_bytes() == updated_bytes
    assert all(path.read_bytes() == data for path, data in covers.items())
