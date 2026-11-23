from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]

def test_scoped_maintenance_dispatch(tmp_path):
    vendor = ROOT / "tests/vendor/cjson"
    binary = tmp_path / "maintenance"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-deprecated-declarations", "-fsanitize=address,undefined", "-I", str(vendor),
                    str(ROOT / "tests/p4_lan_maintenance_test.c"), str(vendor / "cJSON.c"),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)

def test_resource_stream_dispatch_keeps_repair_scope(tmp_path):
    from p4_render_trace_test import function
    production = (ROOT / "main/pet_p4_lan_component.inc").read_text()
    unit = tmp_path / "repair.c"
    header = (ROOT / "main/pet_p4_miniapp.h").read_text()
    commit_constants = "\n".join(line for line in header.splitlines() if line.startswith("#define PET_P4_WIDGET_COMMIT_"))
    wire = (ROOT / "main/pet_p4_lan_component_wire.inc").read_text()
    unit.write_text(commit_constants + "\n" + r'''
#include "cJSON.h"
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#define PET_P4_MINIAPP_WIDGET_ID_MAX 48
#define PET_P4_WIDGET_TRANSFER_PROTOCOL "fixture"
#define PET_P4_WIDGET_CHUNK_BYTES 1024
#define WIDGET_TRANSFER_FILES_MAX 16
#define PET_CENTER_THUMB_BYTES 1568
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#define heap_caps_malloc(n,caps) malloc(n)
typedef struct {cJSON *manifest;const uint8_t *record;size_t size;uint32_t sequence,generation,association;bool repair_only;uint8_t commit_reason;} bundle_job_t;
typedef struct {char widget_id[48];uint8_t resource_state;struct {char version[64];} package_metadata;} pet_p4_miniapp_catalog_entry_t;
static unsigned state, begins,chunks,commits,aborts;static bool current=true;
static bool component_socket_live(void *ignored){return current;}
static bool repair_route_current(const bundle_job_t *job){return current;}
static uint32_t esp_random(void){return 9;}
static void trust_hex(char *out,const uint8_t *p,size_t n){memset(out,'a',64);out[64]=0;}
static size_t pet_p4_miniapp_catalog_count(void){return 1;}
static bool pet_p4_miniapp_catalog_get(size_t i,pet_p4_miniapp_catalog_entry_t *item){
  assert(i==0);strcpy(item->widget_id,"demo");item->resource_state=state;return true;
}
static size_t pet_p4_miniapp_catalog_capacity(void){return 2;}
static bool pet_p4_miniapp_catalog_recovery_blocked(void){return false;}
static bool pet_p4_miniapp_lan_install_idle(void){return true;}
static bool pet_p4_miniapp_lan_new_id(const char *id){return false;}
static bool pet_p4_miniapp_lan_bundle_begin(cJSON *m,char *e,size_t cap){
  assert(cJSON_IsTrue(cJSON_GetObjectItem(m,"repairResources")));
  assert(!cJSON_HasObjectItem(m,"thumbOnly"));begins++;return true;
}
static bool component_chunk(const char *transfer,const char *path,const uint8_t *data,size_t len,uint32_t index,
 uint32_t offset,uint32_t bytes,uint32_t seq,char *scratch,size_t capacity,char *e,size_t cap){
  assert(!strcmp(path,"runtime/images/dog.p4i"));chunks++;return true;
}
static bool component_publish(void *ctx){return current;}
static void pet_p4_miniapp_lan_abort(cJSON *m){aborts++;}
static const char *failure;
static bool pet_p4_miniapp_lan_commit(cJSON *m,bool (*live)(void *),void *ctx,char *error,size_t cap){
  assert(live(ctx));commits++;if(failure) snprintf(error,cap,"%s",failure);return !failure;
}
''' + '\n'.join(function(production, n) for n in ("bundle_u32","bundle_begin","bundle_commit_reason","bundle_step")) + "\n" + function(wire, "bundle_wire_status") + r'''
int main(void){
 uint8_t record[128]={7,4,3};record[6]=32;memcpy(record+39,"demo",4);
 bundle_job_t job={.repair_only=true,.record=record,.size=43};
 for(state=0;state<=1;state++) assert(!bundle_step(&job,true));
 state=3;record[39]='x';assert(!bundle_step(&job,true));record[39]='d';
 for(int op=0;op<=6;op++){record[0]=op;assert(!bundle_step(&job,true));}
 record[0]=7;assert(!bundle_step(&job,false));assert(bundle_step(&job,true));assert(begins==1);
 assert(!bundle_step(&job,true));const char *paths[]={"runtime/center-thumb.match","runtime/speech.hab","runtime/app.wasm","runtime/images/dog.p4i"};
 for(int i=0;i<4;i++){record[0]=2;record[10]=strlen(paths[i]);memcpy(record+11,paths[i],record[10]);job.size=12+record[10];
   assert(bundle_step(&job,true)==(i==3));}
 assert(chunks==1 && job.sequence==1);
 record[0]=3;job.size=1;current=false;assert(!bundle_step(&job,true));current=true;
 assert(!bundle_step(&job,false));assert(bundle_step(&job,true));assert(commits==1);
 // The existing mode-26 lane and malformed/unbound mode-14 requests cannot opt in.
 record[1]=1;job.size=2;job.association=7;assert(!bundle_step(&job,true));assert(commits==1);
 job.repair_only=false;job.association=0;assert(!bundle_step(&job,true));assert(commits==1);
 job.association=7;record[1]=2;assert(!bundle_step(&job,true));assert(commits==1);
 record[1]=1;current=false;assert(!bundle_step(&job,true));current=true;assert(commits==1);
 assert(!bundle_step(&job,false));assert(commits==1);
 failure=PET_P4_WIDGET_COMMIT_AUDIO_BUSY;
 job.size=1;assert(!bundle_step(&job,true));assert(bundle_wire_status(0,&job)==0); // legacy client
 job.size=2;
 struct {const char *error;uint8_t code;} cases[]={
   {PET_P4_WIDGET_COMMIT_STORAGE_DEFERRED,16},{PET_P4_WIDGET_COMMIT_AUDIO_BUSY,17},
   {PET_P4_WIDGET_COMMIT_STORAGE_INTERRUPTED,18},{"component directories need recovery; stored packages were preserved",19},
   {"private-id: WASM first frame: private guest data",26},
   {"component commit failed: open /private/file errno=123",27},{"private unrecognized string",0},
   {"component commit failed: catalog readback failed; completion unknown",2},
   {"component catalog outcome unknown; inspect only, do not replay",2},
 };
 for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++){
   failure=cases[i].error;assert(!bundle_step(&job,true));
   assert(bundle_wire_status(0,&job)==cases[i].code);
   assert(bundle_wire_status(2,&job)==2); // publication already started
 }
 failure=NULL;assert(bundle_step(&job,true));assert(bundle_wire_status(1,&job)==1);
 job.record=NULL;assert(bundle_step(&job,false));assert(aborts==1);cJSON_Delete(job.manifest);return 0;
}
''')
    vendor = ROOT / 'tests/vendor/cjson'
    exe = tmp_path / 'repair'
    subprocess.run(['cc','-std=c11','-fsanitize=address,undefined','-Wno-deprecated-declarations',
                    '-I',str(vendor),str(unit),str(vendor/'cJSON.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=10)
