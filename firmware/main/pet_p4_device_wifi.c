#include "pet_p4_device_wifi.h"
#include "pet_p4_ble_shortcut.h"
#include "pet_p4_protocol.h"
#include "pet_p4_wifi_clock.h"
#include "pet_p4_wifi_profiles.h"

#if PET_P4_SYSTEM_WIFI
#include <stdatomic.h>
#include <stdio.h>
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
#define WIFI_PROFILES_KEY "stations_v2"
#define WIFI_MANUAL_ONLY_KEY "manual_only_v1"
#define WIFI_GOT_IP BIT0
#define WIFI_DISCONNECTED BIT1
#define WIFI_LOST_IP BIT2
#define WIFI_SCAN_RECORDS 36
#define WIFI_RETRY_FIRST_MS 15000
#define WIFI_RETRY_MAX_MS 120000
#define HOSTED_CHIP_ESP32C5 0x17u

typedef struct {
  pet_wifi_operation_t operation;
  pet_wifi_ap_t ap;
  pet_wifi_station_t station;
} request_t;

static esp_netif_t *station_netif;
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
static pet_wifi_manual_only_t manual_only;
static pet_wifi_saved_network_t legacy_network;
static uint8_t connected_security;
static bool profiles_on_flash;

static void clear_memory(void *memory, size_t size) {
  volatile unsigned char *p = memory;
  while (size--) *p++ = 0;
}

static void publish(void) {
  if (!view.connected[0]) memset(&view.connection, 0, sizeof(view.connection));
  view.saved_count = saved.count;
  for (unsigned i = 0; i < saved.count; ++i) {
    const pet_wifi_station_t *network = &saved.items[i];
    pet_wifi_saved_network_t *item = &view.saved[i];
    memcpy(item->ssid, network->ssid, sizeof(item->ssid));
    item->security = network->security;
    item->auto_join = pet_wifi_auto_allowed(&manual_only, network);
    item->current = view.connected[0] && connected_security == item->security &&
      !strcmp(view.connected, item->ssid);
  }
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

static esp_err_t commit_profiles(const pet_wifi_station_t *modified) {
  nvs_handle_t nvs;
  esp_err_t result = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &nvs);
  if (result == ESP_OK) {
    /* Preserve the old firmware's profile unless the user forgets it or replaces
     * its password. Ensure v2 already has the old list before erasing that key. */
    if (modified && legacy_network.security == modified->security &&
        !strcmp(legacy_network.ssid, modified->ssid)) {
      if (!profiles_on_flash) result = write_profiles(nvs, &saved);
      if (result == ESP_OK) {
        result = nvs_erase_key(nvs, WIFI_LEGACY_KEY);
        if (result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND) result = nvs_commit(nvs);
        if (result == ESP_OK) legacy_network.ssid[0] = 0;
      }
    }
    if (result == ESP_OK) result = write_profiles(nvs, &pending_profiles);
    nvs_close(nvs);
    if (result == ESP_OK) saved = pending_profiles;
  }
  clear_memory(&pending_profiles, sizeof(pending_profiles));
  return result;
}

static esp_err_t set_auto_join(const pet_wifi_station_t *station, bool enabled) {
  if (pet_wifi_auto_allowed(&manual_only, station) == enabled) return ESP_OK;
  pet_wifi_manual_only_t next = {0};
  for (unsigned i = 0; i < manual_only.count; ++i) {
    const pet_wifi_network_id_t *item = &manual_only.items[i];
    if (!pet_wifi_same_network(item, station->ssid, station->security) &&
        pet_wifi_profiles_find(&saved, item->ssid, item->security) >= 0)
      next.items[next.count++] = *item;
  }
  if (!enabled) {
    if (next.count >= PET_WIFI_SAVED_MAX) return ESP_ERR_INVALID_STATE;
    pet_wifi_network_id_t *item = &next.items[next.count++];
    memcpy(item->ssid, station->ssid, sizeof(item->ssid));
    item->security = station->security;
  }
  nvs_handle_t nvs;
  esp_err_t result = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &nvs);
  if (result != ESP_OK) return result;
  result = nvs_set_blob(nvs, WIFI_MANUAL_ONLY_KEY, &next, sizeof(next));
  if (result == ESP_OK) result = nvs_commit(nvs);
  nvs_close(nvs);
  if (result == ESP_OK) manual_only = next;
  return result;
}

