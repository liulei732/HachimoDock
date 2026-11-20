#include <assert.h>
#include <stdio.h>

#include "pet_p4_system_settings.h"
#include "pet_p4_lan_context.h"
#include "pet_p4_wifi_clock.h"

static bool private_source;
void mbedtls_platform_zeroize(void *bytes, size_t size) { memset(bytes, 0, size); }
bool pet_p4_miniapp_private_feed(pet_private_feed_t *feed, pet_wifi_endpoint_t *endpoint) {
  if (!private_source) return false;
  *feed = (pet_private_feed_t){.kind = PET_PRIVATE_BEARER};
  *endpoint = (pet_wifi_endpoint_t){.host = "service.example.org", .path = "/account"};
  return true;
}
bool pet_wifi_data_authorization_needed(const pet_private_feed_t *binding) {
  return private_source && (!binding || binding->kind == PET_PRIVATE_BEARER);
}
bool pet_private_storage_ready(void) { return true; }
pet_private_result_t pet_private_local_result(void) { return PET_PRIVATE_ABSENT; }
bool pet_private_secret_schedule(const pet_private_feed_t *binding, const pet_private_secret_t *secret, bool forget) {
  (void)binding; (void)secret; (void)forget; return true;
}
static pet_wifi_snapshot_t wifi;
static pet_wifi_operation_t last_operation;
static char submitted[PET_WIFI_PASSWORD_SIZE], target_ssid[PET_WIFI_SSID_SIZE];
static unsigned requests, cancels;
static bool replace_snapshot_after_read;
static unsigned pair_forgets;
static bool lan_online, lan_paired, synchronized;
static pet_lan_volume_t lan_volume;
static int32_t timezone = PET_WIFI_TZ_AUTO;
int32_t pet_wifi_clock_timezone(void) { return timezone; }
int32_t pet_wifi_clock_auto_offset(void) { return 480; }
bool pet_wifi_clock_set_timezone(int32_t minutes) { timezone = minutes; return true; }
bool pet_wifi_clock_read(uint64_t now_ms, uint64_t *utc, int32_t *offset, uint64_t *age) {
  *utc = now_ms;
  *offset = timezone;
  *age = 0;
  return synchronized;
}

void pet_p4_lan_forget(void) { ++pair_forgets; lan_paired = false; }
void pet_p4_lan_pair_view(pet_lan_pair_view_t *view) {
  *view = (pet_lan_pair_view_t){.paired = lan_paired,
    .ready = pet_p4_lan_can_pair(lan_online, true, lan_paired, false)};
  if (lan_online) snprintf(view->ip, sizeof(view->ip), "%s", "192.0.2.11");
}

static void test_lan_volume(void) {
  const uint64_t now = 100;
  const uint8_t id[16] = {42}, other[16] = {23};
  uint8_t volume = 255;
  assert(!pet_lan_volume_submit(&lan_volume, now, false, id, 80));
  assert(!pet_lan_volume_submit(&lan_volume, now, true, id, 101));
  assert(pet_lan_volume_submit(&lan_volume, now, true, id, 80));
  assert(!pet_lan_volume_submit(&lan_volume, now, true, other, 0));
  assert(pet_lan_volume_query(&lan_volume, now, other) == PET_LAN_VOLUME_NONE);
  assert(pet_lan_volume_query(&lan_volume, now, id) == PET_LAN_VOLUME_APPROVED);
  assert(pet_lan_volume_claim(&lan_volume, now, &volume) && volume == 80);
  assert(!pet_lan_volume_claim(&lan_volume, now, &volume));
  lan_volume = (pet_lan_volume_t){0};
  assert(pet_lan_volume_submit(&lan_volume, now, true, id, 0));
  pet_lan_volume_cancel(&lan_volume, now);
  assert(pet_lan_volume_query(&lan_volume, now, id) == PET_LAN_VOLUME_CANCELLED);
  assert(!pet_lan_volume_claim(&lan_volume, now, &volume));
  assert(!pet_lan_volume_submit(&lan_volume, now, true, id, 0));
}
bool pet_p4_is_wifi6_hardware(void) { return true; }
bool pet_p4_ota_transfer_active(void) { return false; }
bool pet_p4_audio_active(void) { return false; }
void pet_device_wifi_cancel(void) { ++cancels; }
void pet_device_wifi_snapshot(pet_wifi_snapshot_t *snapshot) {
  *snapshot = wifi;
  if (replace_snapshot_after_read) {
    wifi.aps[0].ssid[0] = '\0';
    wifi.count = 0;
    replace_snapshot_after_read = false;
  }
}
bool pet_device_wifi_request(pet_wifi_operation_t operation, const pet_wifi_ap_t *ap,
                             const char *password) {
  if (wifi.request_pending) return false;
  ++requests;
  last_operation = operation;
  if (operation == PET_WIFI_CONNECT || operation == PET_WIFI_CONNECT_SAVED || operation == PET_WIFI_FORGET ||
      operation == PET_WIFI_AUTO_ON || operation == PET_WIFI_AUTO_OFF)
    snprintf(target_ssid, sizeof(target_ssid), "%s", ap->ssid);
  if (operation == PET_WIFI_CONNECT) {
    assert(strcmp(ap->ssid, "fixture-network") == 0);
    snprintf(submitted, sizeof(submitted), "%s", password);
  }
  return true;
}

