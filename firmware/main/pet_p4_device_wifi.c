#include "pet_p4_device_wifi.h"
#include "pet_p4_ble_shortcut.h"
#include "pet_p4_protocol.h"
#include "pet_p4_wifi_clock.h"

#if PET_P4_SYSTEM_WIFI
#include "pet_p4_wifi_profiles.h"
#include <stdatomic.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_hosted_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

#define WIFI_NAMESPACE "pet_wifi_local"
#define WIFI_LEGACY_KEY "station_v1"
#define WIFI_PROFILES_KEY "profiles_v2"
#define WIFI_GOT_IP BIT0
#define WIFI_DISCONNECTED BIT1
#define WIFI_LOST_IP BIT2
#define WIFI_TRANSPORT_FAILURE BIT3
#define WIFI_SCAN_RECORDS 36
#define WIFI_RETRY_FIRST_MS 15000
#define WIFI_RETRY_MAX_MS 120000
#define HOSTED_CHIP_ESP32C5 0x17u

typedef struct {
  pet_wifi_operation_t operation;
  pet_wifi_ap_t ap;
  pet_wifi_station_t station;
} request_t;

static QueueHandle_t requests;
static QueueHandle_t snapshots;
static EventGroupHandle_t events;
static atomic_bool cancelled;
static atomic_bool request_pending;
static atomic_bool online;
static atomic_bool reboot_requested;
static atomic_bool reboot_ready;
static atomic_uint transport_failures;
static atomic_uint disconnect_reason;
static portMUX_TYPE link_lock = portMUX_INITIALIZER_UNLOCKED;
static pet_wifi_link_status_t link_status;
static uint8_t attempted_bssid[6];
static uint8_t attempted_channel;
static bool event_matches_ap;
static bool started;
static bool legacy_phy;
static bool dual_band;
static pet_wifi_snapshot_t view;
static pet_wifi_profiles_t saved, pending_profiles;
static pet_wifi_ap_t legacy_network;
static bool profiles_on_flash, storage_read_failed;
static uint8_t connected_security;
// A boot-local identifier for an association whose SSID and security matched
// the requested station. Never expose the network name in LAN status.
static atomic_uint association_revision = 1, verified_association;

uint32_t pet_device_wifi_association(void) {
  uint32_t current = atomic_load(&association_revision);
  return pet_device_wifi_online() && current == atomic_load(&verified_association) ? current : 0;
}

static void clear_memory(void *memory, size_t size) {
  volatile unsigned char *p = memory;
  while (size--) *p++ = 0;
}

static void publish(void) {
  view.saved_count = saved.count;
  for (unsigned i = 0; i < saved.count; ++i) {
    const pet_wifi_profile_t *profile = &saved.items[i];
    pet_wifi_saved_network_t *entry = &view.saved[i];
    memcpy(entry->ssid, profile->station.ssid, sizeof(entry->ssid));
    entry->security = profile->station.security;
    entry->auto_join = profile->auto_join;
    entry->current = connected_security == entry->security && !strcmp(view.connected, entry->ssid);
  }
  memset(&view.saved[saved.count], 0, (PET_WIFI_SAVED_MAX - saved.count) * sizeof(view.saved[0]));
  atomic_store(&online, view.connected[0] != 0);
  xQueueOverwrite(snapshots, &view);
}

bool pet_device_wifi_online(void) {
  return atomic_load(&online);
}

int pet_device_wifi_suspend_power_save(void) {
  wifi_ps_type_t previous;
  if (esp_wifi_get_ps(&previous) != ESP_OK || previous == WIFI_PS_NONE ||
      esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) return -1;
  return previous;
}

void pet_device_wifi_restore_power_save(int previous) {
  if (previous >= 0) (void)esp_wifi_set_ps((wifi_ps_type_t)previous);
}

bool pet_device_wifi_prepare_reboot(void) {
  if (!requests) return true;
  atomic_store(&reboot_requested, true);
  return atomic_load(&reboot_ready);
}

void pet_device_wifi_note_https(pet_wifi_https_stage_t stage, int error) {
  portENTER_CRITICAL(&link_lock);
  link_status.last_stage = stage;
  link_status.last_error = error;
  if (stage == PET_WIFI_HTTPS_READY) ++link_status.successes;
  else ++link_status.failures;
  portEXIT_CRITICAL(&link_lock);
}

pet_wifi_link_status_t pet_device_wifi_link_status(void) {
  portENTER_CRITICAL(&link_lock);
  pet_wifi_link_status_t copy = link_status;
  portEXIT_CRITICAL(&link_lock);
  copy.transport_failures = atomic_load(&transport_failures);
  return copy;
}

