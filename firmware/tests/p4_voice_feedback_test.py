"""Run the production feedback state machine on the host."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_voice_capture_and_confirmation_lifecycle(tmp_path):
    executable = tmp_path / "voice-feedback"
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-I", str(ROOT / "main"),
        str(ROOT / "main/pet_p4_voice_feedback.c"),
        str(ROOT / "tests/p4_voice_feedback_test.c"), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)


def test_physical_confirmation_writes_the_existing_input_event_once(tmp_path):
    source = (ROOT / "main/pet_p4_input.c").read_text()
    start = source.index("static void send_input_event(")
    emit = source[start:source.index("/* R5 DSH approval", start)]
    harness = tmp_path / "voice-input.c"
    harness.write_text(r'''
#include "pet_p4_protocol.h"
#include "cJSON.h"
#include <assert.h>
#include <stdatomic.h>
#include <string.h>

#define PET_P4_INPUT_CONFIG_VERSION 1
typedef struct { unsigned control; int delta; uint64_t ts_ms; } pet_p4_input_event_t;
typedef struct { char action[20], value[20]; } pet_p4_input_binding_t;
static unsigned g_event_sequence, allowed, refused;
static atomic_uint g_dropped_events;
static uint64_t now;
static uint64_t esp_timer_get_time(void) { return now * 1000; }
static const char *control_name(unsigned control) { return control ? "encoder" : "sw1"; }
static bool pet_p4_ota_transfer_active(void) { return false; }
static bool pet_p4_audio_conversation_active(void) { return false; }
static bool pet_p4_conversation_active(const pet_p4_runtime_state_t *s) { (void)s; return false; }
bool pet_p4_state_usb_client_connected(const pet_p4_runtime_state_t *s, unsigned long long t) {
  return s && t;
}

static void inspect(const char *line, void *ctx) {
  (void)ctx;
  cJSON *root = cJSON_Parse(line);
  assert(root && !strcmp(cJSON_GetObjectItem(root, "topic")->valuestring, "input/event"));
  const cJSON *payload = cJSON_GetObjectItem(root, "payload");
  const cJSON *visible = cJSON_GetObjectItem(payload, "voiceDraftVisible");
  const cJSON *capture = cJSON_GetObjectItem(payload, "voiceUtteranceId");
  const cJSON *token = cJSON_GetObjectItem(payload, "voiceFeedbackToken");
  assert(cJSON_IsBool(visible));
  if (cJSON_IsTrue(visible)) {
    assert(capture && !strcmp(capture->valuestring, "p4-audio-123-4"));
    assert(cJSON_IsNumber(token) && token->valueint == 1);
    ++allowed;
  } else {
    assert(!capture && !token);
    ++refused;
  }
  cJSON_Delete(root);
}
''' + emit + r'''
static void press(pet_p4_runtime_state_t *state, unsigned control, uint64_t timestamp) {
  pet_p4_input_event_t event = {.control=control, .ts_ms=timestamp};
  send_input_event(state, inspect, NULL, &event, "short_press", "short_press", NULL, "page_enter", false);
}

int main(void) {
  pet_p4_runtime_state_t state = {.screen_page="main", .board_device_id="test"};
  uint32_t token = pet_voice_feedback_begin(&state.voice_feedback, 10);
  pet_voice_meter_t meter = {.token=token, .capture_id="p4-audio-123-4", .ended=true};
  assert(pet_voice_feedback_sync(&state.voice_feedback, &meter, 15));
  assert(pet_voice_feedback_update(&state.voice_feedback, token, meter.capture_id,
    1, "draft_ready", "ready", true, 20));
  now = 40;
  press(&state, 0, 38);  /* No successful render yet. */
  state.voice_feedback.presented_ms = 30;
  press(&state, 0, 29);  /* Old deferred input. */
  pet_p4_runtime_state_t expired = state;
  press(&state, 1, 35);  /* The encoder can confirm this draft once. */
  press(&state, 0, 36);
  assert(allowed == 1 && refused == 3 && state.voice_feedback.phase == PET_VOICE_SUBMITTING);
  now = 12030;
  press(&expired, 0, 12029);
  assert(allowed == 1 && refused == 4);
}
''')
    executable = tmp_path / "voice-input"
    cjson = ROOT / "tests/vendor/cjson"
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations",
        "-I", str(ROOT / "main"), "-I", str(cjson),
        str(ROOT / "main/pet_p4_voice_feedback.c"), str(cjson / "cJSON.c"),
        str(harness), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