static void test_lan_settings(void) {
  pet_p4_runtime_state_t state = {.screen_page = "main"};
  pet_settings_view_t view;
  wifi.count = 0;
  assert(pet_settings_open(&state, 20));
  pet_settings_input(&state, PET_SETTINGS_DOWN, 21);
  pet_settings_input(&state, PET_SETTINGS_ENTER, 22);
  pet_settings_snapshot(&view, 25);
  assert(view.screen == PET_SETTINGS_LAN && !view.lan.ready);
  // Connectivity alone makes an unconfigured board ready; there is no action.
  lan_online = true;
  pet_settings_snapshot(&view, 25);
  assert(view.lan.ready && !view.lan.paired);
  pet_settings_input(&state, PET_SETTINGS_ENTER, 23);
  pet_settings_touch(&state, 80, 350, 24);
  assert(pair_forgets == 0);
  // Leaving this screen does not close a pairing window or cancel enrollment.
  pet_settings_touch(&state, 48, 39, 25);
  pet_settings_touch(&state, 70, 220, 26);
  pet_settings_snapshot(&view, 25);
  assert(view.screen == PET_SETTINGS_LAN && view.lan.ready);
  // A saved binding is visible, and only its explicit Unpair control removes it.
  lan_paired = true;
  pet_settings_snapshot(&view, 25);
  assert(view.lan.paired && !view.lan.ready);
  pet_settings_touch(&state, 80, 350, 27);
  assert(pair_forgets == 0 && lan_paired);
  pet_settings_touch(&state, 80, 420, 28);
  pet_settings_snapshot(&view, 25);
  assert(pair_forgets == 1 && !view.lan.paired && view.lan.ready);
  pet_settings_maintain(&state, true);
  assert(!strcmp(state.screen_page, "main") && pair_forgets == 1);
  pet_p4_lan_pair_view(&view.lan);
  assert(view.lan.ready);
}

static void test_timezone_settings(void) {
  pet_p4_runtime_state_t state = {.screen_page = "main"};
  pet_settings_view_t view;
  assert(pet_settings_open(&state, 1));
  pet_settings_touch(&state, 80, 310, 2);
  pet_settings_snapshot(&view, 25);
  assert(view.screen == PET_SETTINGS_TIMEZONE && view.timezone_draft == PET_WIFI_TZ_AUTO);
  assert(view.timezone_effective == 480);
  pet_settings_touch(&state, 530, 194, 3);
  pet_settings_snapshot(&view, 25);
  assert(view.timezone_draft == 495 && timezone == PET_WIFI_TZ_AUTO);
  pet_settings_input(&state, PET_SETTINGS_BACK, 4);
  pet_settings_input(&state, PET_SETTINGS_ENTER, 5);
  pet_settings_input(&state, PET_SETTINGS_LEFT, 6);
  pet_settings_touch(&state, 300, 420, 7);
  assert(timezone == 465);
  pet_settings_input(&state, PET_SETTINGS_ENTER, 8);
  pet_settings_touch(&state, 300, 284, 9);
  pet_settings_snapshot(&view, 25);
  assert(view.timezone_draft == PET_WIFI_TZ_AUTO && view.timezone_effective == 480);
  pet_settings_input(&state, PET_SETTINGS_ENTER, 10);
  assert(timezone == PET_WIFI_TZ_AUTO);
}

