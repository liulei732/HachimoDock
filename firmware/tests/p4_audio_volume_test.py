"""No SDK build or hardware: compile actual pinned generated vendor functions."""
import hashlib
import importlib.util
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / "managed_components/espressif__esp_codec_dev"
SPEC = importlib.util.spec_from_file_location("volume_patch", ROOT / "tools/patch_audio_master_volume.py")
PATCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PATCH)


def test_pinned_copy_refuses_unknown_source_without_writes(tmp_path):
    import shutil
    import pytest
    vendor = tmp_path / "vendor"
    for name in PATCH.PINS:
        dest = vendor / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(VENDOR / name, dest)
    (vendor / "esp_codec_dev.c").write_text("changed")
    with pytest.raises(ValueError, match="unrecognized"):
        PATCH.generate(vendor, tmp_path / "out")
    assert not (tmp_path / "out").exists()
    with pytest.raises(ValueError, match="separate"):
        PATCH.generate(VENDOR, VENDOR / "generated")
    output = tmp_path / "symlink-out"
    output.mkdir()
    (output / "esp_codec_dev.c").symlink_to(VENDOR / "esp_codec_dev.c")
    with pytest.raises(ValueError, match="escapes"):
        PATCH.generate(VENDOR, output)


def test_actual_vendor_errors_cache_and_task_scoped_total_deadline(tmp_path):
    before = {p: hashlib.sha256((VENDOR / p).read_bytes()).hexdigest() for p in PATCH.PINS}
    generated = tmp_path / "generated"
    PATCH.generate(VENDOR, generated)
    includes = [ROOT / "tests/audio_volume_stubs", ROOT / "main", VENDOR, VENDOR / "include",
                VENDOR / "interface", VENDOR / "device/include", VENDOR / "device/priv_include",
                VENDOR / "device/es8311"]
    sources = [generated / p for p in PATCH.PINS] + [VENDOR / "esp_codec_dev_vol.c",
               VENDOR / "esp_codec_dev_if.c", ROOT / "main/pet_p4_audio_volume_deadline.c",
               ROOT / "main/pet_p4_audio_volume.c", ROOT / "main/pet_p4_resources.c",
               ROOT / "tests/vendor/cjson/cJSON.c", ROOT / "tests/p4_audio_volume_vendor_test.c"]
    includes += [ROOT / "tests/vendor/cjson"]
    for sdk in (50501, 50504):
        exe = tmp_path / f"vendor-{sdk}"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare", "-Wno-unused-parameter", "-Wno-deprecated-declarations",
                        "-fsanitize=address,undefined", "-pthread", "-DCONFIG_CODEC_ES8311_SUPPORT=1",
                        f"-DESP_IDF_VERSION={sdk}", *[arg for p in includes for arg in ("-I", str(p))],
                        *map(str, sources), "-lm", "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=20)
        subprocess.run([str(exe), "fresh"], check=True, timeout=20)
        subprocess.run([str(exe), "corrupt"], check=True, timeout=20)
        subprocess.run([str(exe), "out-of-range"], check=True, timeout=20)
        subprocess.run([str(exe), "boot-mute"], check=True, timeout=20)
        subprocess.run([str(exe), "boot-low"], check=True, timeout=20)
    assert before == {p: hashlib.sha256((VENDOR / p).read_bytes()).hexdigest() for p in PATCH.PINS}


def test_exact_cmake_snippet_replaces_only_three_sources_and_optimizes_only_cold_modules(tmp_path):
    import json
    import shutil
    import sys
    main = tmp_path / "source/main"
    main.mkdir(parents=True)
    (main.parent / "tools").mkdir()
    shutil.copyfile(ROOT / "tools/patch_audio_master_volume.py", main.parent / "tools/patch_audio_master_volume.py")
    for name in ("pet_p4_audio_volume.c", "pet_p4_audio_volume_deadline.c"):
        shutil.copyfile(ROOT / "main" / name, main / name)
    (main / "dummy.c").write_text("int volume_cmake_fixture;\n")
    (main / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.16)
