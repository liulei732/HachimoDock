#include <assert.h>
#include <stdio.h>

#include "pet_p4_system_settings.h"
#include "pet_p4_lan_context.h"
#include "pet_p4_wifi_clock.h"

static uint32_t av_ticket;
bool pet_settings_av_queue(uint32_t ticket) { av_ticket=ticket; return true; }
static bool ble_enabled, ble_pairing, ble_paired;
bool pet_ble_keyboard_connected(void) {return ble_enabled && ble_paired;}
bool pet_ble_keyboard_enabled(void) {return ble_enabled;}
bool pet_ble_keyboard_set_mode(bool on) {ble_enabled=on; ble_pairing=on && !ble_paired; return true;}
void pet_ble_shortcut_open_pair(void) {ble_pairing=true;}
void pet_ble_shortcut_close_pair(void) {ble_pairing=false;}
bool pet_ble_shortcut_pairing(void) {return ble_pairing;}
int pet_ble_shortcut_available(void) {return 1;}
int pet_ble_shortcut_paired(void) {return ble_paired;}
void pet_ble_shortcut_forget(void) {ble_paired=false;}
unsigned pet_ble_shortcut_code(void) {return 0;}

static void test_av_ble(void) {
  pet_p4_runtime_state_t state={.screen_page="app"};
  pet_settings_view_t v;
  int volume, brightness;
  assert(pet_settings_open(&state,1));
  pet_settings_touch(&state,500,420,2);
  uint32_t stale=av_ticket;
  assert(pet_settings_av_claim(stale,&volume,&brightness) && volume==-1 && brightness==-1);
  pet_settings_input(&state,PET_SETTINGS_BACK,3);
  assert(!pet_settings_av_claim(stale,&volume,&brightness));
  pet_settings_input(&state,PET_SETTINGS_ENTER,4);
  pet_settings_av_finish(stale,5,5,true,false);
  pet_settings_snapshot(&v);
  assert(v.av_pending && v.volume==-1 && v.brightness==-1);
  pet_settings_av_finish(av_ticket,95,10,true,false);
  pet_settings_av_disk(50,95,false); pet_settings_snapshot(&v); assert(v.volume_storage==PET_AV_SAVING);
  pet_settings_av_disk(-1,95,true); pet_settings_snapshot(&v); assert(v.volume_storage==PET_AV_SAVE_FAILED);
  pet_settings_av_disk(95,-1,false); pet_settings_snapshot(&v); assert(v.volume_storage==PET_AV_SAVED);
  pet_settings_touch(&state,530,153,5);
  pet_settings_touch(&state,100,230,6);
  pet_settings_touch(&state,100,420,7);
  assert(pet_settings_av_claim(av_ticket,&volume,&brightness) && volume==100 && brightness==-1);
  pet_settings_av_finish(av_ticket,95,-1,false,false);
  pet_settings_snapshot(&v);
  assert(v.av_failed && v.volume==95 && v.brightness==-1);
  pet_settings_touch(&state,530,230,8);
  pet_settings_touch(&state,530,420,8);
  assert(pet_settings_av_claim(av_ticket,&volume,&brightness) && volume==-1 && brightness==-1);
  pet_settings_av_finish(av_ticket,95,-1,true,false);
  pet_settings_input(&state,PET_SETTINGS_BACK,9);
  pet_settings_touch(&state,80,360,10);
  pet_settings_touch(&state,80,420,11);
  assert(ble_enabled && ble_pairing);
  pet_settings_touch(&state,500,420,12);
  assert(ble_enabled && !ble_pairing);
  pet_settings_touch(&state,500,420,13);
  assert(ble_pairing);
  ble_pairing=false; ble_paired=true;
  pet_settings_touch(&state,500,420,14);
  pet_settings_input(&state,PET_SETTINGS_ENTER,15);
  assert(ble_paired);
  pet_settings_touch(&state,500,420,16);
  pet_settings_touch(&state,500,420,17);
  assert(!ble_paired);
  pet_settings_touch(&state,80,420,18);
  assert(!ble_enabled);
  pet_settings_maintain(&state,true);
  assert(!strcmp(state.screen_page,"app"));
}
static unsigned community_browses, community_installs, community_removes;
static pet_community_view_t community;
void pet_community_view(pet_community_view_t *out) { *out=community; }
void pet_community_browse(bool next) { (void)next; community_browses++; }
void pet_community_local(void) {}
void pet_community_close(void) {}
void pet_community_move(int delta) { (void)delta; }
void pet_community_action(bool remove) { if (remove) community_removes++; else community_installs++; }
static void test_community_confirmation(void) {
  pet_p4_runtime_state_t state={.screen_page="main"};
  pet_settings_view_t view;
  community=(pet_community_view_t){.install=true,.remove=true,.privileges=1,.count=1};
  assert(pet_settings_open(&state,1));
  pet_settings_touch(&state,80,420,2);
  pet_settings_snapshot(&view);
  assert(view.screen==PET_SETTINGS_COMMUNITY && community_browses==1);
  pet_settings_touch(&state,80,330,3);
  pet_settings_snapshot(&view);
  assert(view.community_confirm && !view.selected && !community_installs);
  pet_settings_input(&state,PET_SETTINGS_ENTER,4);
  assert(!community_installs);
  pet_settings_touch(&state,80,330,5);
  pet_settings_touch(&state,390,260,6);
  assert(community_installs==1);
  pet_settings_touch(&state,390,330,7);
  pet_settings_input(&state,PET_SETTINGS_BACK,8);
  assert(!community_removes);
  pet_settings_touch(&state,390,330,9);
  pet_settings_touch(&state,390,260,10);
  assert(community_removes==1);
  pet_settings_maintain(&state,true);
}