static void load_profiles(void) {
  nvs_handle_t nvs;
  if (nvs_open(WIFI_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return;
  size_t size = sizeof(saved);
  const esp_err_t result = nvs_get_blob(nvs, WIFI_PROFILES_KEY, &saved, &size);
  profiles_on_flash = result == ESP_OK && size == sizeof(saved) && pet_wifi_profiles_valid(&saved);
  if (!profiles_on_flash) clear_memory(&saved, sizeof(saved));

  pet_wifi_station_t legacy = {0};
  size = sizeof(legacy);
  if (nvs_get_blob(nvs, WIFI_LEGACY_KEY, &legacy, &size) == ESP_OK &&
      size == sizeof(legacy) && pet_wifi_station_valid(&legacy)) {
    memcpy(legacy_network.ssid, legacy.ssid, sizeof(legacy.ssid));
    legacy_network.security = legacy.security;
    if (result == ESP_ERR_NVS_NOT_FOUND) pet_wifi_profiles_promote(&saved, 0, &legacy);
  }
  clear_memory(&legacy, sizeof(legacy));
  size = sizeof(manual_only);
  const esp_err_t policy_status = nvs_get_blob(nvs, WIFI_MANUAL_ONLY_KEY, &manual_only, &size);
  if (policy_status != ESP_OK || size != sizeof(manual_only) ||
      !pet_wifi_manual_only_valid(&manual_only)) {
    memset(&manual_only, 0, sizeof(manual_only));
    // A missing policy is the legacy default. A damaged policy must not
    // silently opt the device back into a network the user disabled.
    if (policy_status != ESP_ERR_NVS_NOT_FOUND) {
      manual_only.count = saved.count;
      for (unsigned i = 0; i < saved.count; ++i) {
        memcpy(manual_only.items[i].ssid, saved.items[i].ssid, sizeof(saved.items[i].ssid));
        manual_only.items[i].security = saved.items[i].security;
      }
      view.saved_update_failed = true;
    }
  }
  nvs_close(nvs);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg;
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    pet_p4_finish_board_probe(true);
    xEventGroupSetBits(events, WIFI_GOT_IP);
  }
  if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) xEventGroupSetBits(events, WIFI_LOST_IP);
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
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
  if (base == ESP_HOSTED_EVENT && id == ESP_HOSTED_EVENT_TRANSPORT_FAILURE)
    atomic_fetch_add(&transport_failures, 1);
}