static void note_join(pet_wifi_join_stage_t stage, int error, bool interactive) {
  portENTER_CRITICAL(&link_lock);
  link_status.join_stage = stage;
  link_status.join_error = error;
  link_status.disconnect_reason = stage == PET_WIFI_JOIN_DISCONNECTED
    ? atomic_load(&disconnect_reason) : 0;
  if (interactive) {
    link_status.manual_join_stage = stage;
    link_status.manual_disconnect_reason = link_status.disconnect_reason;
    link_status.manual_legacy_phy = legacy_phy;
    link_status.manual_event_channel = attempted_channel;
    link_status.manual_event_matches_ap = stage == PET_WIFI_JOIN_DISCONNECTED && event_matches_ap;
  }
  portEXIT_CRITICAL(&link_lock);
}

static esp_err_t write_profiles(nvs_handle_t nvs, const pet_wifi_profiles_t *profiles) {
  esp_err_t result = nvs_set_blob(nvs, WIFI_PROFILES_KEY, profiles, sizeof(*profiles));
  if (result == ESP_OK) result = nvs_commit(nvs);
  if (result == ESP_OK) profiles_on_flash = true;
  return result;
}

static esp_err_t update_legacy(nvs_handle_t nvs, pet_wifi_profile_t *profile,
                               const pet_wifi_station_t *replacement) {
  if (!pet_wifi_same_network(&profile->station, legacy_network.ssid, legacy_network.security))
    return ESP_OK;

  esp_err_t result = ESP_OK;
  if (replacement && profile->legacy_kept) {
    /* The legacy key is authoritative for this mirrored password. Write the
     * verified credential first so either firmware can recover after power loss. */
    result = nvs_set_blob(nvs, WIFI_LEGACY_KEY, replacement, sizeof(*replacement));
    if (result == ESP_OK) result = nvs_commit(nvs);
    if (result == ESP_OK) profile->station = *replacement;
    return result;
  }

  /* Mark our own legacy-key removal before erasing it, preserving the
   * credentials and policy if power fails before the final profile edit. */
  if (profile->legacy_kept) {
    profile->legacy_kept = 0;
    result = write_profiles(nvs, &saved);
    if (result != ESP_OK) profile->legacy_kept = 1;
    const int next = pet_wifi_profiles_find(&pending_profiles, legacy_network.ssid, legacy_network.security);
    if (next >= 0) pending_profiles.items[next].legacy_kept = 0;
  }
  if (result == ESP_OK) {
    result = nvs_erase_key(nvs, WIFI_LEGACY_KEY);
    if (result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND) result = nvs_commit(nvs);
    if (result == ESP_OK) legacy_network.ssid[0] = 0;
  }
  return result;
}

static esp_err_t commit_profiles(int legacy_index, const pet_wifi_station_t *replacement) {
  nvs_handle_t nvs;
  esp_err_t result = storage_read_failed ? ESP_FAIL : nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &nvs);
  if (result == ESP_OK) {
    if (legacy_index >= 0) result = update_legacy(nvs, &saved.items[legacy_index], replacement);
    if (result == ESP_OK) result = write_profiles(nvs, &pending_profiles);
    nvs_close(nvs);
    if (result == ESP_OK) saved = pending_profiles;
  }
  clear_memory(&pending_profiles, sizeof(pending_profiles));
  return result;
}

static void load_profiles(void) {
  nvs_handle_t nvs;
  esp_err_t result = nvs_open(WIFI_NAMESPACE, NVS_READONLY, &nvs);
  if (result == ESP_ERR_NVS_NOT_FOUND) return;
  if (result != ESP_OK) {
    storage_read_failed = true;
    return;
  }

  size_t size = sizeof(saved);
  result = nvs_get_blob(nvs, WIFI_PROFILES_KEY, &saved, &size);
  profiles_on_flash = result == ESP_OK && size == sizeof(saved) && pet_wifi_profiles_valid(&saved);
  if (!profiles_on_flash) clear_memory(&saved, sizeof(saved));
  storage_read_failed = !profiles_on_flash && result != ESP_ERR_NVS_NOT_FOUND;

  pet_wifi_station_t legacy = {0};
  size = sizeof(legacy);
  const esp_err_t legacy_result = nvs_get_blob(nvs, WIFI_LEGACY_KEY, &legacy, &size);
  if (legacy_result == ESP_OK && size == sizeof(legacy) && pet_wifi_station_valid(&legacy)) {
    memcpy(legacy_network.ssid, legacy.ssid, sizeof(legacy_network.ssid));
    legacy_network.security = legacy.security;
    if (result == ESP_ERR_NVS_NOT_FOUND) {
      saved.count = 1;
      saved.items[0] = (pet_wifi_profile_t){.station = legacy, .auto_join = true, .legacy_kept = 1};
    }
  } else if (legacy_result != ESP_ERR_NVS_NOT_FOUND) storage_read_failed = true;
  nvs_close(nvs);

  if (!storage_read_failed && profiles_on_flash) {
    for (unsigned i = 0; i < saved.count; ++i) {
      pet_wifi_profile_t *profile = &saved.items[i];
      if (!profile->legacy_kept) continue;
      if (pet_wifi_same_network(&profile->station, legacy.ssid, legacy.security)) {
        if (!strcmp(profile->station.password, legacy.password)) break;
        profile->station = legacy;
      } else if (legacy_result == ESP_ERR_NVS_NOT_FOUND) {
        pet_wifi_profiles_remove(&saved, i);
      } else {
        // The old firmware switched its only slot to another network.
        profile->legacy_kept = 0;
      }
      pending_profiles = saved;
      if (commit_profiles(-1, NULL) != ESP_OK) view.saved_update_failed = true;
      break;
    }
  }
  clear_memory(&legacy, sizeof(legacy));
}

