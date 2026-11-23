"""Exercise the actual Wi-Fi and LAN settings pages with fake radio and pairing state."""
from pathlib import Path
import subprocess


def test_local_wifi_settings_keyboard_and_navigation(tmp_path):
    firmware = Path(__file__).resolve().parents[1]
    tests = firmware / "tests"
    executable = tmp_path / "system-settings-test"
    (tmp_path / "mbedtls").mkdir()
    (tmp_path / "mbedtls/platform_util.h").write_text("#include <stddef.h>\nvoid mbedtls_platform_zeroize(void *, size_t);\n")
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-DPET_P4_SYSTEM_WIFI=1", "-DCONFIG_BT_NIMBLE_ENABLED=1", "-DPET_P4_SCOPED_WRITE=1",
         "-I", str(tmp_path), "-I", str(firmware / "main"), "-I", str(tests / "audio_volume_stubs"),
         "-I", str(tests / "vendor/cjson"),
         str(firmware / "main/pet_p4_system_settings.c"),
         str(tests / "p4_system_settings_test.c"), "-o", str(executable)],
        check=True,
    )
    subprocess.run([str(executable)], check=True)


def test_local_settings_worker_ownership(tmp_path):
    main = (Path(__file__).resolve().parents[1] / "main/pet_p4_main.c").read_text()
    worker = main[main.index("static void settings_av_worker("):main.index("bool pet_community_schedule(")]
    harness = tmp_path / "av.c"
    harness.write_text(r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef int esp_err_t;
enum {ESP_OK,portMAX_DELAY};
static int g_state_mutex,locked,sets,gets,saves,finishes,shown,level=50,light=30,draftvol,draftlight;
static bool active=true,stale,storage,reported,panel_ok=true,codec=true;
static struct {bool approval_active,asset_transfer_active;} g_state;
static void xSemaphoreTake(int m,int delay) {(void)m;(void)delay;assert(!locked++);}
static void xSemaphoreGive(int m) {(void)m;assert(locked--==1);}
static bool pet_settings_active(void *s) {return s==&g_state && active;}
static bool pet_settings_av_claim(uint32_t t,int *v,int *b) {assert(locked && t==1);*v=draftvol;*b=draftlight;return !stale;}
static bool brightness_committed;
static void settings_volume_receipt(void) {assert(locked);}
static void pet_settings_av_finish(uint32_t t,int v,int b,bool ok,bool saved) {brightness_committed=saved;assert(locked && t==1 && b==light);shown=v;reported=ok;++finishes;}
static bool pet_p4_ota_transfer_active(void) {return false;}
static bool pet_p4_diagnostics_reboot_pending(void) {return false;}
static bool pet_p4_miniapp_storage_busy(void) {return storage;}
static bool pet_p4_conversation_active(void *s) {(void)s;return false;}
static bool pet_p4_audio_master_idle(void) {return true;}
static bool pet_p4_audio_playback_ready(void) {return codec;}
typedef struct {int volume;bool confirmed;} pet_audio_volume_status_t;
static esp_err_t pet_audio_volume_get(pet_audio_volume_status_t *r) {assert(!locked);++gets;r->volume=level;r->confirmed=true;return ESP_OK;}
static esp_err_t pet_audio_volume_set(unsigned v,uint64_t t,pet_audio_volume_status_t *r) {
  assert(!locked && t==1);++sets;level=v;r->volume=level;r->confirmed=true;return ESP_OK;
}
static int64_t esp_timer_get_time(void) {return 1000;}
static int pet_p4_lcd_brightness(void) {return light;}
static bool pet_p4_lcd_save_brightness(int v) {assert(!locked);++saves;if(panel_ok) light=v;return panel_ok;}
""" + worker + r"""
int main(void) {
  draftvol=draftlight=-1;settings_av_worker(1);assert(gets==1 && !sets && !saves && shown==50 && reported);
  draftvol=90; draftlight=40;settings_av_worker(1);assert(sets==1 && saves==1 && shown==90 && light==40 && reported && brightness_committed);
  stale=true;settings_av_worker(1);assert(finishes==2 && sets==1 && saves==1);
  stale=false;storage=true;settings_av_worker(1);assert(finishes==3 && !reported && sets==1 && saves==1);
  storage=false;g_state.approval_active=true;settings_av_worker(1);assert(finishes==4 && !reported && sets==1);
  g_state.approval_active=false;codec=false;panel_ok=false;draftlight=60;settings_av_worker(1);
  assert(shown==-1 && !reported && light==40 && saves==2 && sets==1 && !brightness_committed);
}
""")
    executable = tmp_path / "av"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(harness), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