static atomic_uint hosted_probe_step;
unsigned pet_device_wifi_hosted_probe_step(void) { return atomic_load(&hosted_probe_step); }
static bool start_radio(void) {
  if (started) return true;
  // A partially initialized Hosted driver is left for reboot recovery.
  static bool attempted;
  if (attempted) return false;
  attempted = true;
  esp_err_t result = esp_netif_init();
  if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return false;
  result = esp_event_loop_create_default();
  if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return false;
  station_netif = esp_netif_create_default_wifi_sta();
  if (!station_netif) return false;
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

static void update_connection(const wifi_ap_record_t *ap) {
  pet_wifi_connection_t info = {
    .rssi = ap->rssi,
    .channel = ap->primary,
    .security = security_for(ap->authmode),
  };
  esp_netif_ip_info_t ip;
  if (esp_netif_get_ip_info(station_netif, &ip) == ESP_OK && ip.ip.addr) {
    snprintf(info.ip, sizeof(info.ip), IPSTR, IP2STR(&ip.ip));
    if (ip.gw.addr) snprintf(info.gateway, sizeof(info.gateway), IPSTR, IP2STR(&ip.gw));
  }
  view.connection = info;
}

static void refresh_connection(void) {
  wifi_ap_record_t ap;
  if (view.connected[0] && esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
      !strncmp((const char *)ap.ssid, view.connected, PET_WIFI_SSID_SIZE))
    update_connection(&ap);
}

static bool security_matches(uint8_t requested, wifi_auth_mode_t actual) {
  const uint8_t security = security_for(actual);
  return security == requested ||
    (requested == PET_WIFI_PERSONAL && security == PET_WIFI_WPA3);
}

static EXT_RAM_BSS_ATTR wifi_ap_record_t records[WIFI_SCAN_RECORDS];

static void scan(void) {
  view.status = PET_WIFI_SCANNING;
  view.join_stage = PET_WIFI_JOIN_NONE;
  view.join_reason = 0;
  view.count = 0;
  publish();
  if (esp_wifi_scan_start(NULL, true) != ESP_OK) {
    view.status = PET_WIFI_FAILED;
    return;
  }
  // Fetch more BSSIDs than the UI can list, so mesh nodes and extenders
  // don't displace all but the strongest names. Only this worker uses it.
  uint16_t found = 0;
  (void)esp_wifi_scan_get_ap_num(&found);
  uint16_t count = WIFI_SCAN_RECORDS;
  if (esp_wifi_scan_get_ap_records(&count, records) != ESP_OK) {
    (void)esp_wifi_clear_ap_list();
    view.status = PET_WIFI_FAILED;
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

static bool connect_station(const pet_wifi_station_t *station, const pet_wifi_ap_t *ap_hint,
                            bool interactive, bool legacy) {
  xEventGroupClearBits(events, WIFI_GOT_IP | WIFI_DISCONNECTED | WIFI_LOST_IP);
  if (esp_wifi_disconnect() == ESP_OK)
    (void)xEventGroupWaitBits(events, WIFI_DISCONNECTED, pdTRUE, pdFALSE, pdMS_TO_TICKS(800));
  xEventGroupClearBits(events, WIFI_GOT_IP | WIFI_DISCONNECTED | WIFI_LOST_IP);
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
  if (atomic_load(&reboot_requested) ||
      (interactive ? atomic_load(&cancelled) : uxQueueMessagesWaiting(requests) != 0))
    return false;

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
    stage = PET_WIFI_JOIN_ADDRESS_TIMEOUT;
    bool got_ip = false;
    for (int step = 0; step < 120; ++step) {
      if (atomic_load(&reboot_requested) ||
          (interactive ? atomic_load(&cancelled) : uxQueueMessagesWaiting(requests) != 0)) break;
      EventBits_t bits = xEventGroupWaitBits(events, WIFI_GOT_IP | WIFI_DISCONNECTED,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(250));
      if (bits & WIFI_DISCONNECTED) {
        stage = PET_WIFI_JOIN_DISCONNECTED;
        break;
      }
      got_ip = got_ip || (bits & WIFI_GOT_IP);
      if (got_ip) {
        wifi_ap_record_t ap;
        result = esp_wifi_sta_get_ap_info(&ap);
        if (result != ESP_OK) continue;
        if (!security_matches(station->security, ap.authmode) ||
            strncmp((const char *)ap.ssid, station->ssid, PET_WIFI_SSID_SIZE)) {
          stage = PET_WIFI_JOIN_AP_MISMATCH;
          break;
        }
        portENTER_CRITICAL(&link_lock);
        link_status.connected_channel = ap.primary;
        portEXIT_CRITICAL(&link_lock);
        connected_security = station->security;
        memcpy(view.connected, station->ssid, sizeof(view.connected));
        update_connection(&ap);
        note_join(PET_WIFI_JOIN_OK, 0, interactive);
        return true;
      }
    }
  }
  note_join(stage, result, interactive);
  (void)esp_wifi_disconnect();
  return false;
}

static bool handshake_timeout(unsigned reason) {
  return reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
}

static void wifi_task(void *arg) {
  (void)arg;
  pet_wifi_clock_load_offset();
  load_profiles();
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
    pet_wifi_clock_service(pet_device_wifi_online());
    request_t request = {0};
    if (xQueueReceive(requests, &request, pdMS_TO_TICKS(500)) == pdTRUE) {
      const int index = pet_wifi_profiles_find(&saved, request.ap.ssid, request.ap.security);
      if (request.operation == PET_WIFI_AUTO_ON || request.operation == PET_WIFI_AUTO_OFF) {
        view.saved_update_failed = index < 0 ||
          set_auto_join(&saved.items[index], request.operation == PET_WIFI_AUTO_ON) != ESP_OK;
        if (!view.saved_update_failed) {
          retry_index = 0;
          retry = xTaskGetTickCount();
          retry_delay_ms = WIFI_RETRY_FIRST_MS;
        }
      } else if (request.operation == PET_WIFI_FORGET) {
        view.saved_update_failed = false;
        if (index >= 0) {
          request.station = saved.items[index];
          clear_memory(request.station.password, sizeof(request.station.password));
          pending_profiles = saved;
          pet_wifi_profiles_remove(&pending_profiles, (unsigned)index);
          const bool current = view.saved[index].current;
          if (commit_profiles(&saved.items[index]) == ESP_OK) {
            (void)set_auto_join(&request.station, true);
            if (current && started) {
              (void)esp_wifi_disconnect();
              wifi_config_t empty = {0};
              (void)esp_wifi_set_config(WIFI_IF_STA, &empty);
              view.connected[0] = 0;
            }
            view.join_stage = PET_WIFI_JOIN_NONE;
            view.join_reason = 0;
            view.status = view.connected[0] ? PET_WIFI_CONNECTED : PET_WIFI_IDLE;
            retry_index = 0;
            retry = xTaskGetTickCount();
          } else view.saved_update_failed = true;
        }
      } else if (request.operation == PET_WIFI_CONNECT && index < 0 && saved.count == PET_WIFI_SAVED_MAX) {
        view.status = PET_WIFI_LIST_FULL;
      } else if (!start_radio()) {
        view.status = PET_WIFI_UNAVAILABLE;
      } else if (request.operation == PET_WIFI_SCAN) {
        scan();
      } else if (request.operation == PET_WIFI_START) {
        /* A completed scan also proves a working C6 if version RPC is unavailable. */
        if (!pet_p4_is_wifi6_hardware()) scan();
      } else if (request.operation == PET_WIFI_DETAILS) {
        refresh_connection();
      } else if (request.operation == PET_WIFI_CONNECT ||
                 (request.operation == PET_WIFI_CONNECT_SAVED && index >= 0)) {
        if (request.operation == PET_WIFI_CONNECT_SAVED) request.station = saved.items[index];
        view.join_stage = PET_WIFI_JOIN_NONE;
        view.join_reason = 0;
        bool joined = connect_station(&request.station, &request.ap, true, false);
        if (!joined && !atomic_load(&cancelled)) {
          const unsigned reason = pet_device_wifi_link_status().manual_disconnect_reason;
          if (reason == WIFI_REASON_NO_AP_FOUND && request.ap.channel)
            joined = connect_station(&request.station, NULL, true, false);
          else if (handshake_timeout(reason))
            joined = connect_station(&request.station, &request.ap, true, true);
        }
        joined = joined && !atomic_load(&cancelled);
        retry_delay_ms = WIFI_RETRY_FIRST_MS;
        retry_index = 0;
        esp_err_t saved_result = ESP_OK;
        if (joined && (index != 0 || strcmp(saved.items[0].password, request.station.password))) {
          // A previously forgotten network starts with auto-join enabled.
          if (index < 0) saved_result = set_auto_join(&request.station, true);
          pending_profiles = saved;
          pet_wifi_profiles_promote(&pending_profiles, index < 0 ? saved.count : (unsigned)index,
                                    &request.station);
          const bool password_changed = index >= 0 &&
            strcmp(saved.items[index].password, request.station.password);
          if (saved_result == ESP_OK)
            saved_result = commit_profiles(password_changed ? &request.station : NULL);
          else clear_memory(&pending_profiles, sizeof(pending_profiles));
        }
        if (joined && saved_result == ESP_OK) view.status = PET_WIFI_CONNECTED;
        else {
          if (joined) note_join(PET_WIFI_JOIN_SAVE, saved_result, true);
          const pet_wifi_link_status_t link = pet_device_wifi_link_status();
          view.join_stage = link.join_stage;
          view.join_reason = link.disconnect_reason;
          (void)esp_wifi_disconnect();
          view.connected[0] = 0;
          view.status = atomic_load(&cancelled) ? PET_WIFI_IDLE : PET_WIFI_FAILED;
          retry = xTaskGetTickCount() + pdMS_TO_TICKS(retry_delay_ms);
        }
      }
      clear_memory(&request, sizeof(request));
      publish();
      atomic_store(&request_pending, false);
      continue;
    }
    if (started && view.connected[0] &&
        (xEventGroupClearBits(events, WIFI_DISCONNECTED | WIFI_LOST_IP) &
         (WIFI_DISCONNECTED | WIFI_LOST_IP))) {
      view.connected[0] = 0;
      view.status = PET_WIFI_IDLE;
      retry = xTaskGetTickCount();
      retry_delay_ms = WIFI_RETRY_FIRST_MS;
      publish();
    }
    const unsigned next_auto = pet_wifi_next_auto(&saved, &manual_only, retry_index);
    if (!pet_ble_shortcut_restore_requested()) attempted_ble_restore = false;
    else if (!started && next_auto >= saved.count && !attempted_ble_restore) {
      attempted_ble_restore = true;
      (void)start_radio();
    }
    if (next_auto < saved.count && !view.connected[0] && view.status != PET_WIFI_UNAVAILABLE &&
        (int32_t)(xTaskGetTickCount() - retry) >= 0) {
      if (!start_radio()) view.status = PET_WIFI_UNAVAILABLE;
      else {
        const pet_wifi_station_t *candidate = &saved.items[next_auto];
        bool restored = connect_station(candidate, NULL, false, false);
        if (!restored && !uxQueueMessagesWaiting(requests) &&
            handshake_timeout(pet_device_wifi_link_status().disconnect_reason))
          restored = connect_station(candidate, NULL, false, true);
        // Foreground work can interrupt a join. Resume this candidate without backoff.
        if (!restored && uxQueueMessagesWaiting(requests)) continue;
        if (restored) {
          retry_index = 0;
          view.status = PET_WIFI_CONNECTED;
          view.join_stage = PET_WIFI_JOIN_NONE;
          view.join_reason = 0;
          retry_delay_ms = WIFI_RETRY_FIRST_MS;
        } else {
          retry_index = pet_wifi_next_auto(&saved, &manual_only, next_auto + 1);
          if (retry_index < saved.count) {
            retry = xTaskGetTickCount();
            continue;
          }
          retry_index = 0;
          if (view.status == PET_WIFI_IDLE && !view.count) {
            const pet_wifi_link_status_t link = pet_device_wifi_link_status();
            view.join_stage = link.join_stage;
            view.join_reason = link.disconnect_reason;
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
  if (operation == PET_WIFI_CONNECT || operation == PET_WIFI_CONNECT_SAVED || operation == PET_WIFI_FORGET ||
      operation == PET_WIFI_AUTO_ON || operation == PET_WIFI_AUTO_OFF) {
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