static bool private_source;
static pet_private_kind_t selected_kind = PET_PRIVATE_BEARER;
static unsigned sends, credential_saves;
static size_t saved_user_length, saved_token_length;
bool pet_device_write_start(const pet_private_feed_t *selected) {
  assert(selected->kind == selected_kind); ++sends; return true;
}
pet_private_result_t pet_device_write_result(void) { return PET_WRITE_UNKNOWN; }
void pet_device_write_cancel(void) {}
void mbedtls_platform_zeroize(void *bytes, size_t size) { memset(bytes, 0, size); }
bool pet_p4_miniapp_private_feed(pet_private_feed_t *feed, pet_wifi_endpoint_t *endpoint) {
  if (!private_source) return false;
  *feed = (pet_private_feed_t){.kind = selected_kind, .value = "arrived", .port = 443};
  if (endpoint) *endpoint = (pet_wifi_endpoint_t){.host = "service.example.org", .path = "/account"};
  return true;
}
bool pet_wifi_data_authorization_needed(const pet_private_feed_t *binding) {
  return private_source && (!binding || binding->kind == PET_PRIVATE_BEARER);
}
bool pet_private_storage_ready(void) { return true; }
pet_private_result_t pet_private_local_result(void) { return PET_PRIVATE_ABSENT; }
uint32_t pet_private_storage_epoch(void) { return 1; }
bool pet_private_secret_schedule_at(const pet_private_feed_t *binding, const pet_private_secret_t *secret, bool forget, uint32_t epoch) {
  assert(epoch == 1);
  (void)binding;
  if (!forget) {
    credential_saves++;
    saved_user_length = strlen(secret->user);
    saved_token_length = strlen(secret->token);
  }
  return true;
}
static pet_wifi_snapshot_t wifi;
static pet_wifi_operation_t last_operation;
static char submitted[PET_WIFI_PASSWORD_SIZE];
static pet_wifi_ap_t requested_target;
static unsigned requests, cancels;
static bool replace_snapshot_after_read;
static unsigned pair_forgets;
static bool lan_online, lan_paired;
static pet_lan_volume_t lan_volume;
static int32_t timezone = PET_WIFI_TZ_AUTO;
int32_t pet_wifi_clock_timezone(void) { return timezone; }
int32_t pet_wifi_clock_auto_offset(void) { return 480; }
bool pet_wifi_clock_set_timezone(int32_t minutes) { timezone = minutes; return true; }

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
  ++requests;
  last_operation = operation;
  requested_target = *ap;
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
  pet_settings_snapshot(&view);
  assert(view.screen == PET_SETTINGS_LAN && !view.lan.ready);
  // Connectivity alone makes an unconfigured board ready; there is no action.
  lan_online = true;
  pet_settings_snapshot(&view);
  assert(view.lan.ready && !view.lan.paired);
  pet_settings_input(&state, PET_SETTINGS_ENTER, 23);
  pet_settings_touch(&state, 80, 350, 24);
  assert(pair_forgets == 0);
  // Leaving this screen does not close a pairing window or cancel enrollment.
  pet_settings_touch(&state, 48, 39, 25);
  pet_settings_touch(&state, 70, 220, 26);
  pet_settings_snapshot(&view);
  assert(view.screen == PET_SETTINGS_LAN && view.lan.ready);
  // A saved binding is visible, and only its explicit Unpair control removes it.
  lan_paired = true;
  pet_settings_snapshot(&view);
  assert(view.lan.paired && !view.lan.ready);
  pet_settings_touch(&state, 80, 350, 27);
  assert(pair_forgets == 0 && lan_paired);
  pet_settings_touch(&state, 80, 420, 28);
  pet_settings_snapshot(&view);
  assert(view.confirm_remove && lan_paired);
  pet_settings_touch(&state,500,420,29);
  pet_settings_snapshot(&view);
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
  pet_settings_snapshot(&view);
  assert(view.screen == PET_SETTINGS_TIMEZONE && view.timezone_draft == PET_WIFI_TZ_AUTO);
  assert(view.timezone_effective == 480);
  pet_settings_touch(&state, 530, 194, 3);
  pet_settings_snapshot(&view);
  assert(view.timezone_draft == 495 && timezone == PET_WIFI_TZ_AUTO);
  pet_settings_input(&state, PET_SETTINGS_BACK, 4);
  pet_settings_input(&state, PET_SETTINGS_ENTER, 5);
  pet_settings_input(&state, PET_SETTINGS_LEFT, 6);
  pet_settings_touch(&state, 300, 420, 7);
  assert(timezone == 465);
  pet_settings_input(&state, PET_SETTINGS_ENTER, 8);
  pet_settings_touch(&state, 300, 284, 9);
  pet_settings_snapshot(&view);
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
  pet_settings_snapshot(&view);
  assert(view.wifi.count == 0);
  pet_settings_maintain(&state, true);
}