project(volume_fixture C)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(PYTHON "{sys.executable}")
set(COMPONENT_LIB fixture_main)
add_library(fixture_main STATIC dummy.c)
add_library(fixture_codec STATIC
  "{VENDOR}/esp_codec_dev.c" "{VENDOR}/device/es8311/es8311.c"
  "{VENDOR}/platform/audio_codec_ctrl_i2c.c" "{VENDOR}/esp_codec_dev_vol.c")
function(idf_component_get_property out component property)
  if(property STREQUAL "COMPONENT_LIB")
    set(${{out}} fixture_codec PARENT_SCOPE)
  else()
    set(${{out}} "{VENDOR}" PARENT_SCOPE)
  endif()
endfunction()
''' + PATCH.cmake_integration())
    build = tmp_path / "build"
    subprocess.run(["cmake", "-S", str(main), "-B", str(build)], check=True, capture_output=True, text=True)
    commands = json.loads((build / "compile_commands.json").read_text())
    for rel in PATCH.PINS:
        assert sum(c["file"] == str(build / "pet_audio_master_volume_codec" / rel) for c in commands) == 1
        assert all(c["file"] != str(VENDOR / rel) for c in commands)
    assert any(c["file"] == str(VENDOR / "esp_codec_dev_vol.c") for c in commands)
    for c in commands:
        assert ("-Os" in c["command"]) == (Path(c["file"]).name in ("pet_p4_audio_volume.c", "pet_p4_audio_volume_deadline.c"))


def test_actual_audio_idle_gate_covers_armed_tail_pending_and_existing_lock(tmp_path):
    source = (ROOT / "main/pet_p4_audio.c").read_text()
    gate = "bool pet_p4_audio_master_idle(void)" + source.split("bool pet_p4_audio_master_idle(void)", 1)[1].split("\n}\n", 1)[0] + "\n}\n"
    unit = tmp_path / "idle.c"
    unit.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
static atomic_bool g_capture_requested,g_conversation_starting,g_stream_active;
static atomic_bool g_fruit_enabled,g_instrument_enabled;
static atomic_uint g_fruit_pending,g_instrument_pending,g_sfx_pending;
static bool capture,conversation,stream,music,playback,queue,busy,voices,sequence;
static int lock_depth;
static void *g_playback_queue=(void *)1,*g_stream_lock=(void *)1;
static int g_sfx_core;
static struct {float gain;} g_instrument_core;
#define pdTRUE 1
static bool pet_p4_audio_active(void){return capture;}
static bool pet_p4_audio_conversation_active(void){return conversation;}
static bool pet_p4_audio_stream_playing(void){return stream;}
static bool pet_p4_audio_music_active(void){return music;}
static bool pet_p4_audio_playback_active(void){return playback;}
static unsigned uxQueueMessagesWaiting(void *q){assert(q);return queue;}
static int xSemaphoreTakeRecursive(void *m,unsigned ticks){assert(m && !ticks);if(busy)return 0;lock_depth++;return 1;}
static void xSemaphoreGiveRecursive(void *m){assert(m && lock_depth==1);lock_depth--;}
static bool pet_sfx_active(const int *p){(void)p;assert(lock_depth==1);return voices;}
static bool cext_pending(void){assert(lock_depth==1);return sequence;}
#define CEXT_PENDING() cext_pending()
''' + gate + r'''
int main(void){
  assert(pet_p4_audio_master_idle() && !lock_depth);
  atomic_bool *armed[]={&g_capture_requested,&g_conversation_starting,&g_stream_active,&g_fruit_enabled,&g_instrument_enabled};
  for(unsigned i=0;i<sizeof(armed)/sizeof(*armed);i++){
    atomic_store(armed[i],true);assert(!pet_p4_audio_master_idle() && !lock_depth);atomic_store(armed[i],false);
  }
  atomic_uint *pending[]={&g_fruit_pending,&g_instrument_pending,&g_sfx_pending};
  for(unsigned i=0;i<sizeof(pending)/sizeof(*pending);i++){
    atomic_store(pending[i],1);assert(!pet_p4_audio_master_idle() && !lock_depth);atomic_store(pending[i],0);
  }
  bool *actors[]={&capture,&conversation,&stream,&music,&playback,&queue,&busy,&voices,&sequence};
  for(unsigned i=0;i<sizeof(actors)/sizeof(*actors);i++){
    *actors[i]=true;assert(!pet_p4_audio_master_idle() && !lock_depth);*actors[i]=false;
  }
  g_instrument_core.gain=0.001f;assert(!pet_p4_audio_master_idle() && !lock_depth);
  g_instrument_core.gain=0;assert(pet_p4_audio_master_idle() && !lock_depth);
  g_stream_lock=0;assert(!pet_p4_audio_master_idle());
}
''')
    exe = tmp_path / "idle"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", str(unit), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=10)


