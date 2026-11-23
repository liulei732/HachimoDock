"""Bounded LAN key queue and wire/producer invariants; no device or network."""
from pathlib import Path
import json
import re
import subprocess
ROOT = Path(__file__).resolve().parents[1]

def test_queue_lifecycle(tmp_path):
    exe = tmp_path / 'lan-press'
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I', str(ROOT / 'main'), str(ROOT / 'tests/p4_lan_press_test.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

def test_status_current_old_peer_and_future_version_capacity():
    source = (ROOT / 'main/pet_p4_lan.c').read_text()
    section = source.split('const bool built =', 1)[1].split('const bool encoded =', 1)[0]
    version = re.search(r'set\(PROJECT_VER "([^"]+)"\)', (ROOT / 'CMakeLists.txt').read_text())[1] + '-wifi6'
    fields = {}
    for kind, name in re.findall(r'cJSON_Add(String|Bool|Number)ToObject\(status, "([^"]+)"', section):
        fields[name] = False if kind == 'Bool' else (4294967295 if kind == 'Number' else
            {'firmware':version, 'boardDeviceId':'p4-0123456789ab', 'bootId':'0123456789abcdef'}[name])
    fields['rtcKey'] = 7
    # Production socket error encodes 0x20000|uint16_t errno; other exits are smaller.
    fields['wirelessRealtimeLastExit'] = 0x2ffff
    # Fixed true capabilities cannot become false in this exact schema.
    for name in re.findall(r'cJSON_AddBoolToObject\(status, "([^"]+)", true\)', section):
        fields[name] = True
    wire = json.dumps(fields, separators=(',', ':')).encode()
    assert len(wire) <= 512  # old PC response cap, exact current public version
    fields['firmware'] = 'v' * 31  # maximum NUL-terminated ESP app descriptor version
    assert '!extended || (cJSON_AddNumberToObject(status, "ac"' in source
    assert 'cJSON_AddBoolToObject(status, "wd", true)' in source
    assert '(extended ? (8 | PET_LAN_STATUS_GLOBAL_CUES) : 0)' in section  # timed captions on an extended RTC status request
    assert '(extended ? 32 : 0)' in section
    fields['rtcKey'] |= 8 | 16 | 32 | 64 | 128
    fields['ac'] = 16 * 1024 * 1024
    fields['wd'] = True
    fields.update(a=3, ptt=True, pf=False)
    future = json.dumps(fields, separators=(',', ':')).encode()
    assert len(future) <= 576
    assert "LAN_STATUS_MAX + 6, false)" in source  # print slack outside the wire budget
    assert "strlen((char *)plain) <= LAN_STATUS_MAX" in source
    assert '#define LAN_STATUS_MAX 576' in source
    pc = (ROOT.parent / 'pc/src-tauri/src/lan_agent.rs').read_text()
    assert 'const DEVICE_STATUS_MAX: usize = 576;' in pc

def test_legacy_probe_and_producer_routes_preserved():
    lan = (ROOT / 'main/pet_p4_lan.c').read_text()
    assert 'case 0: case LAN_OTA_MODE: if (size) return;' in lan
    assert 'case LAN_STATUS_MODE: if (size > 1) return;' in lan
    assert 'case LAN_PRESS_MODE: if (size != 36) return;' in lan
    assert 'client_seen_ms = now;' in lan
    assert 'request_generation == generation && paired' in lan
    assert '!atomic_load(&usb_client_active)' in lan
    assert 'memcmp(plain, pet_p4_boot_identity(), 16)' in lan
    source = (ROOT / 'main/pet_p4_input.c').read_text()
    producer = source.split('static void send_input_event(', 1)[1].split('/* R5 DSH', 1)[0]
    assert '#if PET_P4_SYSTEM_WIFI' in producer
    assert 'pet_p4_lan_realtime_press(state, event->ts_ms,' in producer
    assert producer.index('pet_p4_lan_realtime_press(state,') < producer.index('cJSON_CreateObject()')
    assert '    return;\n  }\n#endif' in producer
    assert 'pet_p4_lan_realtime_press' not in source.split('static void send_input_event(',1)[0]
    protocol = (ROOT / 'main/pet_p4_protocol.c').read_text()
    assert protocol.count('if (state->desktop_device_id[0]) pet_p4_lan_usb_active();') == 3

