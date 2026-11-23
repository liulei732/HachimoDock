"""Execute the actual state-lock callbacks, including builds without assertions."""
from pathlib import Path
import subprocess
from p4_render_trace_test import function

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("assertions", [True, False])
def test_storage_lock_callbacks_do_not_depend_on_assert_side_effects(tmp_path, assertions):
    main = (ROOT / "main/pet_p4_main.c").read_text()
    callback_source = "\n".join(function(main,n) for n in ("storage_command_owner","storage_command_unlock","storage_command_lock"))
    assertion = "assert(condition)" if assertions else "((void)0)"
    source = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "pet_p4_storage_io_trace.h"
#include "pet_p4_storage_lane.h"
#include "pet_p4_task_policy.h"
typedef int BaseType_t;
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffU
static int g_state_mutex = 1;
static uintptr_t task = 7, holder = 7;
static uintptr_t g_protocol_task = 7;
static bool g_protocol_storage_background;
static _Atomic bool g_storage_query_window;
static unsigned takes, gives, suspended, priority = PET_P4_PROTOCOL_TASK_PRIORITY, priority_changes;
#define configASSERT(condition) ASSERTION
static uintptr_t xTaskGetCurrentTaskHandle(void) { return task; }
static __attribute__((unused)) uintptr_t xSemaphoreGetMutexHolder(int lock) {
  assert(lock == 1); return holder;
}
static BaseType_t xSemaphoreGive(int lock) {
  assert(lock == 1 && holder == task && suspended && atomic_load(&g_storage_query_window)); holder = 0; gives++; return pdTRUE;
}
static BaseType_t xSemaphoreTake(int lock, unsigned wait) {
  assert(lock == 1 && wait == portMAX_DELAY && !holder && !atomic_load(&g_storage_query_window));
  assert(priority == PET_P4_PROTOCOL_TASK_PRIORITY && !g_protocol_storage_background);
  holder = task; takes++; return pdTRUE;
}
static void vTaskPrioritySet(uintptr_t owner, unsigned next) {
  assert(owner == task && task == g_protocol_task && !holder);
  assert(next == PET_P4_STORAGE_IO_TASK_PRIORITY || next == PET_P4_PROTOCOL_TASK_PRIORITY);
  assert(g_protocol_storage_background == (next == PET_P4_STORAGE_IO_TASK_PRIORITY));
  priority = next; priority_changes++;
}
static void pet_p4_renderer_set_asset_preparation_suspended(bool value) {
  assert(holder == task && value); suspended++;
}
'''.replace("ASSERTION", assertion) + callback_source + r'''
int main(void) {
  pet_storage_lane_t lane = {0}; pet_storage_scope_t scope = {0};
  assert(pet_storage_lane_configure(&lane, storage_command_unlock,
    storage_command_lock, storage_command_owner, 0));
  /* Registration alone must not release a lock on boot/native fallback. */
  assert(pet_storage_scope_begin(&lane, &scope));
  assert(pet_storage_scope_unlock(&scope)); assert(holder == task && !gives);
  assert(priority_changes == 0 && priority == PET_P4_PROTOCOL_TASK_PRIORITY);
  assert(pet_storage_scope_finish(&scope));
  assert(pet_storage_lane_enter(&lane));
  assert(pet_storage_scope_begin(&lane, &scope));
  assert(pet_storage_scope_unlock(&scope)); assert(!holder && gives == 1 && suspended == 1);
  assert(priority == PET_P4_STORAGE_IO_TASK_PRIORITY && priority_changes == 1);
  /* Failure exit reacquires before returning to handle_line's caller. */
  assert(pet_storage_scope_finish(&scope)); assert(holder == task && takes == 1);
  assert(priority == PET_P4_PROTOCOL_TASK_PRIORITY && priority_changes == 2);
  assert(pet_storage_lane_leave(&lane));
  /* A non-protocol native fallback never alters any task's scheduling base. */
  task = holder = 8;
  assert(pet_storage_lane_enter(&lane));
  assert(pet_storage_scope_begin(&lane, &scope));
  assert(pet_storage_scope_unlock(&scope));
  assert(priority_changes == 2 && !g_protocol_storage_background);
  assert(pet_storage_scope_finish(&scope));
  assert(pet_storage_lane_leave(&lane));
  return 0;
}
'''
    path, exe = tmp_path / "integration.c", tmp_path / "integration"
    path.write_text(source)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(ROOT / "main"),
                    str(path), str(ROOT / "main/pet_p4_storage_lane.c"),
                    "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


def test_command_scope_and_suspension_integration_leave_render_cadence_unchanged():
    main = (ROOT / "main/pet_p4_main.c").read_text()
    worker = main.split("static void protocol_worker_task(", 1)[1].split("static bool consume_byte(", 1)[0]
    # Alarm maintenance temporarily owns the state mutex before dequeue. It
    # releases the storage lane on admission and the state mutex even on refusal.
    alarm = worker.split("if (pet_local_alarm_work())", 1)[1].split("if (xQueueReceive", 1)[0]
    assert alarm.count("xSemaphoreTake(g_state_mutex") == alarm.count("xSemaphoreGive(g_state_mutex)") == 1
    assert alarm.index("xSemaphoreTake(g_state_mutex") < alarm.index("pet_p4_miniapp_storage_lane_enter()") < alarm.index("pet_local_alarm_run();")
    assert alarm.index("pet_local_alarm_run();") < alarm.index("pet_p4_miniapp_storage_lane_leave()") < alarm.index("xSemaphoreGive(g_state_mutex)")
    worker = worker.split("if (!handled_debug) {", 1)[1]
    assert worker.index("xSemaphoreTake(g_state_mutex") < worker.index("pet_p4_miniapp_storage_lane_enter()")
    assert worker.index("pet_p4_miniapp_storage_lane_enter()") < worker.index("pet_p4_handle_line(")
    assert worker.index("pet_p4_handle_line(") < worker.index("pet_p4_miniapp_storage_lane_leave()")
    assert worker.index("pet_p4_miniapp_storage_lane_leave()") < worker.index("xSemaphoreGive(g_state_mutex)")
    loop = main[main.index('    pet_p4_asset_transfer_process(&g_state, now_ms);'):]
    compact = ''.join(loop.split())
    suspension = ('pet_p4_renderer_set_asset_preparation_suspended('
                  'transfer_active||pet_p4_miniapp_storage_busy()||pet_p4_appearance_active());')
    assert compact.index('booltransfer_active=g_state.asset_transfer_active||firmware_transfer_active;') < compact.index(suspension) < compact.index('xSemaphoreGive(g_state_mutex);')
    # All three causes stop new preparation, but only the original transfer
    # flag selects the 500ms UI. Actual borrow/relock assertions run above
    # with configASSERT both enabled/disabled; activation interleavings also
    # run in p4_phase2_transaction_test (no device timing claim).
    assert 'else if (lcd_err == ESP_OK && !transfer_active' in loop
    # Storage need not stop rendering or reduce it to the transfer UI's 500ms.
    assert "bool transfer_active = g_state.asset_transfer_active || firmware_transfer_active;" in main
    assert "&g_protocol_task" in main


def test_alarm_maintenance_releases_only_locks_it_acquired(tmp_path):
    main = (ROOT / "main/pet_p4_main.c").read_text()
    worker = function(main, "protocol_worker_task")
    # Compile the production pre-dequeue slice, including its feature guards.
    end = worker.index("    if (xQueueReceive(g_protocol_queue")
    start = worker.rindex("#if PET_P4_EVENTS", 0, end)
    maintenance = worker[start:end]
    source = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define PET_P4_EVENTS 1
#define PET_P4_GAP_PEAK_HOLDER 1
#define pdTRUE 1
#define configASSERT assert
static struct { bool asset_transfer_active, approval_active; } g_state;
static int g_state_mutex = 1;
static bool work, idle, available, admitted, held, lane, timed, ota, conversation;
static unsigned runs, takes, gives, entries, leaves, spans;
static uint64_t now = 1000;
static uint32_t g_worker_locked_topic_hash;
static uint32_t pet_topic_hash(const char *topic, unsigned n) { assert(topic && n); return 7; }
static uint64_t esp_timer_get_time(void) { return now * 1000; }
static bool pet_local_alarm_work(void) { return work; }
static bool volume_worker_idle(void) { return idle; }
static bool pet_p4_ota_active(void) { return ota; }
static bool pet_p4_conversation_active(void *state) { assert(state == &g_state); return conversation; }
static int xSemaphoreTake(int m, int wait) {
  assert(m == 1 && !wait && !held); if (!available) return 0; ++takes; return held = true;
}
static void xSemaphoreGive(int m) { assert(m == 1 && held && !lane && !timed); ++gives; held = false; }
static bool pet_p4_miniapp_storage_lane_enter(void) {
  assert(held && !lane); ++entries; return lane = admitted;
}
static bool pet_p4_miniapp_storage_lane_leave(void) {
  assert(held && lane); ++leaves; lane = false; return true;
}
static void worker_locked_begin(void) { assert(held && !timed && g_worker_locked_topic_hash == 7); timed = true; }
static void worker_locked_end(void) { assert(held && timed); ++spans; timed = false; }
static void pet_local_alarm_run(void) { assert(held && lane && timed); ++runs; }
static void maintain(void) {
""" + maintenance + r"""
}
static void step(void) {
  now += 1000; maintain(); assert(!held && !lane && !timed && !g_worker_locked_topic_hash);
  assert(takes == gives && gives == spans);
}
int main(void) {
  idle = available = admitted = true; step(); assert(!takes);
  work = true; idle = false; step(); assert(!takes);
  idle = true; available = false; step(); assert(!takes);
  available = true; g_state.asset_transfer_active = true; step();
  g_state.asset_transfer_active = false; ota = true; step();
  ota = false; conversation = true; step();
  conversation = false; g_state.approval_active = true; step();
  assert(takes == 4 && !entries && !runs && !leaves);
  g_state.approval_active = false; admitted = false; step();
  assert(takes == 5 && entries == 1 && !runs && !leaves);
  admitted = true; step(); assert(takes == 6 && entries == 2 && runs == 1 && leaves == 1);
  maintain(); assert(takes == 6 && gives == 6 && runs == 1);
}
"""
    fixture = tmp_path / "alarm_maintenance.c"
    fixture.write_text(source)
    binary = fixture.with_suffix("")
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", str(fixture), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
