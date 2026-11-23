#pragma once

#include "pet_p4_wifi_data_sources.h"

#if PET_P4_SYSTEM_WIFI
/* Wi-Fi data worker only. The ordered list binds a stock record to its watchlist. */
bool pet_wifi_data_cache_restore(const char *source, const pet_wifi_watchlist_t *list,
                                uint64_t now_ms, uint64_t revision);
bool pet_wifi_data_cache_good_feed(const cJSON *snapshot);
bool pet_wifi_data_cache_save(const char *source, const pet_wifi_watchlist_t *list,
                             const cJSON *snapshot);
/* Worker-owned RAM index. Rebuild lazily from committed records after boot. */
typedef struct {
  char source[sizeof(PET_WIFI_DATA_FEED_PREFIX) + 32];
  uint64_t reserved_ms;
} pet_wifi_cache_slot_t;
typedef struct {
  pet_wifi_cache_slot_t slots[2];
  uint64_t retry_after_ms;
  bool loaded;
} pet_wifi_feed_cache_t;
bool pet_wifi_data_cache_save_feed(pet_wifi_feed_cache_t *cache, const char *source,
                                  cJSON *snapshot, uint64_t now_ms, uint64_t utc_ms);
#endif