def test_worker_services_volume_under_heartbeats_and_bounds_busy_checks(tmp_path):
    main = (ROOT / "main/pet_p4_main.c").read_text()
    worker = main.split("static void protocol_worker_task(void *arg) {", 1)[1]
    start = worker.index("  pet_p4_protocol_request_t request;")
    end = worker.index("#if PET_P4_HELLO_ASYNC", start)
    # Run the real scheduling and receive branches. Stub only the surrounding
    # workers, RTOS queue and hardware. Cross a 32-bit tick rollover each run.
    unit = tmp_path / "volume_worker.c"
    unit.write_text(r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <setjmp.h>
typedef uint32_t TickType_t;
typedef int pet_p4_protocol_request_t;
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY UINT32_MAX
static unsigned now_ms, interval, next_request, received, lan_checks, commit_checks;
static unsigned applies, saves, dirty_since;
static bool dirty, lan_pending, busy, probe;
static struct { bool raw; } g_screenshot;
static int g_protocol_queue;
static jmp_buf finished;
static TickType_t xTaskGetTickCount(void) { return UINT32_MAX - 1500u + now_ms; }
static bool pet_p4_program_main_probe_pending(void) { return probe; }
static void pet_p4_program_main_probe_reply(void) {}
static void screenshot_poll(void) {}
static void asset_idle_worker(void) {}
#if PET_P4_SYSTEM_WIFI
static void volume_worker_lan(void) {
    assert(now_ms == (++lan_checks) * 1000);
    if (!busy && lan_pending) { lan_pending = false; ++applies; dirty = true; dirty_since = now_ms; }
}
#endif
static void volume_worker_idle_commit(void) {
    assert(now_ms == (++commit_checks) * 1000);
    if (!busy && dirty && now_ms - dirty_since >= 1000) { dirty = false; ++saves; }
}
static int xQueueReceive(int queue, int *request, TickType_t wait) {
    (void)queue; (void)request;
    if (now_ms == 5000) longjmp(finished, 1);
    assert(wait > 0 && wait <= 1000 - now_ms % 1000);
    if (g_screenshot.raw) assert(wait <= 100);
    if (probe) assert(wait <= 20);
    if (interval && next_request <= now_ms + wait) {
        now_ms = next_request; next_request += interval; ++received;
        busy = now_ms < 2500; return pdTRUE;
    }
    now_ms += wait; busy = now_ms < 2500; return 0;
}
static void worker(void) {
""" + worker[start:end] + r"""
    continue; /* Simulate short request dispatch, including its early returns. */
  }
}
static void run(unsigned rate, bool screenshot, bool fast_probe) {
    interval = next_request = rate; now_ms = received = 0;
    lan_checks = commit_checks = applies = saves = dirty_since = 0;
    lan_pending = busy = true; dirty = !PET_P4_SYSTEM_WIFI;
    g_screenshot.raw = screenshot; probe = fast_probe;
    if (!setjmp(finished)) worker();
    assert(received == (rate ? 5000 / rate : 0));
    assert(commit_checks == 5 && saves == 1 && !dirty);
    assert(lan_checks == (PET_P4_SYSTEM_WIFI ? 5 : 0));
    assert(applies == (PET_P4_SYSTEM_WIFI ? 1 : 0));
}
int main(void) {
    run(500, false, false); run(1, false, false); run(333, false, false);
    run(0, true, false); run(0, false, true); run(0, false, false);
}
""")
    for wifi in (0, 1):
        exe = tmp_path / f"volume-worker-{wifi}"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        f"-DPET_P4_SYSTEM_WIFI={wifi}", str(unit), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=5)