static esp_err_t save_joined_station(const pet_wifi_station_t *station, int index) {
  const bool changed = index >= 0 && strcmp(saved.items[index].station.password, station->password);
  if (index == 0 && !changed && profiles_on_flash) return ESP_OK;
  pending_profiles = saved;
  const unsigned position = index < 0 ? saved.count : (unsigned)index;
  if (index < 0) ++pending_profiles.count;
  memmove(&pending_profiles.items[1], &pending_profiles.items[0],
          position * sizeof(pending_profiles.items[0]));
  pending_profiles.items[0] = (pet_wifi_profile_t){
    .station = *station,
    .auto_join = index < 0 || saved.items[index].auto_join,
    .legacy_kept = index >= 0 && saved.items[index].legacy_kept,
  };
  return commit_profiles(changed ? index : -1, changed ? station : NULL);
}

static esp_err_t update_saved_profile(unsigned index, pet_wifi_operation_t operation) {
  const bool enabled = operation == PET_WIFI_AUTO_ON;
  if (operation != PET_WIFI_FORGET && saved.items[index].auto_join == enabled) return ESP_OK;
  pending_profiles = saved;
  if (operation == PET_WIFI_FORGET) pet_wifi_profiles_remove(&pending_profiles, index);
  else pending_profiles.items[index].auto_join = enabled;
  return commit_profiles(enabled ? -1 : (int)index, NULL);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg;
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    pet_p4_finish_board_probe(true);
    xEventGroupClearBits(events, WIFI_LOST_IP);
    xEventGroupSetBits(events, WIFI_GOT_IP);
  }
  if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
    atomic_fetch_add(&association_revision, 1);
    xEventGroupClearBits(events, WIFI_GOT_IP);
    xEventGroupSetBits(events, WIFI_LOST_IP);
  }
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    atomic_fetch_add(&association_revision, 1);
    const wifi_event_sta_disconnected_t *disconnected = data;
    if (disconnected) {
      portENTER_CRITICAL(&link_lock);
      atomic_store(&disconnect_reason, disconnected->reason);
      event_matches_ap = attempted_channel &&
        memcmp(disconnected->bssid, attempted_bssid, sizeof(attempted_bssid)) == 0;
      portEXIT_CRITICAL(&link_lock);
    }
    xEventGroupSetBits(events, WIFI_DISCONNECTED);
  }
  if (base == ESP_HOSTED_EVENT && id == ESP_HOSTED_EVENT_TRANSPORT_FAILURE) {
    atomic_fetch_add(&association_revision, 1);
    atomic_fetch_add(&transport_failures, 1);
    xEventGroupSetBits(events, WIFI_TRANSPORT_FAILURE);
  }
}