static void test_touch_keeps_the_network_from_the_hit_test(void) {
  pet_p4_runtime_state_t state = {.screen_page = "components"};
  pet_settings_view_t view;
  wifi = (pet_wifi_snapshot_t){.count = 1};
  wifi.aps[0] = (pet_wifi_ap_t){.ssid = "fixture-network", .security = PET_WIFI_OPEN};
  assert(pet_settings_open(&state, 20));
  pet_settings_input(&state, PET_SETTINGS_ENTER, 21);
  const unsigned before = requests;
  replace_snapshot_after_read = true;
  pet_settings_touch(&state, 36, 155, 22);
  assert(requests == before + 1);
  assert(last_operation == PET_WIFI_CONNECT && submitted[0] == '\0');
  pet_settings_snapshot(&view, 25);
  assert(view.wifi.count == 0);
  pet_settings_maintain(&state, true);
}

static void test_connected_details_and_confirmed_forget(void) {
  pet_p4_runtime_state_t state = {.screen_page = "main"};
  pet_settings_view_t view;
  wifi = (pet_wifi_snapshot_t){
    .status = PET_WIFI_CONNECTED, .saved_count = 2, .connected = "fixture-network",
    .saved = {{.ssid = "fixture-network", .security = PET_WIFI_WPA3, .current = true, .auto_join = true},
              {.ssid = "other-network", .security = PET_WIFI_PERSONAL, .auto_join = true}},
    .connection = {.ip = "192.0.2.11", .gateway = "192.0.2.1", .rssi = -56,
                   .channel = 36, .security = PET_WIFI_WPA3},
  };
  const unsigned before = requests;
  assert(pet_settings_open(&state, 120));
  pet_settings_input(&state, PET_SETTINGS_ENTER, 121);
  pet_settings_snapshot(&view, 122);
  assert(view.selected == pet_settings_wifi_details_index(&wifi) && requests == before);
  pet_settings_input(&state, PET_SETTINGS_ENTER, 123);
  assert(last_operation == PET_WIFI_DETAILS && requests == before + 1);
  synchronized = true;
  pet_settings_snapshot(&view, 124);
  assert(view.screen == PET_SETTINGS_WIFI_DETAILS && view.clock_synchronized);
  assert(!strcmp(view.wifi.connected, "fixture-network"));
  assert(!strcmp(view.wifi.connection.ip, "192.0.2.11"));
  assert(view.wifi.connection.channel == 36 && view.wifi.connection.rssi == -56);
  pet_settings_touch(&state, 250, 423, 125);
  assert(last_operation == PET_WIFI_DETAILS && requests == before + 2);
  wifi.connected[0] = 0;
  wifi.saved[0].current = false;
  pet_settings_snapshot(&view, 126);
  assert(!view.wifi.connected[0] && !view.clock_synchronized);
  pet_settings_input(&state, PET_SETTINGS_ENTER, 127);
  assert(requests == before + 2);
  pet_settings_input(&state, PET_SETTINGS_BACK, 128);
  pet_settings_touch(&state, 311, 420, 129);
  pet_settings_snapshot(&view, 130);
  assert(view.screen == PET_SETTINGS_WIFI_SAVED && view.wifi.saved_count == 2 && requests == before + 2);
  pet_settings_touch(&state, 80, 156, 131); // Select the second profile without connecting.
  assert(requests == before + 2);
  pet_settings_touch(&state, 390, 420, 132); // Password change starts from a blank entry.
  pet_settings_snapshot(&view, 133);
  assert(view.screen == PET_SETTINGS_PASSWORD && view.password_length == 0);
  assert(!strcmp(view.target.ssid, "other-network") && !view.password_visible);
  pet_settings_input(&state, PET_SETTINGS_BACK, 134);
  pet_settings_touch(&state, 520, 156, 135);
  pet_settings_snapshot(&view, 136);
  assert(view.screen == PET_SETTINGS_WIFI_FORGET && !view.confirm_forget && requests == before + 2);
  pet_settings_input(&state, PET_SETTINGS_ENTER, 137);
  assert(requests == before + 2);
  pet_settings_touch(&state, 520, 156, 138);
  pet_settings_input(&state, PET_SETTINGS_RIGHT, 139);
  wifi.request_pending = true;
  pet_settings_input(&state, PET_SETTINGS_ENTER, 140);
  pet_settings_snapshot(&view, 140);
  assert(view.screen == PET_SETTINGS_WIFI_FORGET && view.wifi.request_pending && requests == before + 2);
  wifi.request_pending = false;
  pet_settings_snapshot(&view, 140);
  assert(!view.wifi.request_pending);
  pet_settings_input(&state, PET_SETTINGS_ENTER, 140);
  assert(last_operation == PET_WIFI_FORGET && requests == before + 3 && !strcmp(target_ssid, "other-network"));
  wifi.saved_count = 1;
  pet_settings_snapshot(&view, 141);
  assert(view.screen == PET_SETTINGS_WIFI_SAVED && view.selected == 0);
  pet_settings_touch(&state, 118, 420, 142);
  assert(last_operation == PET_WIFI_CONNECT_SAVED && requests == before + 4 && !strcmp(target_ssid, "fixture-network"));
  wifi.count = 1;
  wifi.aps[0] = (pet_wifi_ap_t){.ssid = "fixture-network", .security = PET_WIFI_WPA3};
  pet_settings_touch(&state, 36, 155, 143);
  assert(last_operation == PET_WIFI_CONNECT_SAVED && requests == before + 5);
  pet_settings_maintain(&state, true);
}

