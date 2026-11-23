#include "pet_p4_wifi_data_cache.h"

#if PET_P4_SYSTEM_WIFI
#include <string.h>

#include "esp_heap_caps.h"
#include "nvs.h"
#include "pet_p4_json_source.h"

#define CACHE_NAMESPACE "pet_wifi_cache"
#define STOCK_CACHE_LIMIT 6144
#define NEWS_CACHE_LIMIT 2048
#define FEED_CACHE_LIMIT 4096
#define FEED_INTERVAL_MS 900000ULL
/* The first key is also the legacy, untimestamped single-slot cache. */
static const char *const feed_keys[] = {"feed_v1", "feed2_v1"};
#define FEED_SLOTS (sizeof(feed_keys) / sizeof(*feed_keys))
_Static_assert(FEED_SLOTS == sizeof(((pet_wifi_feed_cache_t *)0)->slots) / sizeof(pet_wifi_cache_slot_t),
               "one persistent key per feed slot");

static bool feed_key(const char *source) {
  const size_t prefix = sizeof(PET_WIFI_DATA_FEED_PREFIX) - 1;
  return !strncmp(source, PET_WIFI_DATA_FEED_PREFIX, prefix) && strlen(source + prefix) == 32 &&
    strspn(source + prefix, "0123456789abcdef") == 32;
}

bool pet_wifi_data_cache_good_feed(const cJSON *snapshot) {
  const char *status = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(snapshot, "status"));
  const cJSON *rows = cJSON_GetObjectItemCaseSensitive(snapshot, "rows");
  return status && cJSON_IsArray(rows) && cJSON_GetArraySize(rows) <= PET_P4_DATA_MAX_ROWS &&
    (!strcmp(status, "ok") || (!strcmp(status, "empty") && !rows->child));
}

static const char *cache_key(const char *source, size_t *limit) {
  if (!strcmp(source, PET_WIFI_DATA_STOCKS)) {
    *limit = STOCK_CACHE_LIMIT;
    return "stocks_v1";
  }
  if (!strcmp(source, PET_WIFI_DATA_NEWS)) {
    *limit = NEWS_CACHE_LIMIT;
    return "news_v1";
  }
  return NULL;
}

static bool matches(const cJSON *snapshot, const char *source, const pet_wifi_watchlist_t *list) {
  const cJSON *name = cJSON_GetObjectItemCaseSensitive(snapshot, "source");
  const cJSON *status = cJSON_GetObjectItemCaseSensitive(snapshot, "status");
  const cJSON *rows = cJSON_GetObjectItemCaseSensitive(snapshot, "rows");
  if (!cJSON_IsString(name) || strcmp(name->valuestring, source) ||
      cJSON_HasObjectItem(snapshot, "clearOnExpire")) return false;
  if (feed_key(source)) return pet_wifi_data_cache_good_feed(snapshot);
  if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") ||
      !cJSON_IsArray(rows) || !rows->child) return false;
  if (strcmp(source, PET_WIFI_DATA_STOCKS)) return cJSON_GetArraySize(rows) <= PET_P4_DATA_PAGE_ROWS;
  if (!list || cJSON_GetArraySize(rows) != (int)list->count) return false;
  unsigned i = 0;
  const cJSON *row;
  cJSON_ArrayForEach(row, rows) {
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(row, "id");
    if (!cJSON_IsString(id) || strcmp(id->valuestring, list->symbols[i++])) return false;
  }
  return true;
}

/* Missing or malformed blobs have no usable snapshot. A transient read or
 * allocation failure must never make an occupied slot eligible for eviction. */
static bool read_snapshot(nvs_handle_t handle, const char *key, size_t limit, cJSON **snapshot) {
  *snapshot = NULL;
  size_t length = 0;
  esp_err_t result = nvs_get_blob(handle, key, NULL, &length);
  if (result != ESP_OK) return result == ESP_ERR_NVS_NOT_FOUND;
  if (length < 2 || length > limit) return true;
  const size_t capacity = length;
  char *raw = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!raw) return false;
  result = nvs_get_blob(handle, key, raw, &length);
  if (result == ESP_OK && length > 1 && length <= capacity && raw[length - 1] == 0 &&
      pet_json_source_text_limit_bytes(raw, length - 1, 3)) {
    *snapshot = cJSON_ParseWithLengthOpts(raw, length, NULL, true);
    /* A parse failure can also be OOM. Preserve the old record on ambiguity. */
    if (!*snapshot) { heap_caps_free(raw); return false; }
  }
  heap_caps_free(raw);
  return result == ESP_OK;
}

static bool restore_slot(nvs_handle_t handle, const char *key, size_t limit, const char *source,
                         const pet_wifi_watchlist_t *list, uint64_t now_ms, uint64_t revision) {
  cJSON *snapshot;
  if (!read_snapshot(handle, key, limit, &snapshot)) return false;
  const bool ok = snapshot && matches(snapshot, source, list) &&
    pet_p4_widget_data_restore_local(snapshot, now_ms, revision);
  cJSON_Delete(snapshot);
  return ok;
}

