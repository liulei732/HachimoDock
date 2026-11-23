"""Host checks for the production Wi-Fi profile commits and manual join gate."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "main"


def test_wifi_profile_persistence_and_retry(tmp_path):
    radio = (MAIN / "pet_p4_device_wifi.c").read_text()
    keys = radio[radio.index("#define WIFI_NAMESPACE"):radio.index("#define WIFI_GOT_IP")]
    storage = radio[radio.index("static esp_err_t write_profiles("):radio.index("static void wifi_event(")]
    association = radio[radio.index("static atomic_uint association_revision"):radio.index("static void clear_memory(")]
    callback = radio[radio.index("static void wifi_event("):radio.index("static atomic_uint hosted_probe_step")]
    address = radio[radio.index("static bool join_current("):radio.index("static bool connect_station(")]
    manual = radio[radio.index("static bool rediscover_station("):radio.index("static bool connected_link_lost(")]
    worker = radio[radio.index("static bool connected_link_lost("):radio.index("void pet_device_wifi_start(")]
    fixture = tmp_path / "wifi_profiles.c"
    fixture.write_text(keys + r'''
#include <assert.h>
#include <setjmp.h>
#include <stdatomic.h>
#include "pet_p4_wifi_profiles.h"

typedef int nvs_handle_t;
typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_NVS_NOT_FOUND, ESP_FAIL, NVS_READWRITE, NVS_READONLY };
enum { WIFI_REASON_NO_AP_FOUND = 201, WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT = 15,
       WIFI_REASON_HANDSHAKE_TIMEOUT = 204 };
typedef struct {
  pet_wifi_operation_t operation;
  pet_wifi_ap_t ap;
  pet_wifi_station_t station;
} request_t;

static pet_wifi_profiles_t saved, pending_profiles, flash;
static const pet_wifi_station_t original = {"shared", "good-key", PET_WIFI_PERSONAL};
static pet_wifi_station_t legacy;
static pet_wifi_ap_t legacy_network;
static bool have_legacy = true, have_flash, profiles_on_flash, storage_read_failed;
static unsigned mutations, cut_after, fail_at;
static jmp_buf power_cut;
static pet_wifi_snapshot_t view;
static atomic_bool cancelled, reboot_requested;
static int requests;
static unsigned uxQueueMessagesWaiting(int queue);
static bool join_succeeds, legacy_phy;
static unsigned attempts, scans, script_reason;
static uint8_t channels[2];
enum { WIFI_SCAN_RECORDS = 3 };
typedef struct { uint8_t *ssid; bool show_hidden; } wifi_scan_config_t;
typedef struct { char ssid[33]; uint8_t authmode, primary, bssid[6]; } wifi_ap_record_t;
static wifi_ap_record_t records[WIFI_SCAN_RECORDS];
static bool security_matches(uint8_t requested, uint8_t actual) {return requested==actual;}
static int esp_wifi_scan_start(wifi_scan_config_t *scan, bool wait) {
  assert(wait && !strcmp((char *)scan->ssid,"shared") && scan->show_hidden);
  ++scans; return ESP_OK;
}
static int esp_wifi_scan_get_ap_records(uint16_t *n,wifi_ap_record_t *out) {
  assert(*n==WIFI_SCAN_RECORDS && out==records); return ESP_OK;
}
static int esp_wifi_clear_ap_list(void) {return ESP_OK;}
static uint8_t connected_security;
static unsigned note, joins, auto_joins, restores;
static const char *auto_order[] = {"eligible-a", "eligible-b"};
static pet_wifi_link_status_t link;
static void clear_memory(void *bytes, size_t size) { memset(bytes, 0, size); }

static bool connect_station(const pet_wifi_station_t *station, const pet_wifi_ap_t *hint,
                            bool interactive, bool legacy_phy) {
  assert(station->ssid[0]);
  view.connected[0] = 0;
  if (script_reason) {
    assert(attempts<2 && !legacy_phy);
    channels[attempts++]=hint?hint->channel:0;
    link.disconnect_reason=script_reason;
    if (attempts==2 && hint) assert(hint->bssid[0]==42);
    return attempts==2;
  }
  if (interactive) {
    assert(!legacy_phy);
    ++joins;
    if (!join_succeeds) return false;
  } else if (!strcmp(station->ssid, "prior-live")) {
    assert(!hint && legacy_phy && !strcmp(station->password, "prior-key"));
    ++restores;
  } else {
    assert(!hint && !legacy_phy && auto_joins < 2 && !strcmp(station->ssid, auto_order[auto_joins]));
    if (++auto_joins == 1) return false;
  }
  memcpy(view.connected, station->ssid, sizeof(view.connected));
  connected_security = station->security;
  return true;
}
static int esp_wifi_disconnect(void) { return ESP_OK; }
pet_wifi_link_status_t pet_device_wifi_link_status(void) { return link; }
static void note_join(pet_wifi_join_stage_t stage, int error, bool interactive) {
  assert(interactive);
  if (stage == PET_WIFI_JOIN_SAVE) assert(error != ESP_OK);
  note = stage;
}

static int nvs_open(const char *ns, int mode, nvs_handle_t *out) {
  assert(!strcmp(ns, WIFI_NAMESPACE));
  if (mode == NVS_READONLY && !have_legacy && !have_flash) return ESP_ERR_NVS_NOT_FOUND;
  *out = 1;
  return ESP_OK;
}
static int nvs_get_blob(int nvs, const char *key, void *out, size_t *size) {
  (void)nvs;
  if (!strcmp(key, WIFI_LEGACY_KEY)) {
    assert(*size == sizeof(legacy));
    if (!have_legacy) return ESP_ERR_NVS_NOT_FOUND;
    memcpy(out, &legacy, *size);
  } else {
    assert(!strcmp(key, WIFI_PROFILES_KEY) && *size == sizeof(flash));
    if (!have_flash) return ESP_ERR_NVS_NOT_FOUND;
    memcpy(out, &flash, *size);
  }
  return ESP_OK;
}
/* ESP-IDF NVS operations can reach flash before nvs_commit returns. */
static int nvs_set_blob(int nvs, const char *key, const void *data, size_t size) {
  (void)nvs;
  if (++mutations == fail_at) return ESP_FAIL;
  if (!strcmp(key, WIFI_LEGACY_KEY)) {
    assert(size == sizeof(legacy));
    memcpy(&legacy, data, size);
    have_legacy = true;
  } else {
    assert(!strcmp(key, WIFI_PROFILES_KEY) && size == sizeof(flash));
    memcpy(&flash, data, size);
    have_flash = true;
  }
  if (mutations == cut_after) longjmp(power_cut, 1);
  return ESP_OK;
}
static int nvs_erase_key(int nvs, const char *key) {
  (void)nvs;
  assert(!strcmp(key, WIFI_LEGACY_KEY));
  if (++mutations == fail_at) return ESP_FAIL;
  const bool existed = have_legacy;
  have_legacy = false;
  if (mutations == cut_after) longjmp(power_cut, 1);
  return existed ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}
static int nvs_commit(int nvs) { (void)nvs; return ESP_OK; }
static void nvs_close(int nvs) { (void)nvs; }
''' + storage + r'''
typedef uint32_t TickType_t;
typedef int wifi_config_t;
typedef unsigned EventBits_t;
typedef int esp_event_base_t;
typedef struct { unsigned reason; uint8_t bssid[6]; } wifi_event_sta_disconnected_t;
enum { pdFALSE, pdTRUE, WIFI_IF_STA = 0, WIFI_GOT_IP = 1, WIFI_DISCONNECTED = 2, WIFI_LOST_IP = 4,
       WIFI_TRANSPORT_FAILURE = 8 };
enum { IP_EVENT, WIFI_EVENT, ESP_HOSTED_EVENT };
enum { IP_EVENT_STA_GOT_IP, IP_EVENT_STA_LOST_IP, WIFI_EVENT_STA_DISCONNECTED, ESP_HOSTED_EVENT_TRANSPORT_FAILURE };
#define portENTER_CRITICAL(lock) ((void)0)
#define portEXIT_CRITICAL(lock) ((void)0)
#define link_status link
static atomic_uint transport_failures, disconnect_reason;
static uint8_t attempted_channel, attempted_bssid[6];
static bool event_matches_ap;
static void pet_p4_finish_board_probe(bool ok) { assert(ok); }
#define pdMS_TO_TICKS(n) (n)
#define WIFI_RETRY_FIRST_MS 15000
#define WIFI_RETRY_MAX_MS 120000
static bool started, queued;
static int events;
static atomic_bool request_pending, reboot_ready;
static request_t queued_request;
static TickType_t ticks;
static jmp_buf worker_exit;
static void pet_wifi_clock_load_offset(void) {}
static void pet_wifi_clock_service(bool online) { (void)online; }
bool pet_device_wifi_online(void) { return view.connected[0] != 0; }
static void publish(void) {}
static int esp_wifi_stop(void) { return ESP_OK; }
static int esp_wifi_set_config(int iface, const wifi_config_t *config) {
  assert(iface == WIFI_IF_STA && *config == 0);
  return ESP_OK;
}
static void vTaskSuspend(void *task) { (void)task; longjmp(worker_exit, 1); }
static TickType_t xTaskGetTickCount(void) { return ticks; }
static unsigned uxQueueMessagesWaiting(int queue) { (void)queue; return queued; }
static EventBits_t event_bits;
static EventBits_t xEventGroupClearBits(int group, unsigned bits) {
  assert(group == events);
  EventBits_t before = event_bits;
  event_bits &= ~bits;
  return before;
}
static void xEventGroupSetBits(int group, unsigned bits) { assert(group == events); event_bits |= bits; }
static EventBits_t xEventGroupGetBits(int group) { assert(group == events); return event_bits; }
static EventBits_t xEventGroupWaitBits(int group, unsigned bits, int clear, int all, unsigned timeout) {
  assert(group == events && !all && timeout == 250);
  EventBits_t before = event_bits;
  if (clear && (before & bits)) event_bits &= ~bits;
  return before;
}
static void vTaskDelay(unsigned ticks_to_wait) { assert(ticks_to_wait == 250); }
''' + association + callback + r'''
static wifi_ap_record_t associated;
static unsigned ap_queries;
static int ap_result;
static bool lose_ip_during_query;
static int esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap) {
  ++ap_queries;
  *ap = associated;
  if (lose_ip_during_query) wifi_event(NULL, IP_EVENT, IP_EVENT_STA_LOST_IP, NULL);
  return ap_result;
}
''' + address + manual + r'''
static bool pet_ble_shortcut_restore_requested(void) { return false; }
static bool pet_p4_is_wifi6_hardware(void) { return true; }
static unsigned early_failures, recovery_scans;
static TickType_t worker_limit=2500;
static bool start_radio(void) {
  if (early_failures) { --early_failures; view.status=PET_WIFI_INIT_RETRY; return false; }
  view.status=PET_WIFI_IDLE; return started = true;
}
static void scan(void) { ++recovery_scans; }
static int xQueueReceive(int queue, request_t *out, TickType_t timeout) {
  (void)queue;
  ticks += timeout;
  if (ticks > worker_limit) longjmp(worker_exit, 1);
  if (!queued) return 0;
  *out = queued_request;
  queued = false;
  return pdTRUE;
}
''' + worker + r'''
static void boot(void) {
  saved = pending_profiles = (pet_wifi_profiles_t){0};
  legacy_network = (pet_wifi_ap_t){0};
  view = (pet_wifi_snapshot_t){0};
  legacy_phy = false;
  profiles_on_flash = storage_read_failed = false;
  cut_after = fail_at = mutations = 0;
  load_profiles();
}

static void test_legacy_revoke_power_cut(unsigned cut) {
  legacy = original;
  have_legacy = true;
  have_flash = false;
  boot();
  cut_after = cut;
  if (!setjmp(power_cut)) {
    assert(update_saved_profile(0, PET_WIFI_FORGET) == ESP_OK);
    assert(0);
  }
  boot();
  assert(have_legacy == (cut == 1));
  assert(saved.count == (cut < 3 ? 1 : 0));
  if (cut < 3) {
    assert(saved.items[0].auto_join);
    assert(!strcmp(saved.items[0].station.password, "good-key"));
  }
}

static void test_worker(void) {
  have_legacy = false;
  have_flash = true;
  flash = (pet_wifi_profiles_t){.count = PET_WIFI_SAVED_MAX, .items = {
    {.station = {"manual-a", "", PET_WIFI_OPEN}},
    {.station = {"eligible-a", "", PET_WIFI_OPEN}, .auto_join = 1},
    {.station = {"manual-b", "", PET_WIFI_OPEN}},
    {.station = {"eligible-b", "", PET_WIFI_OPEN}, .auto_join = 1},
    {.station = {"manual-c", "", PET_WIFI_OPEN}},
    {.station = {"manual-d", "", PET_WIFI_OPEN}},
  }};
  boot();
  ticks = 0;
  queued = true;
  queued_request = (request_t){
    .operation = PET_WIFI_CONNECT,
    .ap = {.ssid = "over-capacity", .security = PET_WIFI_OPEN},
  };
  if (!setjmp(worker_exit)) wifi_task(NULL);
  assert(auto_joins == 2 && view.status == PET_WIFI_CONNECTED && !strcmp(view.connected, "eligible-b"));
  assert(!mutations && flash.count == PET_WIFI_SAVED_MAX && !strcmp(flash.items[0].station.ssid, "manual-a"));
  assert(!pet_wifi_profiles_find(&saved, "manual-a", PET_WIFI_OPEN));
}

static void test_first_boot_init_recovery(void) {
  have_legacy=have_flash=started=false;
  boot();
  ticks=0; early_failures=1; queued=true;
  queued_request=(request_t){.operation=PET_WIFI_START};
  worker_limit=WIFI_RETRY_FIRST_MS+1000;
  if (!setjmp(worker_exit)) wifi_task(NULL);
  assert(started && recovery_scans==1 && !saved.count && !mutations);
  worker_limit=2500;
}

static void test_legacy_password_recovery(unsigned failure, unsigned cut) {
  legacy = original;
  have_legacy = true;
  have_flash = false;
  boot();
  const pet_wifi_station_t other = {"keep-other", "", PET_WIFI_OPEN};
  assert(save_joined_station(&other, -1) == ESP_OK);
  request_t request = {.operation = PET_WIFI_CONNECT,
                       .station = {"shared", "updated-key", PET_WIFI_PERSONAL}};
  join_succeeds = true;
  fail_at = failure ? mutations + failure : 0;
  cut_after = cut ? mutations + cut : 0;
  if (!setjmp(power_cut)) {
    join_requested(&request, 1);
    assert(!cut);
    assert(view.status == (failure ? PET_WIFI_FAILED : PET_WIFI_CONNECTED));
  }
  const char *expected = failure == 1 ? "good-key" : "updated-key";
  // What the old firmware can read immediately, before any newer boot.
  assert(have_legacy && !strcmp(legacy.password, expected));
  boot();
  const int found = pet_wifi_profiles_find(&saved, "shared", PET_WIFI_PERSONAL);
  assert(saved.count == 2 && found >= 0 && saved.items[found].legacy_kept);
  assert(!strcmp(saved.items[found].station.password, expected));
  assert(!strcmp(flash.items[found].station.password, expected));
  assert(pet_wifi_profiles_find(&saved, "keep-other", PET_WIFI_OPEN) >= 0);
}

static void test_downgrade_reconcile(void) {
  legacy = original;
  have_legacy = true;
  have_flash = false;
  boot();
  const pet_wifi_station_t other = {"keep-other", "", PET_WIFI_OPEN};
  assert(save_joined_station(&other, -1) == ESP_OK);

  // Old firmware saved a different verified password for the same identity.
  strcpy(legacy.password, "changed-in-old");
  boot();
  assert(saved.count == 2 && saved.items[1].legacy_kept);
  assert(!strcmp(saved.items[1].station.password, "changed-in-old"));
  assert(!strcmp(flash.items[1].station.password, "changed-in-old"));
  pet_wifi_profiles_t before_forget = flash;
  have_legacy = false;
  boot();
  assert(saved.count == 1 && !strcmp(saved.items[0].station.ssid, "keep-other"));
  boot();
  assert(saved.count == 1 && !flash.items[1].station.ssid[0]);

  // Replacing the old slot with a different SSID or security type leaves the
  // newer multi-network list intact. A later forget removes only that old slot.
  for (unsigned changed_security = 0; changed_security < 2; ++changed_security) {
    flash = before_forget;
    have_legacy = true;
    legacy = changed_security
      ? (pet_wifi_station_t){"shared", "", PET_WIFI_OPEN}
      : (pet_wifi_station_t){"new-in-old", "a-good-key", PET_WIFI_PERSONAL};
    boot();
    assert(saved.count == 2 && !saved.items[1].legacy_kept);
    assert(!strcmp(saved.items[1].station.password, "changed-in-old"));
    assert(pet_wifi_profiles_find(&saved, legacy.ssid, legacy.security) < 0);
    have_legacy = false;
    boot();
    assert(saved.count == 2 && !saved.items[1].legacy_kept);
  }
}

static void test_restore_manual_only_live_link(void) {
  have_legacy = have_flash = false;
  boot();
  const pet_wifi_station_t prior = {"prior-live", "prior-key", PET_WIFI_PERSONAL};
  assert(save_joined_station(&prior, -1) == ESP_OK);
  boot();
  assert(saved.items[0].auto_join && pet_wifi_next_auto(&saved, 0) == 0);
  assert(update_saved_profile(0, PET_WIFI_AUTO_OFF) == ESP_OK);
  const unsigned before = mutations;
  memcpy(view.connected, prior.ssid, sizeof(view.connected));
  connected_security = prior.security;
  legacy_phy = true;
  request_t request = {.operation = PET_WIFI_CONNECT,
                       .station = {"prior-live", "wrong-key", PET_WIFI_PERSONAL}};
  join_succeeds = false;
  link.manual_disconnect_reason = link.disconnect_reason = 202;
  join_requested(&request, 0);
  assert(view.status == PET_WIFI_FAILED && view.join_reason == 202);
  assert(restores == 1 && !strcmp(view.connected, prior.ssid));
  assert(mutations == before && !saved.items[0].auto_join);
  assert(!strcmp(saved.items[0].station.password, prior.password));

  boot();
  ticks = 0;
  if (!setjmp(worker_exit)) wifi_task(NULL);
  assert(!view.connected[0] && restores == 1 && !auto_joins && !mutations);
  assert(pet_wifi_next_auto(&saved, 0) == saved.count);

  strcpy(request.station.password, "new-prior-key");
  join_succeeds = true;
  join_requested(&request, 0);
  boot();
  assert(!saved.items[0].auto_join && !strcmp(saved.items[0].station.password, request.station.password));
  link = (pet_wifi_link_status_t){0};
}

static void test_recovery(void) {
  const pet_wifi_ap_t pinned={.channel=11};
  records[0]=(wifi_ap_record_t){.ssid="shared",.authmode=PET_WIFI_OPEN,.primary=3};
  records[1]=(wifi_ap_record_t){.ssid="other",.authmode=PET_WIFI_PERSONAL,.primary=4};
  records[2]=(wifi_ap_record_t){.ssid="shared",.authmode=PET_WIFI_PERSONAL,.primary=6,.bssid={42}};
  script_reason=201;
  assert(join_with_recovery(&original,NULL,false) && scans==1 && channels[0]==0 && channels[1]==6);
  attempts=0;
  assert(join_with_recovery(&original,&pinned,true) && scans==1 && channels[0]==11 && channels[1]==0);
  attempts=0; records[2].primary=0;
  assert(!join_with_recovery(&original,NULL,true) && scans==2 && attempts==1);
  attempts=0; atomic_store(&cancelled,true);
  assert(!join_with_recovery(&original,NULL,true) && scans==2 && attempts==1);
  atomic_store(&cancelled,false); attempts=0; queued=true;
  assert(!join_with_recovery(&original,NULL,false) && scans==2 && attempts==1);
  queued=false; attempts=0; script_reason=202;
  assert(!join_with_recovery(&original,NULL,true) && scans==2 && attempts==1);
  script_reason=0; link=(pet_wifi_link_status_t){0};
}

static void test_link_events(void) {
  have_legacy = false;
  have_flash = true;
  flash = (pet_wifi_profiles_t){.count = 1, .items = {{.station = original}}};
  boot();
  associated = (wifi_ap_record_t){.ssid = "shared", .authmode = PET_WIFI_PERSONAL, .primary = 6};
  wifi_event(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
  lose_ip_during_query = true;
  assert(!wait_for_station_address(&original, true) && !view.connected[0]);
  assert(note == PET_WIFI_JOIN_ADDRESS_TIMEOUT && ap_queries == 1);
  lose_ip_during_query = false;
  wifi_event(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
  assert(wait_for_station_address(&original, true) && note == PET_WIFI_JOIN_OK);
  assert(pet_device_wifi_association() != 0);
  assert(!connected_link_lost());

  wifi_event(NULL, IP_EVENT, IP_EVENT_STA_LOST_IP, NULL);
  assert(pet_device_wifi_association() == 0);
  wifi_event(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
  assert(!connected_link_lost());
  wifi_event(NULL, IP_EVENT, IP_EVENT_STA_LOST_IP, NULL);
  assert(connected_link_lost());
  wifi_event(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
  wifi_event(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, NULL);
  assert(connected_link_lost());

  ap_queries = 0;
  for (unsigned i = 0; i < 3; ++i)
    wifi_event(NULL, ESP_HOSTED_EVENT, ESP_HOSTED_EVENT_TRANSPORT_FAILURE, NULL);
  assert(transport_failures == 3 && !ap_queries);
  assert(!connected_link_lost() && !connected_link_lost() && ap_queries == 1);
  associated.authmode = PET_WIFI_OPEN;
  wifi_event(NULL, ESP_HOSTED_EVENT, ESP_HOSTED_EVENT_TRANSPORT_FAILURE, NULL);
  assert(connected_link_lost());

  // A transport-only failure clears the live snapshot, including for a
  // manual-only profile. Only an eligible saved network may reconnect.
  for (unsigned enable = 0; enable < 2; ++enable) {
    flash.items[0].auto_join = enable;
    flash.items[0].station = (pet_wifi_station_t){"eligible-b", "", PET_WIFI_OPEN};
    boot();
    strcpy(view.connected, "eligible-b");
    connected_security = PET_WIFI_OPEN;
    started = true;
    auto_joins = 1;
    ticks = 0;
    ap_result = ESP_FAIL;
    wifi_event(NULL, ESP_HOSTED_EVENT, ESP_HOSTED_EVENT_TRANSPORT_FAILURE, NULL);
    if (!setjmp(worker_exit)) wifi_task(NULL);
    assert(auto_joins == 1 + enable && !mutations);
    assert(enable ? !strcmp(view.connected, "eligible-b") : !view.connected[0]);
  }
  auto_joins = ticks = 0;
  started = have_flash = false;
  have_legacy = true;
  event_bits = 0;
}

int main(void) {
  test_link_events();
  test_recovery();
  legacy = original;
  boot();
  assert(saved.count == 1 && saved.items[0].legacy_kept && saved.items[0].auto_join);
  request_t request = {
    .operation = PET_WIFI_CONNECT,
    .station = {"shared", "wrong-key", PET_WIFI_PERSONAL},
  };
  join_requested(&request, 0);
  assert(joins == 1 && view.status == PET_WIFI_FAILED && !have_flash && have_legacy);
  assert(!strcmp(saved.items[0].station.password, "good-key"));

  join_succeeds = true;
  atomic_store(&cancelled, true);
  join_requested(&request, 0);
  assert(!have_flash && have_legacy && view.status == PET_WIFI_IDLE);
  atomic_store(&cancelled, false);

  request.station = (pet_wifi_station_t){"shared", "", PET_WIFI_OPEN};
  fail_at = 1;
  join_requested(&request, -1);
  assert(note == PET_WIFI_JOIN_SAVE && view.saved_update_failed && saved.count == 1 && have_legacy);
  fail_at = 0;
  join_requested(&request, -1);
  assert(saved.count == 2 && view.status == PET_WIFI_CONNECTED && have_legacy && have_flash);
  assert(pet_wifi_profiles_find(&saved, "shared", PET_WIFI_OPEN) == 0);
  assert(pet_wifi_profiles_find(&saved, "shared", PET_WIFI_PERSONAL) == 1);
  boot();
  assert(pet_wifi_next_auto(&saved, 0) == 0 && pet_wifi_next_auto(&saved, 1) == 1);
  assert(update_saved_profile(0, PET_WIFI_AUTO_OFF) == ESP_OK);
  boot();
  assert(pet_wifi_next_auto(&saved, 0) == 1);
  assert(update_saved_profile(0, PET_WIFI_AUTO_ON) == ESP_OK);
  assert(update_saved_profile(1, PET_WIFI_AUTO_OFF) == ESP_OK && !have_legacy);
  boot();
  assert(pet_wifi_next_auto(&saved, 0) == 0 && pet_wifi_next_auto(&saved, 1) == saved.count);
  assert(update_saved_profile(0, PET_WIFI_FORGET) == ESP_OK);
  boot();
  assert(saved.count == 1 && pet_wifi_next_auto(&saved, 0) == saved.count);
  assert(!saved.items[1].station.ssid[0] && !saved.items[1].station.password[0]);

  test_downgrade_reconcile();
  for (unsigned cut = 1; cut <= 3; ++cut) test_legacy_revoke_power_cut(cut);
  test_legacy_password_recovery(0, 0);
  for (unsigned step = 1; step <= 2; ++step) {
    test_legacy_password_recovery(step, 0);
    test_legacy_password_recovery(0, step);
  }

  have_legacy = true;
  flash.count = PET_WIFI_SAVED_MAX + 1;
  boot();
  assert(storage_read_failed && saved.count == 0 && have_legacy);
  assert(save_joined_station(&request.station, -1) == ESP_FAIL);
  assert(flash.count == PET_WIFI_SAVED_MAX + 1 && have_legacy);
  test_restore_manual_only_live_link();
  test_worker();
  assert(!recovery_scans);
  test_first_boot_init_recovery();
}
''')
    binary = tmp_path / "wifi_profiles"
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-DPET_P4_SYSTEM_WIFI=1", "-I", str(MAIN), str(fixture), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