static void test_each_saved_row_has_a_touch_and_joystick_auto_join_control(void) {
  pet_p4_runtime_state_t state = {.screen_page = "main"};
  wifi = (pet_wifi_snapshot_t){.saved_count = 2,
    .saved = {{.ssid = "fixture-network", .security = PET_WIFI_PERSONAL, .auto_join = true},
              {.ssid = "other-network", .security = PET_WIFI_PERSONAL, .auto_join = true}}};
  assert(pet_settings_open(&state, 200));
  pet_settings_input(&state, PET_SETTINGS_ENTER, 201);
  pet_settings_touch(&state, 310, 420, 202);
  unsigned before = requests;
  pet_settings_touch(&state, 400, 156, 203);
  assert(requests == before + 1 && last_operation == PET_WIFI_AUTO_OFF);
  assert(!strcmp(target_ssid, "other-network"));
  wifi.saved[1].auto_join = false;
  pet_settings_input(&state, PET_SETTINGS_ENTER, 204);
  assert(requests == before + 2 && last_operation == PET_WIFI_AUTO_ON);
  assert(!strcmp(target_ssid, "other-network"));
  pet_settings_input(&state, PET_SETTINGS_UP, 205);
  pet_settings_input(&state, PET_SETTINGS_ENTER, 206);
  assert(requests == before + 3 && last_operation == PET_WIFI_AUTO_OFF);
  assert(!strcmp(target_ssid, "fixture-network"));
  pet_settings_maintain(&state, true);
}

