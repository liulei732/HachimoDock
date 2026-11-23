"""Production BUSY classifier with the actual resource broker, no device."""
from pathlib import Path
import subprocess
from p4_render_trace_test import function

ROOT=Path(__file__).resolve().parents[1]


def test_busy_requires_other_live_current_owner_and_full_real_broker(tmp_path):
    text=(ROOT/'main/pet_p4_miniapp_background.inc').read_text()
    helper=function(text,'background_other_program_busy')
    code=r'''
#include <assert.h>
#include <string.h>
#include "pet_p4_resources.h"
#define PET_PROGRAM_REGISTRY_SLOTS 4
typedef struct {char id[48];bool occupied,stopped;struct{bool live;struct{pet_resource_lease_t lease;}host;}session;}pet_program_registry_entry_t;
static struct{pet_program_registry_entry_t entries[4];}g_background_programs;
static pet_resource_broker_t broker;
static void pet_resources_device_usage(pet_resource_usage_t*out){pet_resources_usage(&broker,out);}
static bool pet_resources_device_current(pet_resource_lease_t l){return pet_resources_current(&broker,l);}
'''+helper+r'''
int main(void){
 assert(!background_other_program_busy("second"));
 pet_resource_owner_t owner={0};pet_resource_lease_t lease={0};
 assert(pet_resources_owner_open(&broker,PET_RESOURCE_MASK(PET_RESOURCE_PROGRAM),&owner)==PET_RESOURCE_OK);
 assert(pet_resources_try_acquire(&broker,owner,PET_RESOURCE_PROGRAM,4096,&lease)==PET_RESOURCE_OK);
 pet_program_registry_entry_t*e=&g_background_programs.entries[0];strcpy(e->id,"first");e->occupied=e->session.live=true;e->session.host.lease=lease;
 pet_resource_broker_t before=broker;assert(background_other_program_busy("second"));assert(!memcmp(&broker,&before,sizeof broker));
 assert(!background_other_program_busy("first"));
 e->stopped=true;assert(!background_other_program_busy("second"));e->stopped=false;
 e->session.live=false;assert(!background_other_program_busy("second"));e->session.live=true;
 assert(pet_resources_owner_revoke(&broker,owner));assert(!background_other_program_busy("second"));
 (void)pet_resources_finish(&broker,lease);assert(!background_other_program_busy("second"));
 return 0;
}
'''
    source=tmp_path/'busy.c';source.write_text(code);exe=tmp_path/'busy'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-DPET_COMPONENT_PROGRAM_ENABLED=1','-DPET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=1',
                    '-I',str(ROOT/'main'),str(source),str(ROOT/'main/pet_p4_resources.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=10)


def test_busy_refusal_precedes_checkpoint_and_retirement():
    start=function((ROOT/'main/pet_p4_miniapp_background.inc').read_text(),'background_start_installed')
    assert start.index('if(duplicate)') < start.index('if(program_busy)') < start.index('if(found&&!background_prepare_remove')
    assert start.index('"PROGRAM_BUSY"') < start.index('save_app_runtime()')


def test_repeat_start_confirms_existing_owner_without_storage_io(tmp_path):
    start = function((ROOT/'main/pet_p4_miniapp_background.inc').read_text(), 'background_start_installed')
    # Execute all production admission decisions up to the first foreground write.
    admission = start[:start.index('  if(!save_app_runtime()')] + '  return false;\n}'
    fixture = tmp_path/'repeat.c'
    fixture.write_text(r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#define PET_P4_EVENTS 1
typedef struct {int slot,resource_state;uint32_t widget_checksum,buttons_checksum,sprites_checksum;}miniapp_catalog_item_t;
typedef struct {int slot;uint64_t serial;}pet_program_registration_t;
typedef struct {uint32_t content[3];bool stopped;struct{bool live;struct{int lease;}host;}session;struct{bool local_background;}config;}pet_program_registry_entry_t;
typedef int pet_storage_scope_t;
static miniapp_catalog_item_t g_catalog[]={{0,1,1,2,3}};
static struct {pet_program_registry_entry_t entries[1];}g_background_programs;
static int g_catalog_count=1,g_catalog_sequence,io;static bool g_catalog_recovery_blocked,mutating,audio,consent=true;
#define xSemaphoreTake(a,b) ((void)0)
#define xSemaphoreGive(a) ((void)0)
#define pet_program_registry_find(reg,id,out) ((out)->serial=17,true)
#define pet_resources_device_current(lease) (lease)
#define storage_audio_busy() audio
#define pet_p4_miniapp_storage_busy() mutating
#define pet_p4_miniapp_background_available() true
#define safe_widget_id(id) true
#define set_error(buf,size,msg) ((void)0)
#define catalog_find(items,count,id) ((count)==1?0:-1)
#define local_program_authorized(slot,id,content) consent
#define background_other_program_busy(id) false
static bool storage_scope_begin(pet_storage_scope_t *scope,bool local){io++;return false;}
static void storage_scope_finish(pet_storage_scope_t *scope){}
''' + admission + r'''
int main(void){
 const uint32_t hash[]={1,2,3};uint64_t token=0;
 pet_program_registry_entry_t *entry=&g_background_programs.entries[0];
 memcpy(entry->content,hash,sizeof(hash));entry->session.live=entry->session.host.lease=1;
 #define start(local) background_start_installed("app",hash,&token,NULL,0,local,NULL,NULL)
 audio=true;assert(start(false)&&token==17&&!io);token=0;
 assert(!start(true)&&!token&&!io);
 entry->config.local_background=true;consent=false;assert(!start(true)&&!token&&!io);
 consent=true;assert(start(true)&&token==17&&!io);token=0;
 mutating=true;assert(!start(true)&&!token&&!io);mutating=false;
 g_catalog_recovery_blocked=true;assert(!start(true)&&!token&&!io);g_catalog_recovery_blocked=false;
 g_catalog[0].buttons_checksum++;assert(!start(true)&&!token&&!io);g_catalog[0].buttons_checksum--;
 audio=false;entry->session.host.lease=0;assert(!start(true)&&!token&&io==1);
}
''')
    exe = tmp_path/'repeat'
    subprocess.run(['cc','-std=c11','-fsanitize=address,undefined',str(fixture),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=5)
