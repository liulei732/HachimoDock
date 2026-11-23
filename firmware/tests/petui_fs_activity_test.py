"""Real observer/cleanup integration, concurrent actor, errors and coverage guard."""
from pathlib import Path
import subprocess
from p4_render_trace_test import function

ROOT=Path(__file__).resolve().parents[1]


def test_real_activity_observer_concurrency_and_swallowed_directory_error(tmp_path):
    header=tmp_path/"freertos/FreeRTOS.h";header.parent.mkdir()
    header.write_text('''#pragma once
#include <pthread.h>
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
extern _Thread_local unsigned critical_depth;
#define portENTER_CRITICAL(m) do{assert(!critical_depth);pthread_mutex_lock(m);critical_depth++;}while(0)
#define portEXIT_CRITICAL(m) do{assert(critical_depth==1);critical_depth--;pthread_mutex_unlock(m);}while(0)
''')
    source=r'''
#define PET_FS_ACTIVITY_TEST 1
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
_Thread_local unsigned critical_depth;
#include "pet_p4_fs_activity.c"
static pthread_mutex_t barrier=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition=PTHREAD_COND_INITIALIZER;
static bool started,released;
static unsigned remove_count;
static bool swallowed_error;
static void *reader(void *unused){
 (void)unused;PET_FS_ACTIVITY_SCOPE(io);
 assert(!critical_depth);pthread_mutex_lock(&barrier);started=true;pthread_cond_broadcast(&condition);
 while(!released)pthread_cond_wait(&condition,&barrier);
 pthread_mutex_unlock(&barrier);return NULL;
}
static struct dirent *observed_readdir(DIR *dir){
 assert(!critical_depth);
 if(swallowed_error){
  PET_FS_ACTIVITY_SCOPE(concurrent_read); // independent VFS reader clears shared SPIFFS errno
  errno=0;return NULL;
 }
 return readdir(dir);
}
static int counted_remove(const char *path){assert(!critical_depth);remove_count++;return remove(path);}
#define readdir observed_readdir
#define remove counted_remove
#include "pet_p4_file_cleanup.c"
#undef readdir
#undef remove
static bool owner(void *p){(void)p;return true;}
static bool idle(void *p,uint64_t *out){(void)p;return pet_fs_activity_idle_epoch(out);}
static void early(void){PET_FS_ACTIVITY_SCOPE(io);errno=ENOENT;return;}
int main(void){
 uint64_t before,after;errno=ENOSPC;assert(!pet_fs_activity_idle_epoch(&before)&&errno==ENOSPC);
 pet_fs_activity_t existing=pet_fs_activity_begin();pet_fs_activity_enable_observation();
 assert(!pet_fs_activity_idle_epoch(&after)&&after>before);pet_fs_activity_end(&existing);
 assert(pet_fs_activity_idle_epoch(&before));early();assert(errno==ENOENT);
 assert(pet_fs_activity_idle_epoch(&after)&&after==before+2);
 before=after;pthread_t thread;assert(!pthread_create(&thread,NULL,reader,NULL));
 pthread_mutex_lock(&barrier);while(!started)pthread_cond_wait(&condition,&barrier);pthread_mutex_unlock(&barrier);
 assert(!pet_fs_activity_idle_epoch(&after));
 pthread_mutex_lock(&barrier);released=true;pthread_cond_broadcast(&condition);pthread_mutex_unlock(&barrier);
 assert(!pthread_join(thread,NULL));assert(pet_fs_activity_idle_epoch(&after)&&after==before+2);
 assert(!mkdir("store",0700));FILE *file=fopen("store/target","wb");assert(file);fputs("value",file);fclose(file);
 pet_file_cleanup_target_t target={"store/target"};pet_file_cleanup_plan_t plan;
 pet_file_cleanup_guard_t guard={.owner_current=owner,.idle_epoch=idle};
 swallowed_error=true;
 assert(pet_file_cleanup_prepare(&plan,"store",&target,1,guard)&&!plan.snapshot_used);
 assert(pet_file_cleanup_remove_at(&plan,0)&&remove_count==1&&access(target.path,F_OK));
 swallowed_error=false;
 // Enable is idempotent, never resets activity history. All overflow modes
 // disable eligibility permanently without rejecting the original I/O.
 assert(pet_fs_activity_idle_epoch(&before));pet_fs_activity_enable_observation();
 assert(pet_fs_activity_idle_epoch(&after)&&after==before);
 fs_activity_active=UINT32_MAX;existing=pet_fs_activity_begin();assert(!existing.counted);
 assert(!pet_fs_activity_idle_epoch(&after));pet_fs_activity_end(&existing);
 fs_activity_active=0;assert(!pet_fs_activity_idle_epoch(&after));
 fs_activity_unsafe=false;fs_activity_epoch=UINT64_MAX-1;
 existing=pet_fs_activity_begin();pet_fs_activity_end(&existing);
 assert(!pet_fs_activity_idle_epoch(&after)&&after==UINT64_MAX);
 pet_fs_activity_enable_observation();assert(!pet_fs_activity_idle_epoch(&after));
 return 0;
}
'''
    path=tmp_path/"activity.c";path.write_text(source);binary=tmp_path/"activity"
    subprocess.run(["cc","-std=c11","-D_POSIX_C_SOURCE=200809L","-Wall","-Wextra","-Werror","-pthread",
                    "-fsanitize=address,undefined","-I",str(tmp_path),"-I",str(ROOT/"main"),str(path),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],cwd=tmp_path,check=True,timeout=20)