static void test_long_password_preview_and_exit(void) {
  pet_p4_runtime_state_t state = {.screen_page = "main"};
  pet_settings_view_t view;
  wifi = (pet_wifi_snapshot_t){.count = 1};
  wifi.aps[0] = (pet_wifi_ap_t){.ssid = "fixture-network", .security = PET_WIFI_PERSONAL};
  assert(pet_settings_open(&state, 200));
  pet_settings_input(&state, PET_SETTINGS_ENTER, 201);
  pet_settings_touch(&state, 36, 155, 202);
  for (unsigned i = 0; i < 63; ++i) pet_settings_touch(&state, 38, 246, 203+i);
  pet_settings_snapshot(&view, 268);
  assert(view.password_length == 63 && !view.password_visible && !view.password_preview[0][0]);
  pet_settings_touch(&state, 460, 429, 269);
  pet_settings_snapshot(&view, 270);
  assert(view.password_visible && strlen(view.password_preview[0]) == 32);
  assert(strlen(view.password_preview[1]) == 31 && view.password_preview[1][30] == 'q');
  pet_settings_clear_preview(&view);
  assert(!view.password_visible && !view.password_preview[0][0] && !view.password_preview[1][0]);
  pet_settings_touch(&state, 460, 429, 271);
  pet_settings_snapshot(&view, 272);
  assert(!view.password_visible && !view.password_preview[0][0]);
  pet_settings_touch(&state, 460, 429, 273);
  pet_settings_snapshot(&view, 10273);
  assert(!view.password_visible && !view.password_preview[0][0] && view.password_length == 63);
  pet_settings_touch(&state, 460, 429, 10274);
  pet_settings_input(&state, PET_SETTINGS_BACK, 10275);
  pet_settings_snapshot(&view, 10276);
  assert(view.screen == PET_SETTINGS_WIFI && !view.password_length && !view.password_visible && !view.password_preview[0][0]);
  pet_settings_maintain(&state, true);
}

static void test_keyboards_cover_printable_password_characters(void) {
  bool available[127] = {[' '] = true}; // The separate space key inserts a space.
  for (unsigned keyboard = 0; keyboard < 3; ++keyboard) {
    for (unsigned row = 0; row < 4; ++row) {
      for (unsigned col = 0; col < pet_settings_key_columns(row); ++col) {
        char ascii[2];
        unsigned char key = pet_settings_key_label(keyboard, row, col, ascii)[0];
        assert(key > ' ' && key < sizeof(available));
        available[key] = true;
      }
    }
  }
  for (unsigned key = ' '; key < sizeof(available); ++key) assert(available[key]);
}