static atomic_uint hosted_probe_step;
unsigned pet_device_wifi_hosted_probe_step(void) { return atomic_load(&hosted_probe_step); }
static bool start_radio(void) {
  if (started) return true;
  // netif/event-loop setup is idempotent. STA creation installs handlers and
  // starts owning SDK resources, so never repeat a partially completed setup.
  static bool sta_attempted;
  static unsigned early_failures;
  view.status = PET_WIFI_REBOOT_REQUIRED;
  if (sta_attempted) return false;
  esp_err_t result = esp_netif_init();
  if (result == ESP_OK || result == ESP_ERR_INVALID_STATE)
    result = esp_event_loop_create_default();
  esp_netif_t *sta = NULL;
  if (result == ESP_OK || result == ESP_ERR_INVALID_STATE) {
    const esp_netif_config_t config = ESP_NETIF_DEFAULT_WIFI_STA();
    sta = esp_netif_new(&config);
  }
  if (!sta) {
    if (early_failures < 3) ++early_failures;
    view.status = early_failures < 3 ? PET_WIFI_INIT_RETRY : PET_WIFI_UNAVAILABLE;
    return false;
  }
  sta_attempted = true;
  // The SDK convenience constructor aborts on allocation/attach/handler errors.
  if (esp_netif_attach_wifi_station(sta) != ESP_OK ||
      esp_wifi_set_default_wifi_sta_handlers() != ESP_OK) return false;
  atomic_store(&hosted_probe_step,1);
  result = esp_hosted_init();
  if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return false;
  atomic_store(&hosted_probe_step,2);
  wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
  config.nvs_enable = 0;
  if (esp_wifi_init(&config) != ESP_OK ||
      esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK ||
      esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
      esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event, NULL) != ESP_OK ||
      esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL) != ESP_OK ||
      esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, wifi_event, NULL) != ESP_OK ||
      esp_event_handler_register(ESP_HOSTED_EVENT, ESP_HOSTED_EVENT_TRANSPORT_FAILURE,
                                 wifi_event, NULL) != ESP_OK)
    return false;
  // C6 keeps the validated single-band radio settings. Only a C5 identified
  // in the Hosted INIT handshake receives a dual-band RPC.
  if (pet_p4_hosted_radio_chip_id() == HOSTED_CHIP_ESP32C5) {
    const esp_err_t band_error = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    dual_band = band_error == ESP_OK;
    portENTER_CRITICAL(&link_lock);
    link_status.dual_band = dual_band;
    link_status.band_mode_error = band_error;
    portEXIT_CRITICAL(&link_lock);
  }
  atomic_store(&hosted_probe_step,3);
  wifi_config_t empty = {0};
  (void)esp_wifi_set_config(WIFI_IF_STA, &empty);
  started = esp_wifi_start() == ESP_OK;
  if (started) {
    view.status = PET_WIFI_IDLE;
    atomic_store(&hosted_probe_step,4);
    esp_hosted_coprocessor_fwver_t version = {0};
    if (esp_hosted_get_coprocessor_fwversion(&version) == ESP_OK &&
        (version.major1 || version.minor1 || version.patch1) &&
        version.major1 < 256 && version.minor1 < 256 && version.patch1 < 256) {
      portENTER_CRITICAL(&link_lock);
      link_status.radio_version = version.major1 << 16 | version.minor1 << 8 | version.patch1;
      atomic_store(&hosted_probe_step,5);
      portEXIT_CRITICAL(&link_lock);
      pet_p4_finish_board_probe(true);
    }
    ble_budget_probe();
  }
  return started;
}

static uint8_t security_for(wifi_auth_mode_t mode) {
  switch (mode) {
    case WIFI_AUTH_OPEN: return PET_WIFI_OPEN;
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK: return PET_WIFI_PERSONAL;
    case WIFI_AUTH_WPA3_PSK: return PET_WIFI_WPA3;
    default: return PET_WIFI_UNSUPPORTED;
  }
}

static bool security_matches(uint8_t requested, wifi_auth_mode_t actual) {
  const uint8_t security = security_for(actual);
  return security == requested ||
    (requested == PET_WIFI_PERSONAL && security == PET_WIFI_WPA3);
}

static EXT_RAM_BSS_ATTR wifi_ap_record_t records[WIFI_SCAN_RECORDS];

static void scan(void) {
  view.status = PET_WIFI_SCANNING;
  view.count = 0;
  publish();
  if (esp_wifi_scan_start(NULL, true) != ESP_OK) {
    view.status = PET_WIFI_SCAN_FAILED;
    return;
  }
  // Fetch more BSSIDs than the UI can list, so mesh nodes and extenders
  // don't displace all but the strongest names. Only this worker uses it.
  uint16_t found = 0;
  (void)esp_wifi_scan_get_ap_num(&found);
  uint16_t count = WIFI_SCAN_RECORDS;
  if (esp_wifi_scan_get_ap_records(&count, records) != ESP_OK) {
    (void)esp_wifi_clear_ap_list();
    view.status = PET_WIFI_SCAN_FAILED;
    return;
  }
  if (atomic_load(&hosted_probe_step)>=4) {
    atomic_store(&hosted_probe_step,6);
    pet_p4_finish_board_probe(true);
  }
  for (unsigned i = 0; i < count && view.count < PET_WIFI_AP_MAX; ++i) {
    const uint8_t security = security_for(records[i].authmode);
    if (!records[i].ssid[0]) continue;
    bool duplicate = false;
    for (unsigned j = 0; j < view.count; ++j)
      if (view.aps[j].security == security &&
          strncmp(view.aps[j].ssid, (const char *)records[i].ssid, PET_WIFI_SSID_SIZE) == 0)
        duplicate = true;
    if (duplicate) continue;
    pet_wifi_ap_t *ap = &view.aps[view.count++];
    memcpy(ap->ssid, records[i].ssid, PET_WIFI_SSID_SIZE - 1);
    ap->ssid[PET_WIFI_SSID_SIZE - 1] = 0;
    ap->rssi = records[i].rssi;
    ap->security = security;
    ap->channel = records[i].primary;
    memcpy(ap->bssid, records[i].bssid, sizeof(ap->bssid));
  }
  portENTER_CRITICAL(&link_lock);
  link_status.scan_found = found < count ? count : found;
  link_status.scan_sampled = count;
  link_status.scan_listed = view.count;
  portEXIT_CRITICAL(&link_lock);
  view.status = view.connected[0] ? PET_WIFI_CONNECTED : PET_WIFI_IDLE;
}