def test_all_independent_spiffs_entrypoints_are_observed():
    renderer=(ROOT/"main/pet_p4_renderer.c").read_text()
    prepare=function(renderer,"prepare_asset_entry")
    assert prepare.index("if (ok && !job.raw_source)") < prepare.index("file_activity=pet_fs_activity_begin()") < prepare.index("fopen(")
    assert prepare.index("if (file) fclose(file)") < prepare.index("pet_fs_activity_end(&file_activity)") < prepare.index("h264_stream_has_single_slice_access_units")
    audio=(ROOT/"main/pet_p4_audio.c").read_text()
    playback=function(audio,"playback_task")
    assert playback.index("file_activity=pet_fs_activity_begin()") < playback.index("fopen(")
    assert playback.index("wav_seek_pcm") < playback.index("if (!file) pet_fs_activity_end(&file_activity)")
    assert playback.rindex("fclose(file)") < playback.rindex("pet_fs_activity_end(&file_activity)")
    # New direct file entrypoints require explicit coverage review, not merely
    # retaining an enabled flag. Read helpers inherit the open-file scope.
    assert renderer.count("fopen(")==prepare.count("fopen(")==1
    assert audio.count("fopen(")==playback.count("fopen(")==1
    scope = playback.index("file_activity=pet_fs_activity_begin()")
    override = playback.index('file=fopen(fs_path,"rb")')
    assert scope < override < playback.index("wav_seek_pcm")
    assert "pet_p4_asset_fs_path(request.logical_path" not in playback
    store=(ROOT/"main/pet_p4_app_store.c").read_text()
    for name,scope in (("read_slot","storage_read"),("write_slot_data","storage_write"),
                       ("read_epoch","epoch_read")):
        body=function(store,name)
        assert body.index(f"PET_FS_ACTIVITY_SCOPE({scope})") < body.index("fopen(") < body.index("fclose(")
    # Both scratch-storage variants delegate their whole I/O lifetime.
    writer=function(store,"write_slot")
    assert writer.count("return write_slot_data(")==2 and "fopen(" not in writer
    epoch=function(store,"write_forget_epoch")
    record=function(store,"write_epoch_record")
    assert epoch.index("PET_FS_ACTIVITY_SCOPE(epoch_write)") < epoch.index("write_epoch_record(")
    assert store.count("write_epoch_record(")==epoch.count("write_epoch_record(")+1
    assert record.index("fopen(") < record.index("fclose(")
    miniapp=(ROOT/"main/pet_p4_miniapp.c").read_text()
    images=function(miniapp,"pet_p4_miniapp_get_surface_images")
    assert "surface_images_locked(out,revision)" in images
    copied=function(miniapp,"surface_images_locked")
    assert "memcpy" in copied
    for body in (images,copied,function(miniapp,"pet_p4_miniapp_render_snapshot")):
        assert "fopen" not in body and "read_file" not in body