static void test_saved_network_actions(void) {
  pet_p4_runtime_state_t state = {.screen_page = "components"};
  pet_settings_view_t view;
  wifi = (pet_wifi_snapshot_t){.saved_count = 6};
  wifi.saved[0] = (pet_wifi_saved_network_t){.ssid = "shared", .security = PET_WIFI_OPEN};
  for (unsigned i = 1; i < 5; ++i) snprintf(wifi.saved[i].ssid, sizeof(wifi.saved[i].ssid), "fixture-%u", i);
  wifi.saved[5] = (pet_wifi_saved_network_t){.ssid = "shared", .security = PET_WIFI_PERSONAL};
  assert(pet_settings_open(&state, 1));
  pet_settings_input(&state, PET_SETTINGS_ENTER, 2);
  pet_settings_touch(&state, 322, 416, 3);
  pet_settings_wifi_swipe(&state, 150, 200, true, 4);
  pet_settings_snapshot(&view);
  assert(view.screen == PET_SETTINGS_WIFI_SAVED && view.list_start == 4);
  pet_settings_touch(&state, 100, 210, 5);
  pet_settings_snapshot(&view);
  assert(view.screen == PET_SETTINGS_WIFI_NETWORK && view.target.security == PET_WIFI_PERSONAL);
  pet_settings_input(&state, PET_SETTINGS_ENTER, 6);
  assert(last_operation == PET_WIFI_CONNECT_SAVED && requested_target.security == PET_WIFI_PERSONAL);

  // An update can reorder the list while the network's action screen is open.
  pet_wifi_saved_network_t swap = wifi.saved[0];
  wifi.saved[0] = wifi.saved[5];
  wifi.saved[5] = swap;
  pet_settings_touch(&state, 120, 245, 7);
  assert(last_operation == PET_WIFI_AUTO_ON && requested_target.security == PET_WIFI_PERSONAL);
  wifi.saved[0].auto_join = true;
  pet_settings_touch(&state, 120, 245, 8);
  assert(last_operation == PET_WIFI_AUTO_OFF && requested_target.security == PET_WIFI_PERSONAL);

  unsigned before = requests;
  wifi.request_pending = true;
  pet_settings_touch(&state, 120, 245, 9);
  assert(requests == before);
  wifi.request_pending = false;
  pet_settings_touch(&state, 120, 309, 10);
  pet_settings_snapshot(&view);
  assert(view.screen == PET_SETTINGS_WIFI_FORGET && view.selected == 0 && requests == before);
  pet_settings_input(&state, PET_SETTINGS_ENTER, 11); // Default is Cancel.
  pet_settings_snapshot(&view);
  assert(view.screen == PET_SETTINGS_WIFI_NETWORK && requests == before);
  pet_settings_input(&state, PET_SETTINGS_ENTER, 12);
  pet_settings_input(&state, PET_SETTINGS_DOWN, 13);
  pet_settings_input(&state, PET_SETTINGS_ENTER, 14);
  assert(requests == before + 1 && last_operation == PET_WIFI_FORGET &&
         !strcmp(requested_target.ssid, "shared") && requested_target.security == PET_WIFI_PERSONAL);
  pet_settings_snapshot(&view);
  assert(view.screen == PET_SETTINGS_WIFI_SAVED);
  pet_settings_maintain(&state, true);
}