def test_component_descriptors_reuse_usb_build_and_storage_limits():
    protocol = (ROOT / "main/pet_p4_protocol.c").read_text()
    lan = (ROOT / "main/pet_p4_lan.c").read_text()
    compact = protocol.split("void pet_p4_add_bundle_capabilities(cJSON *capabilities) {", 1)[1].split("\n}", 1)[0]
    full = protocol.split("void pet_p4_add_component_capabilities(cJSON *capabilities) {", 1)[1].split("\n}", 1)[0]
    usb = protocol.split("void pet_p4_send_hello_for_board(", 1)[1].split("\n}", 1)[0]
    report = lan.split("static bool send_component_capabilities(", 1)[1].split("\n}", 1)[0]
    assert "pet_p4_add_component_capabilities(capabilities)" in usb
    assert "pet_p4_add_bundle_capabilities(capabilities)" in full
    assert "pet_p4_add_component_capabilities(caps)" in report and "pet_p4_add_bundle_capabilities(caps)" in report
    assert "full ? LAN_MAX_BODY - 2 : 1024" in report
    for key, limit in [("widgetWasmMaxBytes", "HACHI_MAX_MODULE"), ("widgetWasmMemoryPages", "HACHI_MAX_MEMORY_PAGES"),
                       ("widgetTransferChunkBytes", "PET_P4_WIDGET_CHUNK_BYTES"), ("widgetTransferMaxFiles", "WIDGET_TRANSFER_FILES_MAX"),
                       ("widgetTransferMaxBytes", "WIDGET_TRANSFER_BYTES_MAX"), ("widgetWasmAudioMaxBytes", "PET_CEXT_SPEECH_MAX_BYTES")]:
        assert re.search(f'"{key}",\\s*{limit}', compact)
        assert key not in report

def test_staged_component_publication_requires_current_live_connection(tmp_path):
    source = (ROOT / "main/pet_p4_lan_component.inc").read_text()
    def function(name):
        return "static bool " + name + source.split("static bool " + name, 1)[1].split("\n}", 1)[0] + "\n}"
    fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <unistd.h>
#define LAN_BUNDLE_MODE 14
static atomic_bool control_trusted = true, forget_pending;
static bool online = true;
static unsigned association = 2;
static int64_t now = 1;
static bool pet_device_wifi_online(void) { return online; }
static unsigned pet_device_wifi_association(void) { return association; }
static int64_t esp_timer_get_time(void) { return now; }
struct job { unsigned association; bool repair_only, repair_current; };
static bool repair_route_current(const struct job *job) { return job->repair_current; }
static struct { int fd, operation; struct job *bundle; int64_t until; bool publication_started; } delivery;
'''
    fixture += function("component_socket_live") + "\n" + function("component_publish")
    fixture += r'''
int main(void) {
    int fd[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
    assert(fcntl(fd[0], F_SETFL, O_NONBLOCK) == 0);
    struct job job = {.association = 2};
    delivery.fd = fd[0]; delivery.bundle = &job; delivery.operation = LAN_BUNDLE_MODE; delivery.until = 5;
    assert(component_socket_live(NULL));
    association++; assert(!component_publish(NULL)); association--;
    now = 5; assert(!component_publish(NULL)); now = 4;
    online = false; assert(!component_publish(NULL)); online = true;
    forget_pending = true; assert(!component_publish(NULL)); forget_pending = false;
    assert(!delivery.publication_started);
    job.repair_only = true; assert(!component_publish(NULL));
    job.repair_current = true; assert(component_publish(NULL) && delivery.publication_started);
    delivery.publication_started = false;
    close(fd[1]); assert(!component_publish(NULL) && !delivery.publication_started);
    close(fd[0]);
}
'''
    cfile, exe = tmp_path / "publish.c", tmp_path / "publish"
    cfile.write_text(fixture)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", cfile, "-o", exe], check=True)
    subprocess.run([exe], check=True)