static bool join_current(bool manual) {
  return !atomic_load(&reboot_requested) && (manual ? !atomic_load(&cancelled) : !uxQueueMessagesWaiting(requests));
}

static bool associated_ap_matches(const wifi_ap_record_t *ap, const char *ssid, uint8_t security) {
  return security_matches(security, ap->authmode) &&
         strncmp((const char *)ap->ssid, ssid, PET_WIFI_SSID_SIZE) == 0;
}

static bool wait_for_station_address(const pet_wifi_station_t *station, bool interactive) {
  pet_wifi_join_stage_t stage = PET_WIFI_JOIN_ADDRESS_TIMEOUT;
  esp_err_t result = ESP_OK;
  for (int step = 0; step < 120 && join_current(interactive); ++step) {
    const EventBits_t bits = xEventGroupWaitBits(events, WIFI_GOT_IP | WIFI_DISCONNECTED,
                                                 pdFALSE, pdFALSE, pdMS_TO_TICKS(250));
    if (bits & WIFI_DISCONNECTED) {
      stage = PET_WIFI_JOIN_DISCONNECTED;
      break;
    }
    if (!(bits & WIFI_GOT_IP)) continue;

    const uint32_t revision = atomic_load(&association_revision);
    wifi_ap_record_t ap;
    result = esp_wifi_sta_get_ap_info(&ap);
    if (result != ESP_OK) {
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    // A lease or association can disappear while the Hosted RPC is in flight.
    if ((xEventGroupGetBits(events) & (WIFI_GOT_IP | WIFI_DISCONNECTED)) != WIFI_GOT_IP) continue;
    if (!associated_ap_matches(&ap, station->ssid, station->security)) {
      stage = PET_WIFI_JOIN_AP_MISMATCH;
      break;
    }
    if (!join_current(interactive)) break;
    portENTER_CRITICAL(&link_lock);
    link_status.connected_channel = ap.primary;
    portEXIT_CRITICAL(&link_lock);
    atomic_store(&verified_association, revision);
    connected_security = station->security;
    memcpy(view.connected, station->ssid, sizeof(view.connected));
    note_join(PET_WIFI_JOIN_OK, 0, interactive);
    return true;
  }
  note_join(stage, result, interactive);
  return false;
}

static bool connect_station(const pet_wifi_station_t *station, const pet_wifi_ap_t *ap_hint,
                            bool interactive, bool legacy) {
  atomic_fetch_add(&association_revision, 1);
  xEventGroupClearBits(events, WIFI_GOT_IP | WIFI_DISCONNECTED | WIFI_LOST_IP);
  if (esp_wifi_disconnect() == ESP_OK)
    (void)xEventGroupWaitBits(events, WIFI_DISCONNECTED, pdTRUE, pdFALSE, pdMS_TO_TICKS(800));
  xEventGroupClearBits(events, WIFI_GOT_IP | WIFI_DISCONNECTED | WIFI_LOST_IP | WIFI_TRANSPORT_FAILURE);
  portENTER_CRITICAL(&link_lock);
  atomic_store(&disconnect_reason, 0);
  attempted_channel = ap_hint ? ap_hint->channel : 0;
  if (attempted_channel) memcpy(attempted_bssid, ap_hint->bssid, sizeof(attempted_bssid));
  event_matches_ap = false;
  portEXIT_CRITICAL(&link_lock);
  note_join(PET_WIFI_JOIN_NONE, 0, interactive);
  view.connected[0] = 0;
  if (interactive) {
    view.status = PET_WIFI_CONNECTING;
    publish();
  }
  if (!join_current(interactive)) return false;

  if (legacy != legacy_phy) {
    const uint8_t protocols = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
    const uint8_t desired = legacy ? protocols : protocols | WIFI_PROTOCOL_11AX;
    esp_err_t changed;
    if (dual_band) {
      // The old-router fallback changes only 2.4 GHz. Keep the radio's 5 GHz settings.
      wifi_protocols_t bands = {0};
      changed = esp_wifi_get_protocols(WIFI_IF_STA, &bands);
      if (changed == ESP_OK) {
        bands.ghz_2g = desired;
        changed = esp_wifi_set_protocols(WIFI_IF_STA, &bands);
      }
    } else {
      changed = esp_wifi_set_protocol(WIFI_IF_STA, desired);
    }
    if (changed != ESP_OK) {
      note_join(PET_WIFI_JOIN_CONFIG, changed, interactive);
      return false;
    }
    legacy_phy = legacy;
  }

  wifi_config_t config = {0};
  memcpy(config.sta.ssid, station->ssid, strlen(station->ssid));
  memcpy(config.sta.password, station->password, strlen(station->password));
  config.sta.threshold.authmode = station->security == PET_WIFI_WPA3 ? WIFI_AUTH_WPA3_PSK :
    station->security == PET_WIFI_PERSONAL ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  // Hosted forwards this threshold to the radio. Set it explicitly so the
  // strongest eligible AP can still be selected at a negative RSSI.
  config.sta.threshold.rssi = -127;
  if (ap_hint && ap_hint->channel) {
    config.sta.channel = ap_hint->channel;
    config.sta.bssid_set = true;
    memcpy(config.sta.bssid, ap_hint->bssid, sizeof(config.sta.bssid));
  }
  config.sta.pmf_cfg.capable = true;
  config.sta.pmf_cfg.required = station->security == PET_WIFI_WPA3;
  // A WPA2/WPA3 transition AP can negotiate SAE even with a WPA2 scan threshold.
  if (station->security != PET_WIFI_OPEN)
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
  esp_err_t result = esp_wifi_set_config(WIFI_IF_STA, &config);
  clear_memory(&config, sizeof(config));
  pet_wifi_join_stage_t stage = PET_WIFI_JOIN_CONFIG;
  if (result == ESP_OK) {
    stage = PET_WIFI_JOIN_START;
    result = esp_wifi_connect();
  }
  if (result == ESP_OK) {
    if (wait_for_station_address(station, interactive)) return true;
  } else {
    note_join(stage, result, interactive);
  }
  (void)esp_wifi_disconnect();
  return false;
}

/* Saved profiles intentionally omit the BSSID. If a blind join cannot find
 * the AP, actively rediscover just that SSID, then try its strongest eligible
 * radio once. The worker owns the shared scan buffer throughout. */
static bool rediscover_station(const pet_wifi_station_t *station, pet_wifi_ap_t *hint) {
  wifi_scan_config_t scan_config = {.ssid=(uint8_t *)station->ssid, .show_hidden=true};
  if (esp_wifi_scan_start(&scan_config, true) != ESP_OK) return false;
  uint16_t count = WIFI_SCAN_RECORDS;
  if (esp_wifi_scan_get_ap_records(&count, records) != ESP_OK) {
    (void)esp_wifi_clear_ap_list();
    return false;
  }
  for (unsigned i=0; i<count; ++i) {
    const wifi_ap_record_t *ap=&records[i];
    if (!ap->primary || strncmp((const char *)ap->ssid,station->ssid,PET_WIFI_SSID_SIZE) ||
        !security_matches(station->security,ap->authmode)) continue;
    hint->channel=ap->primary;
    memcpy(hint->bssid,ap->bssid,sizeof(hint->bssid));
    return true; /* SDK scan records are sorted by descending RSSI. */
  }
  return false;
}

static bool handshake_timeout(unsigned reason) {
  return reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
}

static bool join_with_recovery(const pet_wifi_station_t *station, const pet_wifi_ap_t *hint, bool manual) {
  if (connect_station(station,hint,manual,false)) return true;
  if (!join_current(manual)) return false;
  unsigned reason=pet_device_wifi_link_status().disconnect_reason;
  if (handshake_timeout(reason)) return connect_station(station,hint,manual,true);
  if (reason!=WIFI_REASON_NO_AP_FOUND) return false;
  /* A selected mesh node can disappear. First release that pin and let the
   * driver find another node. A saved/unpinned join instead uses a fresh scan. */
  pet_wifi_ap_t found={0};
  if (hint && hint->channel) return connect_station(station,NULL,manual,false);
  return rediscover_station(station,&found) && join_current(manual) &&
         connect_station(station,&found,manual,false);
}

static void join_requested(request_t *request, int index) {
  const int previous = pet_wifi_profiles_find(&saved, view.connected, connected_security);
  const bool previous_legacy_phy = legacy_phy;
  view.join_reason = 0;
  const pet_wifi_ap_t *hint = request->operation == PET_WIFI_CONNECT ? &request->ap : NULL;
  bool joined = join_with_recovery(&request->station,hint,true);
  joined = joined && !atomic_load(&cancelled) && !atomic_load(&reboot_requested);
  const esp_err_t result = joined ? save_joined_station(&request->station, index) : ESP_FAIL;
  if (joined && result == ESP_OK) {
    view.status = PET_WIFI_CONNECTED;
    return;
  }

  if (joined) {
    note_join(PET_WIFI_JOIN_SAVE, result, true);
    view.saved_update_failed = true;
  }
  view.join_reason = pet_device_wifi_link_status().manual_disconnect_reason;
  view.join_stage = pet_device_wifi_link_status().manual_join_stage;
  view.connected[0] = 0;
  // Undo this manual switch once, independently of the saved auto-join policy.
  if (previous >= 0 && !atomic_load(&reboot_requested))
    (void)connect_station(&saved.items[previous].station, NULL, false, previous_legacy_phy);
  else
    (void)esp_wifi_disconnect();
  view.status = atomic_load(&cancelled)
    ? (view.connected[0] ? PET_WIFI_CONNECTED : PET_WIFI_IDLE) : PET_WIFI_FAILED;
}

static bool connected_link_lost(void) {
  const EventBits_t bits = xEventGroupClearBits(events, WIFI_DISCONNECTED | WIFI_LOST_IP |
                                                        WIFI_TRANSPORT_FAILURE);
  if (bits & (WIFI_DISCONNECTED | WIFI_LOST_IP)) return true;
  if (!(bits & WIFI_TRANSPORT_FAILURE)) return false;

  // SDIO can report a failed transfer without a station disconnect event.
  // Query the coprocessor once per batch so a recovered transfer preserves the link.
  wifi_ap_record_t ap;
  return esp_wifi_sta_get_ap_info(&ap) != ESP_OK ||
         !associated_ap_matches(&ap, view.connected, connected_security);
}

static void wifi_task(void *arg) {
  (void)arg;
  pet_wifi_clock_load_offset();
  load_profiles();
  if (storage_read_failed) view.saved_update_failed = true;
  publish();
  TickType_t retry = xTaskGetTickCount();
  unsigned retry_delay_ms = WIFI_RETRY_FIRST_MS;
  bool attempted_ble_restore=false;
  unsigned retry_index = 0;
  for (;;) {
    if (atomic_load(&reboot_requested)) {
      view.connected[0] = 0;
      view.status = PET_WIFI_IDLE;
      publish();
      pet_wifi_clock_service(false);
      if (started) (void)esp_wifi_stop();
      atomic_store(&reboot_ready, true);
      vTaskSuspend(NULL);
    }
    if (started && view.connected[0] && connected_link_lost()) {
      view.connected[0] = 0;
      view.status = PET_WIFI_IDLE;
      retry_index = 0;
      retry = xTaskGetTickCount();
      retry_delay_ms = WIFI_RETRY_FIRST_MS;
      publish();
    }
    pet_wifi_clock_service(pet_device_wifi_online());
    request_t request = {0};
    if (xQueueReceive(requests, &request, pdMS_TO_TICKS(500)) == pdTRUE) {
      const int index = pet_wifi_profiles_find(&saved, request.ap.ssid, request.ap.security);
      if (request.operation == PET_WIFI_FORGET ||
          request.operation == PET_WIFI_AUTO_ON || request.operation == PET_WIFI_AUTO_OFF) {
        const bool current = connected_security == request.ap.security &&
          !strcmp(view.connected, request.ap.ssid);
        view.saved_update_failed = index < 0 ||
          update_saved_profile((unsigned)index, request.operation) != ESP_OK;
        if (!view.saved_update_failed) {
          if (request.operation == PET_WIFI_FORGET && current && started) {
            (void)esp_wifi_disconnect();
            wifi_config_t empty = {0};
            (void)esp_wifi_set_config(WIFI_IF_STA, &empty);
            view.connected[0] = 0;
            view.status = PET_WIFI_IDLE;
            view.join_reason = 0;
          } else if (view.status == PET_WIFI_LIST_FULL && request.operation == PET_WIFI_FORGET) {
            view.status = view.connected[0] ? PET_WIFI_CONNECTED : PET_WIFI_IDLE;
          }
          retry_index = 0;
          retry = xTaskGetTickCount();
          retry_delay_ms = WIFI_RETRY_FIRST_MS;
        }
      } else if ((request.operation == PET_WIFI_CONNECT || request.operation == PET_WIFI_CONNECT_SAVED) &&
                 (storage_read_failed || (index < 0 && request.operation == PET_WIFI_CONNECT_SAVED))) {
        view.saved_update_failed = true;
      } else if (request.operation == PET_WIFI_CONNECT && index < 0 && saved.count == PET_WIFI_SAVED_MAX) {
        view.status = PET_WIFI_LIST_FULL;
      } else if (!start_radio()) {
        retry_delay_ms = WIFI_RETRY_FIRST_MS;
        retry = xTaskGetTickCount() + pdMS_TO_TICKS(retry_delay_ms);
      } else if (request.operation == PET_WIFI_SCAN) {
        scan();
      } else if (request.operation == PET_WIFI_START) {
        /* A completed scan also proves a working C6 if version RPC is unavailable. */
        if (!pet_p4_is_wifi6_hardware()) scan();
      } else if (request.operation == PET_WIFI_CONNECT || request.operation == PET_WIFI_CONNECT_SAVED) {
        if (request.operation == PET_WIFI_CONNECT_SAVED) request.station = saved.items[index].station;
        view.saved_update_failed = false;
        join_requested(&request, index);
        retry_index = 0;
        retry_delay_ms = WIFI_RETRY_FIRST_MS;
        retry = xTaskGetTickCount() + pdMS_TO_TICKS(retry_delay_ms);
      }
      clear_memory(&request, sizeof(request));
      publish();
      atomic_store(&request_pending, false);
      continue;
    }
    const unsigned next_auto = pet_wifi_next_auto(&saved, retry_index);
    if (!pet_ble_shortcut_restore_requested()) attempted_ble_restore = false;
    else if (!started && next_auto >= saved.count && !attempted_ble_restore) {
      attempted_ble_restore = true;
      (void)start_radio();
    }
    const bool init_retry = view.status == PET_WIFI_INIT_RETRY;
    if ((init_retry || next_auto < saved.count) && !view.connected[0] && view.status != PET_WIFI_UNAVAILABLE &&
        view.status != PET_WIFI_REBOOT_REQUIRED &&
        (int32_t)(xTaskGetTickCount() - retry) >= 0) {
      const bool ready = start_radio();
      if (ready && next_auto >= saved.count) {
        // Finish discovery even on the first boot with no saved credentials.
        scan();
      } else if (ready) {
        const pet_wifi_station_t *candidate = &saved.items[next_auto].station;
        bool restored = join_with_recovery(candidate,NULL,false);
        if (!restored && uxQueueMessagesWaiting(requests)) continue;
        if (restored) {
          retry_index = 0;
          view.status = PET_WIFI_CONNECTED;
          view.join_reason = 0;
          retry_delay_ms = WIFI_RETRY_FIRST_MS;
        } else {
          retry_index = pet_wifi_next_auto(&saved, next_auto + 1);
          if (retry_index < saved.count) {
            retry = xTaskGetTickCount();
            continue;
          }
          retry_index = 0;
          if (view.status == PET_WIFI_IDLE || (view.status == PET_WIFI_FAILED && !view.join_reason)) {
            view.join_reason = pet_device_wifi_link_status().disconnect_reason;
            view.join_stage = pet_device_wifi_link_status().join_stage;
            view.status = PET_WIFI_FAILED;
          }
        }
      }
      retry = xTaskGetTickCount() + pdMS_TO_TICKS(retry_delay_ms);
      if (retry_delay_ms < WIFI_RETRY_MAX_MS) retry_delay_ms *= 2;
      publish();
    }
  }
}

void pet_device_wifi_start(void) {
  if (requests) return;
  requests = xQueueCreate(1, sizeof(request_t));
  snapshots = xQueueCreate(1, sizeof(view));
  events = xEventGroupCreate();
  if (requests && snapshots && events &&
      xTaskCreate(wifi_task, "pet_wifi", 5120, NULL, 2, NULL) == pdPASS) return;
  if (requests) vQueueDelete(requests);
  if (snapshots) vQueueDelete(snapshots);
  if (events) vEventGroupDelete(events);
  requests = NULL;
  snapshots = NULL;
  events = NULL;
}

bool pet_device_wifi_request(pet_wifi_operation_t operation, const pet_wifi_ap_t *ap,
                             const char *password) {
  if (!requests || atomic_load(&reboot_requested)) return false;
  request_t request = {.operation = operation};
  if (operation == PET_WIFI_CONNECT || operation == PET_WIFI_CONNECT_SAVED ||
      operation == PET_WIFI_FORGET || operation == PET_WIFI_AUTO_ON || operation == PET_WIFI_AUTO_OFF) {
    if (!ap || !ap->ssid[0] || ap->security > PET_WIFI_WPA3 ||
        !memchr(ap->ssid, 0, sizeof(ap->ssid))) return false;
    request.ap = *ap;
  }
  if (operation == PET_WIFI_CONNECT) {
    if (!password) return false;
    size_t length = strlen(password);
    if (length >= PET_WIFI_PASSWORD_SIZE - 1 || (ap->security == PET_WIFI_OPEN && length) ||
        (ap->security != PET_WIFI_OPEN && length < 8)) return false;
    memcpy(request.station.ssid, ap->ssid, sizeof(request.station.ssid));
    memcpy(request.station.password, password, length + 1);
    request.station.security = ap->security;
  }
  bool queued = false;
  if (!atomic_exchange(&request_pending, true)) {
    atomic_store(&cancelled, false);
    queued = xQueueSend(requests, &request, 0) == pdTRUE;
    if (!queued) atomic_store(&request_pending, false);
  }
  clear_memory(&request, sizeof(request));
  return queued;
}

void pet_device_wifi_cancel(void) {
  atomic_store(&cancelled, true);
}

void pet_device_wifi_snapshot(pet_wifi_snapshot_t *snapshot) {
  const bool pending = atomic_load(&request_pending);
  memset(snapshot, 0, sizeof(*snapshot));
  if (!snapshots) snapshot->status = PET_WIFI_UNAVAILABLE;
  else (void)xQueuePeek(snapshots, snapshot, 0);
  snapshot->request_pending = pending;
}
#endif