static void test_wifi_edit_and_limits(void) {
  pet_p4_runtime_state_t state = {.screen_page="main"};
  pet_settings_view_t v;
  wifi=(pet_wifi_snapshot_t){.count=1,.saved_count=1,.status=PET_WIFI_FAILED,.join_reason=201};
  wifi.aps[0]=(pet_wifi_ap_t){.ssid="fixture-network",.security=PET_WIFI_PERSONAL};
  wifi.saved[0]=(pet_wifi_saved_network_t){.ssid="fixture-network",.security=PET_WIFI_PERSONAL};
  assert(pet_settings_open(&state,1));
  pet_settings_input(&state,PET_SETTINGS_ENTER,2);
  pet_settings_snapshot(&v);
  assert(strstr(pet_settings_wifi_status(&v),"未找到热点"));
  v.wifi.status=PET_WIFI_CONNECTING; v.wifi.request_pending=true;
  assert(!strcmp(pet_settings_wifi_status(&v),"正在连接"));
  v.wifi.request_pending=false; v.wifi.status=PET_WIFI_FAILED;
  v.wifi.join_stage=PET_WIFI_JOIN_ADDRESS_TIMEOUT;
  assert(strstr(pet_settings_wifi_status(&v),"获取地址"));
  v.wifi.status=PET_WIFI_INIT_RETRY;
  assert(strstr(pet_settings_wifi_status(&v),"自动重试"));
  v.wifi.status=PET_WIFI_UNAVAILABLE;
  assert(strstr(pet_settings_wifi_status(&v),"重新扫描"));
  v.wifi.saved_update_failed=true; v.wifi.status=PET_WIFI_REBOOT_REQUIRED;
  assert(strstr(pet_settings_wifi_status(&v),"重启设备"));
  const unsigned before=requests;
  pet_settings_touch(&state,120,152,3);
  pet_settings_snapshot(&v);
  assert(v.screen==PET_SETTINGS_WIFI_NETWORK && requests==before);
  pet_settings_touch(&state,120,395,4);
  pet_settings_snapshot(&v);
  assert(v.screen==PET_SETTINGS_PASSWORD && v.password_edit);
  pet_settings_touch(&state,10,185,5); /* outer gutter */
  pet_settings_touch(&state,38,211,6); /* row gutter */
  pet_settings_snapshot(&v);
  assert(!v.password_length);
  for (unsigned i=0;i<64;++i) pet_settings_touch(&state,38,185,7+i);
  pet_settings_snapshot(&v);
  assert(v.entry_full && v.password_length==63 && requests==before);
  pet_settings_input(&state,PET_SETTINGS_BACK,80);
  pet_settings_snapshot(&v);
  assert(v.screen==PET_SETTINGS_WIFI_NETWORK && !v.password_length && requests==before);
  pet_settings_input(&state,PET_SETTINGS_BACK,81);
  pet_settings_snapshot(&v);
  assert(v.screen==PET_SETTINGS_WIFI);
  pet_settings_maintain(&state,true);
}