int main(void) {
  for (unsigned bits = 0; bits < 16; ++bits) {
    bool online = bits & 1, known = bits & 2, paired = bits & 4, forgetting = bits & 8;
    assert(pet_p4_lan_can_pair(online, known, paired, forgetting) == (bits == 3));
  }

  const char query[] = "HACHI-DISCOVER-1 0123456789abcdef0123456789abcdef";
  assert(strlen(query) == 49);
  assert(!strcmp(pet_p4_lan_discovery_nonce(query, strlen(query)), "0123456789abcdef0123456789abcdef"));
  assert(!pet_p4_lan_discovery_nonce(query, 48));
  assert(!pet_p4_lan_discovery_nonce(query, 50));
  assert(!pet_p4_lan_discovery_nonce("HACHI-DISCOVER-1 z123456789abcdef0123456789abcdef", 49));
  char context[64];
  assert(pet_p4_lan_identity_password(context, sizeof(context), PET_P4_LAN_AUTO_CONTEXT) == strlen("Hachimodock:Hachimodock-auto-v1"));
  assert(strcmp(context, "Hachimodock:Hachimodock-auto-v1") == 0);
  assert(pet_p4_lan_identity_password(context, sizeof(context), "0123") == strlen("Hachimodock:0123"));
  assert(!pet_p4_lan_identity_password(context, 5, PET_P4_LAN_AUTO_CONTEXT) && !context[0]);
  pet_p4_runtime_state_t state = {.screen_page = "components"};
  pet_settings_view_t view;
  bool ascii[127] = {0};
  ascii[' '] = true;
  for (unsigned layer = 0; layer < 3; ++layer) {
    for (unsigned row = 0; row < 4; ++row) {
      for (unsigned col = 0; col < pet_settings_key_columns(row); ++col) {
        char key[2];
        pet_settings_key_label(layer, row, col, key);
        assert((unsigned char)key[0] >= 32 && (unsigned char)key[0] < 127 && !key[1]);
        ascii[(unsigned char)key[0]] = true;
      }
    }
  }
  for (unsigned i = 32; i < 127; ++i) assert(ascii[i]);

  assert(pet_settings_open(&state, 1));
  pet_settings_snapshot(&view, 25);
  assert(view.screen == PET_SETTINGS_HOME);
  pet_settings_input(&state, PET_SETTINGS_ENTER, 2);
  assert(last_operation == PET_WIFI_SCAN && requests == 1);
  wifi.count = 1;
  wifi.aps[0] = (pet_wifi_ap_t){.ssid = "fixture-network", .security = PET_WIFI_PERSONAL};
  pet_settings_touch(&state, 36, 155, 3);
  pet_settings_snapshot(&view, 25);
  assert(view.screen == PET_SETTINGS_PASSWORD);
  pet_settings_touch(&state, 560, 429, 4);
  pet_settings_snapshot(&view, 25);
  assert(view.short_password && requests == 1);

  pet_settings_touch(&state, 153, 427, 5); // Symbol layer.
  pet_settings_touch(&state, 38, 185, 6); // '!'
  pet_settings_touch(&state, 153, 427, 7); // Back to letters.
  for (unsigned i = 0; i < 7; ++i) pet_settings_touch(&state, 38, 246, 8 + i);
  pet_settings_snapshot(&view, 25);
  assert(view.password_length == 8);
  pet_settings_touch(&state, 560, 429, 15);
  assert(last_operation == PET_WIFI_CONNECT && requests == 2);
  assert(strcmp(submitted, "!qqqqqqq") == 0);
  pet_settings_snapshot(&view, 25);
  assert(view.screen == PET_SETTINGS_WIFI && view.password_length == 0);

  // Swipe changes pages without selecting a network or sending a request.
  wifi.count = 9;
  pet_settings_wifi_swipe(&state, 150, 200, true, 16);
  pet_settings_snapshot(&view, 25);
  assert(view.list_start == 4 && view.selected == 4 && requests == 2);
  pet_settings_input(&state, PET_SETTINGS_RIGHT, 16);
  pet_settings_wifi_swipe(&state, 150, 200, true, 16);
  pet_settings_snapshot(&view, 25);
  assert(view.list_start == 8 && view.selected == 8 && requests == 2);
  pet_settings_touch(&state, 520, 420, 16);
  pet_settings_wifi_swipe(&state, 150, 200, false, 16);
  pet_settings_snapshot(&view, 25);
  assert(view.list_start == 0 && view.selected == 0 && requests == 2);
  for (unsigned i = 0; i < 4; ++i) pet_settings_input(&state, PET_SETTINGS_DOWN, 16);
  pet_settings_snapshot(&view, 25);
  assert(view.list_start == 4 && view.selected == 4);
  pet_settings_wifi_swipe(&state, 150, 200, false, 16);
  pet_settings_snapshot(&view, 25);
  assert(view.list_start == 0 && view.selected == 0);

  pet_settings_input(&state, PET_SETTINGS_BACK, 16);
  assert(cancels == 1);
  pet_settings_input(&state, PET_SETTINGS_BACK, 17);
  pet_settings_maintain(&state, false);
  assert(strcmp(state.screen_page, "components") == 0);
  strcpy(state.screen_page, "app");
  pet_settings_maintain(&state, false);
  assert(!pet_settings_active(&state));
  private_source = true;
  pet_settings_maintain(&state, false);
  pet_settings_snapshot(&view, 0);
  assert(pet_settings_active(&state) && view.screen == PET_SETTINGS_FEED);
  pet_settings_input(&state, PET_SETTINGS_BACK, 18);
  pet_settings_maintain(&state, false);
  assert(!strcmp(state.screen_page, "app"));
  private_source = false;
  test_lan_settings();
  test_lan_volume();
  test_timezone_settings();
  test_touch_keeps_the_network_from_the_hit_test();
  test_connected_details_and_confirmed_forget();
  test_each_saved_row_has_a_touch_and_joystick_auto_join_control();
  test_long_password_preview_and_exit();
  test_keyboards_cover_printable_password_characters();
  return 0;
}
