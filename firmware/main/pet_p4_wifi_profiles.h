#pragma once

#include <string.h>
#include "pet_p4_device_wifi.h"

/* The station record is also the on-flash station_v1 layout. */
typedef struct {
  char ssid[PET_WIFI_SSID_SIZE];
  char password[PET_WIFI_PASSWORD_SIZE];
  uint8_t security;
} pet_wifi_station_t;

typedef struct {
  uint8_t count;
  pet_wifi_station_t items[PET_WIFI_SAVED_MAX];
} pet_wifi_profiles_t;

/* Separate from the credential blob so older firmware can still read the
 * saved network list. An entry means this exact network is manual-only. */
typedef struct {
  char ssid[PET_WIFI_SSID_SIZE];
  uint8_t security;
} pet_wifi_network_id_t;

typedef struct {
  uint8_t count;
  pet_wifi_network_id_t items[PET_WIFI_SAVED_MAX];
} pet_wifi_manual_only_t;

static inline bool pet_wifi_same_network(const pet_wifi_network_id_t *item,
                                         const char *ssid, uint8_t security) {
  return item->security == security && !strcmp(item->ssid, ssid);
}

static inline bool pet_wifi_auto_allowed(const pet_wifi_manual_only_t *policy,
                                         const pet_wifi_station_t *station) {
  for (unsigned i = 0; i < policy->count; ++i)
    if (pet_wifi_same_network(&policy->items[i], station->ssid, station->security)) return false;
  return true;
}

static inline unsigned pet_wifi_next_auto(const pet_wifi_profiles_t *profiles,
                                          const pet_wifi_manual_only_t *policy, unsigned start) {
  while (start < profiles->count && !pet_wifi_auto_allowed(policy, &profiles->items[start])) ++start;
  return start;
}

static inline bool pet_wifi_station_valid(const pet_wifi_station_t *station) {
  if (!station->ssid[0] || !memchr(station->ssid, 0, sizeof(station->ssid)) ||
      !memchr(station->password, 0, sizeof(station->password)) || station->security > PET_WIFI_WPA3)
    return false;
  const size_t n = strlen(station->password);
  return station->security == PET_WIFI_OPEN ? n == 0 : n >= 8 && n <= 63;
}

static inline int pet_wifi_profiles_find(const pet_wifi_profiles_t *profiles,
                                        const char *ssid, uint8_t security) {
  for (unsigned i = 0; i < profiles->count; ++i)
    if (profiles->items[i].security == security && !strcmp(profiles->items[i].ssid, ssid))
      return (int)i;
  return -1;
}

static inline bool pet_wifi_manual_only_valid(const pet_wifi_manual_only_t *policy) {
  if (policy->count > PET_WIFI_SAVED_MAX) return false;
  for (unsigned i = 0; i < policy->count; ++i) {
    const pet_wifi_network_id_t *item = &policy->items[i];
    if (!item->ssid[0] || !memchr(item->ssid, 0, sizeof(item->ssid)) || item->security > PET_WIFI_WPA3)
      return false;
    for (unsigned j = 0; j < i; ++j)
      if (pet_wifi_same_network(&policy->items[j], item->ssid, item->security)) return false;
  }
  return true;
}

static inline bool pet_wifi_profiles_valid(const pet_wifi_profiles_t *profiles) {
  if (profiles->count > PET_WIFI_SAVED_MAX) return false;
  for (unsigned i = 0; i < profiles->count; ++i) {
    const pet_wifi_station_t *item = &profiles->items[i];
    if (!pet_wifi_station_valid(item) || pet_wifi_profiles_find(profiles, item->ssid, item->security) != (int)i)
      return false;
  }
  return true;
}

static inline void pet_wifi_profiles_promote(pet_wifi_profiles_t *profiles, unsigned index,
                                             const pet_wifi_station_t *station) {
  if (index == profiles->count) ++profiles->count;
  memmove(&profiles->items[1], &profiles->items[0], index * sizeof(*station));
  profiles->items[0] = *station;
}

static inline void pet_wifi_profiles_remove(pet_wifi_profiles_t *profiles, unsigned index) {
  --profiles->count;
  memmove(&profiles->items[index], &profiles->items[index + 1],
          (profiles->count - index) * sizeof(profiles->items[0]));
  memset(&profiles->items[profiles->count], 0, sizeof(profiles->items[0]));
}
