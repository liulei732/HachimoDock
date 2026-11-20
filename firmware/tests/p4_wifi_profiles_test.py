"""Exercise the production credential migration and commits against synthetic NVS."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_wifi_profile_migration_reordering_and_forgetting(tmp_path):
    radio = (ROOT / "main/pet_p4_device_wifi.c").read_text()
    operations = radio[radio.index("static esp_err_t write_profiles("):radio.index("static void wifi_event(")]
    source = tmp_path / "wifi_profiles.c"
    binary = tmp_path / "wifi_profiles"
    source.write_text(r'''
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include "pet_p4_wifi_profiles.h"
typedef int nvs_handle_t;
typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_NVS_NOT_FOUND, ESP_FAIL, ESP_ERR_INVALID_STATE, NVS_READWRITE, NVS_READONLY };
#define WIFI_NAMESPACE "pet_wifi_local"
#define WIFI_LEGACY_KEY "station_v1"
#define WIFI_PROFILES_KEY "stations_v2"
#define WIFI_MANUAL_ONLY_KEY "manual_only_v1"
static pet_wifi_station_t old = {.ssid="first-network", .password="fixture-1", .security=PET_WIFI_PERSONAL};
static pet_wifi_profiles_t stored, saved, pending_profiles;
static pet_wifi_manual_only_t stored_policy, manual_only;
static pet_wifi_snapshot_t view;
static bool have_policy;
static pet_wifi_saved_network_t legacy_network;
static bool have_old = true, have_profiles, profiles_on_flash, fail_write;
static void clear_memory(void *memory, size_t size) { memset(memory, 0, size); }
static int nvs_open(const char *ns, int mode, nvs_handle_t *out) {
  assert(!strcmp(ns,WIFI_NAMESPACE)); (void)mode; *out = 1; return ESP_OK;
}
static int nvs_set_blob(int nvs, const char *key, const void *data, size_t size) {
  (void)nvs;
  if (fail_write) return ESP_FAIL;
  if (!strcmp(key, WIFI_MANUAL_ONLY_KEY)) {
    assert(size == sizeof(stored_policy));
    memcpy(&stored_policy, data, size);
    have_policy = true;
  } else {
    assert(!strcmp(key, WIFI_PROFILES_KEY) && size == sizeof(stored));
    memcpy(&stored, data, size);
    have_profiles = true;
  }
  return ESP_OK;
}
static int nvs_get_blob(int nvs, const char *key, void *data, size_t *size) {
  (void)nvs;
  const void *record;
  if (!strcmp(key, WIFI_MANUAL_ONLY_KEY)) {
    if (!have_policy) return ESP_ERR_NVS_NOT_FOUND;
    assert(*size == sizeof(stored_policy));
    record = &stored_policy;
  } else if (!strcmp(key, WIFI_LEGACY_KEY)) {
    if (!have_old) return ESP_ERR_NVS_NOT_FOUND;
    assert(*size == sizeof(old));
    record = &old;
  } else {
    assert(!strcmp(key, WIFI_PROFILES_KEY));
    if (!have_profiles) return ESP_ERR_NVS_NOT_FOUND;
    assert(*size == sizeof(stored));
    record = &stored;
  }
  memcpy(data, record, *size);
  return ESP_OK;
}
static int nvs_erase_key(int nvs, const char *key) {
  (void)nvs; assert(!strcmp(key,WIFI_LEGACY_KEY));
  const bool existed = have_old; have_old = false; return existed ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}
static int nvs_commit(int nvs) { (void)nvs; return ESP_OK; }
static void nvs_close(int nvs) { (void)nvs; }
''' + operations + r'''
int main(void) {
  load_profiles();
  assert(saved.count == 1 && !strcmp(saved.items[0].ssid,"first-network") && !profiles_on_flash);
  pet_wifi_station_t next = {.ssid="next-network", .password="fixture-2", .security=PET_WIFI_WPA3};
  pending_profiles = saved;
  pet_wifi_profiles_promote(&pending_profiles, 1, &next);
  fail_write = true;
  assert(commit_profiles(NULL) == ESP_FAIL && saved.count == 1 && have_old && !have_profiles);
  fail_write = false;
  pending_profiles = saved;
  pet_wifi_profiles_promote(&pending_profiles, 1, &next);
  assert(commit_profiles(NULL) == ESP_OK && have_old && saved.count == 2 && have_profiles);
  saved = (pet_wifi_profiles_t){0};
  load_profiles();
  assert(saved.count == 2 && !strcmp(saved.items[0].ssid,"next-network"));
  assert(pet_wifi_profiles_find(&saved,"next-network",PET_WIFI_OPEN) < 0);
  assert(pet_wifi_next_auto(&saved,&manual_only,0) == 0);
  fail_write = true;
  assert(set_auto_join(&saved.items[0], false) == ESP_FAIL && !have_policy);
  assert(pet_wifi_next_auto(&saved,&manual_only,0) == 0);
  fail_write = false;
  assert(set_auto_join(&saved.items[0], false) == ESP_OK && have_policy);
  load_profiles();
  assert(pet_wifi_next_auto(&saved,&manual_only,0) == 1);
  next = saved.items[1];
  pending_profiles = saved;
  pet_wifi_profiles_promote(&pending_profiles,1,&next);
  assert(commit_profiles(NULL) == ESP_OK);
  load_profiles();
  assert(pet_wifi_next_auto(&saved,&manual_only,0) == 0);
  assert(pet_wifi_next_auto(&saved,&manual_only,1) == saved.count);
  assert(set_auto_join(&saved.items[0], false) == ESP_OK); // All disabled.
  assert(pet_wifi_next_auto(&saved,&manual_only,0) == saved.count);
  next = saved.items[1];
  assert(set_auto_join(&next, true) == ESP_OK);
  pending_profiles = saved;
  pet_wifi_profiles_promote(&pending_profiles,1,&next);
  assert(commit_profiles(NULL) == ESP_OK);
  assert(pet_wifi_next_auto(&saved,&manual_only,0) == 0);
  const int index = pet_wifi_profiles_find(&saved,"first-network",PET_WIFI_PERSONAL);
  assert(index == 1);
  pending_profiles = saved;
  pet_wifi_profiles_remove(&pending_profiles, (unsigned)index);
  next = saved.items[index];
  assert(commit_profiles(&saved.items[index]) == ESP_OK && !have_old);
  assert(set_auto_join(&next, true) == ESP_OK && manual_only.count == 0);
  saved = (pet_wifi_profiles_t){0};
  load_profiles();
  assert(saved.count == 1 && !strcmp(saved.items[0].ssid,"next-network"));
  assert(saved.items[1].password[0] == 0);
  stored_policy.count = PET_WIFI_SAVED_MAX + 1;
  load_profiles();
  assert(view.saved_update_failed && pet_wifi_next_auto(&saved,&manual_only,0) == 1);
  assert(set_auto_join(&saved.items[0], true) == ESP_OK);
  load_profiles();
  assert(pet_wifi_next_auto(&saved,&manual_only,0) == 0);
  pending_profiles = saved;
  pet_wifi_profiles_remove(&pending_profiles, 0);
  assert(commit_profiles(&saved.items[0]) == ESP_OK);
  load_profiles();
  assert(profiles_on_flash && saved.count == 0);
  for (unsigned i = 0; i < PET_WIFI_SAVED_MAX; ++i) {
    snprintf(next.ssid, sizeof(next.ssid), "network-%u", i);
    pet_wifi_profiles_promote(&saved, saved.count, &next);
  }
  assert(pet_wifi_profiles_valid(&saved) && saved.count == PET_WIFI_SAVED_MAX);
  next = saved.items[PET_WIFI_SAVED_MAX - 1];
  pet_wifi_profiles_promote(&saved, PET_WIFI_SAVED_MAX - 1, &next);
  assert(pet_wifi_profiles_valid(&saved) && !strcmp(saved.items[0].ssid, "network-0"));
  ++saved.count;
  assert(!pet_wifi_profiles_valid(&saved));
}
''')
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-I", str(ROOT / "main"), str(source), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