bool pet_wifi_data_cache_restore(const char *source, const pet_wifi_watchlist_t *list,
                                uint64_t now_ms, uint64_t revision) {
  size_t limit;
  const char *key = cache_key(source, &limit);
  const bool feed = feed_key(source);
  nvs_handle_t handle;
  if ((!key && !feed) || nvs_open(CACHE_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
  bool ok = key && restore_slot(handle, key, limit, source, list, now_ms, revision);
  for (size_t i = 0; feed && !ok && i < FEED_SLOTS; ++i)
    ok = restore_slot(handle, feed_keys[i], FEED_CACHE_LIMIT, source, NULL, now_ms, revision);
  nvs_close(handle);
  return ok;
}

static bool write_snapshot(nvs_handle_t handle, const char *key, size_t limit, const cJSON *snapshot) {
  char *raw = heap_caps_malloc(limit, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!raw) return false;
  const bool ok = cJSON_PrintPreallocated((cJSON *)snapshot, raw, (int)limit, false) &&
    nvs_set_blob(handle, key, raw, strlen(raw) + 1) == ESP_OK && nvs_commit(handle) == ESP_OK;
  heap_caps_free(raw);
  return ok;
}

bool pet_wifi_data_cache_save(const char *source, const pet_wifi_watchlist_t *list,
                             const cJSON *snapshot) {
  size_t limit;
  const char *key = cache_key(source, &limit);
  nvs_handle_t handle;
  if (!key || !matches(snapshot, source, list) ||
      nvs_open(CACHE_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
  const bool ok = write_snapshot(handle, key, limit, snapshot);
  nvs_close(handle);
  return ok;
}

static bool load_feed_index(pet_wifi_feed_cache_t *cache) {
  nvs_handle_t handle;
  /* READWRITE also opens a fresh namespace on first use. */
  if (nvs_open(CACHE_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
  for (size_t i = 0; i < FEED_SLOTS; ++i) {
    cJSON *record;
    if (!read_snapshot(handle, feed_keys[i], FEED_CACHE_LIMIT, &record)) {
      nvs_close(handle);
      return false;
    }
    pet_wifi_cache_slot_t *slot = &cache->slots[i];
    *slot = (pet_wifi_cache_slot_t){0};
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(record, "source"));
    if (name && feed_key(name)) {
      strcpy(slot->source, name);
      double stamp = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(record, "cacheSlotMs"));
      /* The legacy record has an identity but no reservation. */
      if (stamp > 0 && stamp <= 9007199254740991.0) slot->reserved_ms = (uint64_t)stamp;
    }
    cJSON_Delete(record);
  }
  nvs_close(handle);
  cache->loaded = true;
  return true;
}

/* Two initial checkpoints, then one token every 15 minutes across all public
 * sources. Each blob carries its reservation in the same NVS commit as its
 * payload, so reboot/failed writes need no separate index or wear counter. */
bool pet_wifi_data_cache_save_feed(pet_wifi_feed_cache_t *cache, const char *source,
                                  cJSON *snapshot, uint64_t now_ms, uint64_t utc_ms) {
  if (now_ms < cache->retry_after_ms || !feed_key(source) || !matches(snapshot, source, NULL) ||
      !utc_ms || cJSON_HasObjectItem(snapshot, "cacheSlotMs")) return false;
  if (!cache->loaded && !load_feed_index(cache)) goto retry;

  size_t target = 0, matching = FEED_SLOTS;
  uint64_t latest = 0;
  for (size_t i = 0; i < FEED_SLOTS; ++i) {
    const pet_wifi_cache_slot_t *slot = &cache->slots[i];
    if (!strcmp(slot->source, source)) matching = i;
    if (cache->slots[target].source[0] &&
        (!slot->source[0] || slot->reserved_ms < cache->slots[target].reserved_ms)) target = i;
    if (slot->reserved_ms > latest) latest = slot->reserved_ms;
  }
  if (matching != FEED_SLOTS) target = matching;
  pet_wifi_cache_slot_t *slot = &cache->slots[target];
  /* Do not evict a source still on cooldown: quick rotation cannot reset it. */
  if ((slot->reserved_ms && utc_ms < slot->reserved_ms + FEED_INTERVAL_MS) ||
      latest > utc_ms) return false;
  uint64_t reservation = latest && latest + FEED_INTERVAL_MS > utc_ms
    ? latest + FEED_INTERVAL_MS : utc_ms;
  nvs_handle_t handle;
  if (nvs_open(CACHE_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) goto retry;
  bool saved = false;
  if (cJSON_AddNumberToObject(snapshot, "cacheSlotMs", (double)reservation)) {
    saved = write_snapshot(handle, feed_keys[target], FEED_CACHE_LIMIT, snapshot);
    cJSON_DeleteItemFromObjectCaseSensitive(snapshot, "cacheSlotMs");
  }
  nvs_close(handle);
  if (!saved) goto retry;
  strcpy(slot->source, source);
  slot->reserved_ms = reservation;
  return true;
retry:
  cache->loaded = false; /* A failed commit may have reached flash. */
  cache->retry_after_ms = now_ms + 30000;
  return false;
}
#endif