def test_real_file_cue_io_lifetime_errors_and_shared_pcm_fallback(tmp_path):
    header=tmp_path/"freertos/FreeRTOS.h";header.parent.mkdir()
    header.write_text('''#pragma once
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(m) ((void)(m))
#define portEXIT_CRITICAL(m) ((void)(m))
''')
    audio=(ROOT/"main/pet_p4_audio.c").read_text()
    playback=function(audio,"playback_task")
    body=playback[playback.index("    char fs_path[80];"):playback.rindex("  }\n}")]
    source=r'''
#define PET_FS_ACTIVITY_TEST 1
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "pet_p4_fs_activity.c"
#define ESP_OK 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define PET_P4_AUDIO_PLAYBACK_MAX_PCM_BYTES 64
static atomic_bool g_playback_active;
static atomic_uint g_playback_generation;
static void*g_speaker;
static unsigned scenario,codec_calls;
static uint8_t pcm[16];
typedef struct{char logical_path[48];unsigned family,generation;bool audio_custom;}request_t;
static void active(void){uint64_t epoch;assert(!pet_fs_activity_idle_epoch(&epoch)&&fs_activity_active==1);}
static FILE*observed_open(const char*p,const char*m){active();if(scenario==1){errno=ENOENT;return NULL;}return fopen(p,m);}
static size_t observed_read(void*p,size_t n,size_t count,FILE*f){active();if(scenario==4)return 0;return fread(p,n,count,f);}
static int observed_close(FILE*f){active();return fclose(f);}
static void observed_rewind(FILE*f){active();rewind(f);}
static bool pet_p4_asset_audio_override_path(unsigned family,char*out,size_t n){
 (void)family;if(scenario==3)return false;snprintf(out,n,"cue.wav");return true;
}
static bool pet_p4_system_cue_family(unsigned f){(void)f;return scenario==3;}
static uint64_t fnv1a64_update(uint64_t h,const void*p,size_t n){(void)p;return h+n;}
static bool pet_p4_system_cue_legacy(unsigned f,uint64_t h){(void)f;(void)h;return scenario==3;}
static bool wav_seek_pcm(FILE*f,uint32_t*remaining){active();rewind(f);*remaining=8;return scenario!=2;}
static bool pet_p4_system_cue_pcm(unsigned f,const uint8_t**out,size_t*n){
 (void)f;uint64_t epoch;assert(pet_fs_activity_idle_epoch(&epoch));
 if(scenario!=1&&scenario!=3)return false;
 static uint8_t shared[8];*out=shared;*n=sizeof(shared);return true;
}
static void pet_p4_audio_send_status(void){}
static int esp_codec_dev_write(void*s,void*p,int n){(void)s;(void)p;(void)n;codec_calls++;return scenario==5?-1:ESP_OK;}
#define fopen observed_open
#define fread observed_read
#define fclose observed_close
#define rewind observed_rewind
static void iteration(request_t request){for(unsigned once=0;once<1;once++){
''' + body + r'''
}}
#undef fopen
#undef fread
#undef fclose
#undef rewind
int main(void){
 FILE*f=fopen("cue.wav","wb");assert(f);fputs("0123456789abcdef",f);fclose(f);
 pet_fs_activity_enable_observation();request_t request={.logical_path="cue.wav"};
 for(scenario=0;scenario<=6;scenario++){
  uint64_t before,after;codec_calls=0;assert(pet_fs_activity_idle_epoch(&before));
  iteration(request);assert(pet_fs_activity_idle_epoch(&after)&&after==before+2);
  assert(!atomic_load(&g_playback_active));
  if(scenario==2||scenario==4)assert(codec_calls==0);else assert(codec_calls==1);
 }
 return 0;
}
'''
    path=tmp_path/"file_cue.c";path.write_text(source);binary=tmp_path/"file_cue"
    subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-fsanitize=address,undefined",
                    "-I",str(tmp_path),"-I",str(ROOT/"main"),str(path),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],cwd=tmp_path,check=True,timeout=20)


def test_font_worker_and_expiration_guard_real_io_until_task_exit(tmp_path):
    font = (ROOT / "main/pet_p4_font_delivery.inc").read_text()
    header = tmp_path / "freertos/FreeRTOS.h"
    header.parent.mkdir()
    header.write_text("#define portMUX_TYPE int\n#define portMUX_INITIALIZER_UNLOCKED 0\n"
                      "#define portENTER_CRITICAL(m) ((void)(m))\n#define portEXIT_CRITICAL(m) ((void)(m))\n")
    source = r'''
#define PET_FS_ACTIVITY_TEST 1
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include "pet_p4_fs_activity.c"
#define FONT_STAGE "stage"
static atomic_bool font_loading;
static bool verified;
static unsigned removals;
static struct {void *file; bool writing; int64_t last_us;} font_upload;
static int64_t esp_timer_get_time(void){return 15000001;}
static void observed(void){uint64_t e;assert(!pet_fs_activity_idle_epoch(&e));}
static void font_upload_close(void){observed();font_upload.file=NULL;}
static int remove(const char *p){assert(p);observed();removals++;return 0;}
static bool pet_shared_font_activate_staged(const char *p){assert(p);observed();return verified;}
static void vTaskDelete(void *task){uint64_t e;assert(!task&&!font_loading&&pet_fs_activity_idle_epoch(&e));}
''' + function(font, "pet_p4_font_delivery_expire") + "\n" + function(font, "font_load_worker") + r'''
int main(void){
 pet_fs_activity_enable_observation();
 uint64_t before,after;
 assert(pet_fs_activity_idle_epoch(&before));
 pet_p4_font_delivery_expire();
 assert(pet_fs_activity_idle_epoch(&after)&&after==before);
 font_upload.file=&font_upload;
 pet_p4_font_delivery_expire();
 assert(removals==1&&pet_fs_activity_idle_epoch(&after)&&after==before+2);
 for(unsigned i=0;i<2;i++){
  before=after;verified=i;font_loading=true;font_load_worker(NULL);
  assert(removals==2&&pet_fs_activity_idle_epoch(&after)&&after==before+2);
 }
}
'''
    path = tmp_path / "font-worker.c"
    path.write_text(source)
    exe = tmp_path / "font-worker"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", "-fsanitize=address,undefined",
                    "-I", tmp_path, "-I", ROOT / "main", path, "-o", exe], check=True)
    subprocess.run([exe], check=True)
