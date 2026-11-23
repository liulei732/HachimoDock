#pragma once

#include <string.h>

#include "pet_p4_device_wifi.h"

/* Keep the exact station_v1 layout for migration and firmware downgrade. */
typedef struct {
  char ssid[PET_WIFI_SSID_SIZE];
  char password[PET_WIFI_PASSWORD_SIZE];
  uint8_t security;
} pet_wifi_station_t;

typedef struct {
  pet_wifi_station_t station;
  uint8_t auto_join;
  uint8_t legacy_kept; /* The old firmware still has this migrated network. */
} pet_wifi_profile_t;

/* The NVS key versions this byte-only layout. Order is most recent manual join first. */
typedef struct {
  uint8_t count;
  pet_wifi_profile_t items[PET_WIFI_SAVED_MAX];
} pet_wifi_profiles_t;

static inline bool pet_wifi_same_network(const pet_wifi_station_t *station,
                                         const char *ssid, uint8_t security) {
  return station->security == security && !strcmp(station->ssid, ssid);
}

static inline int pet_wifi_profiles_find(const pet_wifi_profiles_t *profiles,
                                         const char *ssid, uint8_t security) {
  for (unsigned i = 0; i < profiles->count; ++i)
    if (pet_wifi_same_network(&profiles->items[i].station, ssid, security)) return (int)i;
  return -1;
}

static inline bool pet_wifi_station_valid(const pet_wifi_station_t *station) {
  if (!station->ssid[0] || !memchr(station->ssid, 0, sizeof(station->ssid)) ||
      !memchr(station->password, 0, sizeof(station->password)) || station->security > PET_WIFI_WPA3)
    return false;
  const size_t length = strlen(station->password);
  return station->security == PET_WIFI_OPEN ? length == 0 :
    length >= 8 && length <= PET_WIFI_PASSWORD_SIZE - 2;
}

static inline bool pet_wifi_profiles_valid(const pet_wifi_profiles_t *profiles) {
  if (profiles->count > PET_WIFI_SAVED_MAX) return false;
  unsigned legacy_count = 0;
  for (unsigned i = 0; i < profiles->count; ++i) {
    const pet_wifi_profile_t *profile = &profiles->items[i];
    if (profile->auto_join > 1 || !pet_wifi_station_valid(&profile->station)) return false;
    legacy_count += profile->legacy_kept;
    if (legacy_count > 1 || (profile->legacy_kept && !profile->auto_join)) return false;
    for (unsigned j = 0; j < i; ++j)
      if (pet_wifi_same_network(&profiles->items[j].station, profile->station.ssid,
                                profile->station.security)) return false;
  }
  return true;
}

static inline unsigned pet_wifi_next_auto(const pet_wifi_profiles_t *profiles, unsigned start) {
  while (start < profiles->count && !profiles->items[start].auto_join) ++start;
  return start;
}

static inline void pet_wifi_profiles_remove(pet_wifi_profiles_t *profiles, unsigned index) {
  --profiles->count;
  memmove(&profiles->items[index], &profiles->items[index + 1],
          (profiles->count - index) * sizeof(profiles->items[0]));
  memset(&profiles->items[profiles->count], 0, sizeof(profiles->items[0]));
}