static void test_masked_credentials_limits(void) {
  pet_p4_runtime_state_t state={.screen_page="app"};
  pet_settings_view_t v;
  selected_kind=PET_MQTT_PUBLISH;
  private_source=true;
  assert(pet_settings_open(&state,1));
  pet_settings_touch(&state,100,295,2);
  pet_settings_touch(&state,560,429,3);
  pet_settings_snapshot(&v);
  assert(v.feed_username && v.short_password && !credential_saves);
  for(unsigned i=0;i<PET_PRIVATE_USER_MAX+1;i++) pet_settings_touch(&state,38,246,4+i);
  pet_settings_snapshot(&v);
  assert(v.password_length==PET_PRIVATE_USER_MAX && v.entry_full && !credential_saves);
  pet_settings_touch(&state,560,429,100);
  pet_settings_snapshot(&v);
  assert(!v.feed_username && !v.password_length && !v.entry_full);
  for(unsigned i=0;i<PET_PRIVATE_TOKEN_MAX+1;i++) pet_settings_touch(&state,38,246,101+i);
  pet_settings_snapshot(&v);
  assert(v.password_length==PET_PRIVATE_TOKEN_MAX && v.entry_full && !credential_saves);
  /* The form snapshot contains length and focus, never the entered credential. */
  for(size_t offset=0;offset+12<=sizeof(v);offset++)
    assert(memcmp((char *)&v+offset,"qqqqqqqqqqqq",12));
  pet_settings_touch(&state,560,429,500);
  pet_settings_snapshot(&v);
  assert(v.screen==PET_SETTINGS_FEED && !v.password_length && credential_saves==1);
  assert(saved_user_length==PET_PRIVATE_USER_MAX && saved_token_length==PET_PRIVATE_TOKEN_MAX);
  pet_settings_maintain(&state,true);
  private_source=false;
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
  pet_settings_snapshot(&view);
  assert(view.screen == PET_SETTINGS_HOME);
  pet_settings_input(&state, PET_SETTINGS_ENTER, 2);
  assert(last_operation == PET_WIFI_SCAN && requests == 1);
  wifi.count = 1;
  wifi.aps[0] = (pet_wifi_ap_t){.ssid = "fixture-network", .security = PET_WIFI_PERSONAL};
  pet_settings_touch(&state, 36, 155, 3);
  pet_settings_snapshot(&view);
  assert(view.screen == PET_SETTINGS_PASSWORD);
  pet_settings_touch(&state, 560, 429, 4);
  pet_settings_snapshot(&view);
  assert(view.short_password && requests == 1);

  pet_settings_touch(&state, 153, 427, 5); // Symbol layer.
  pet_settings_touch(&state, 38, 185, 6); // '!'
  pet_settings_touch(&state, 153, 427, 7); // Back to letters.
  for (unsigned i = 0; i < 7; ++i) pet_settings_touch(&state, 38, 246, 8 + i);
  pet_settings_snapshot(&view);
  assert(view.password_length == 8);
  pet_settings_touch(&state, 560, 429, 15);
  assert(last_operation == PET_WIFI_CONNECT && requests == 2);
  assert(strcmp(submitted, "!qqqqqqq") == 0);
  pet_settings_snapshot(&view);
  assert(view.screen == PET_SETTINGS_WIFI && view.password_length == 0);

  // Swipe changes pages without selecting a network or sending a request.
  wifi.count = 9;
  pet_settings_wifi_swipe(&state, 150, 200, true, 16);
  pet_settings_snapshot(&view);
  assert(view.list_start == 4 && view.selected == 4 && requests == 2);
  pet_settings_input(&state, PET_SETTINGS_RIGHT, 16);
  pet_settings_wifi_swipe(&state, 150, 200, true, 16);
  pet_settings_snapshot(&view);
  assert(view.list_start == 8 && view.selected == 8 && requests == 2);
  pet_settings_touch(&state, 520, 420, 16);
  pet_settings_wifi_swipe(&state, 150, 200, false, 16);
  pet_settings_snapshot(&view);
  assert(view.list_start == 0 && view.selected == 0 && requests == 2);
  for (unsigned i = 0; i < 4; ++i) pet_settings_input(&state, PET_SETTINGS_DOWN, 16);
  pet_settings_snapshot(&view);
  assert(view.list_start == 4 && view.selected == 4);
  pet_settings_wifi_swipe(&state, 150, 200, false, 16);
  pet_settings_snapshot(&view);
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
  pet_settings_snapshot(&view);
  assert(pet_settings_active(&state) && view.screen == PET_SETTINGS_FEED);
  pet_settings_input(&state, PET_SETTINGS_BACK, 18);
  pet_settings_maintain(&state, false);
  assert(!strcmp(state.screen_page, "app"));
  selected_kind = PET_HTTPS_SUBMIT;
  assert(pet_settings_open(&state, 20));
  pet_settings_snapshot(&view);
  assert(!strcmp(view.feed_host, "POST service.example.org:443") && !strcmp(view.feed_target, "/account"));
  assert(strstr(view.write_label, "arrived") && !sends);
  pet_settings_touch(&state, 80, 450, 21);
  pet_settings_snapshot(&view);
  assert(sends == 1 && view.write_submitted && view.feed_result == PET_WRITE_UNKNOWN);
  pet_settings_touch(&state, 80, 380, 22);
  pet_settings_snapshot(&view);
  assert(view.confirm_remove && !view.feed_submitted);
  pet_settings_touch(&state,500,420,23);
  pet_settings_snapshot(&view);
  assert(sends == 1 && view.feed_submitted && !view.write_submitted);
  /* Removing the app while its account form is open invalidates the local UI. */
  private_source = false;
  pet_settings_maintain(&state, false);
  assert(!pet_settings_active(&state) && !strcmp(state.screen_page,"app"));
  test_masked_credentials_limits();
  test_wifi_edit_and_limits();
  test_av_ble();
  test_lan_settings();
  test_lan_volume();
  test_timezone_settings();
  test_touch_keeps_the_network_from_the_hit_test();
  test_saved_network_actions();
  test_community_confirmation();
  return 0;
}
