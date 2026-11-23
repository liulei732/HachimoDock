/* Both transports call this handler on the device state lane. Only a fixed,
 * digest-pinned system resource can be published. The state lane expires abandoned writers. */
#include "pet_p4_shared_font_id.h"
#include "pet_p4_font_delivery.h"
#include "pet_p4_fs_activity.h"
#define FONT_STAGE "/spiffs/bubble32-v1.tmp"
static struct {
  char transfer[33];
  pet_font_route_t route;
  bool writing;
  FILE *file;
  uint8_t *scratch;
  size_t received;
  int64_t last_us;
} font_upload;

static void font_upload_close(void) {
  if (font_upload.file) fclose(font_upload.file);
  font_upload.file = NULL;
  free(font_upload.scratch);
  font_upload.scratch = NULL;
  font_upload.transfer[0] = 0;
}

void pet_p4_font_delivery_expire(void) {
  if (!font_upload.writing && font_upload.file && esp_timer_get_time() - font_upload.last_us >= 15000000) {
    PET_FS_ACTIVITY_SCOPE(expire_io);
    font_upload_close();
    remove(FONT_STAGE);
  }
}

static bool font_route_matches(pet_font_route_t route) {
  return route.owner == font_upload.route.owner && route.generation == font_upload.route.generation &&
    route.resume == font_upload.route.resume;
}

bool pet_p4_font_write(const char *id, pet_font_route_t route, size_t offset,
                       const uint8_t *bytes, size_t size) {
  pet_p4_font_delivery_expire();
  if (font_upload.writing || !font_upload.file || !font_route_matches(route) ||
      memcmp(id, font_upload.transfer, 32) || offset != font_upload.received ||
      !size || size > 4096 || size > PET_SHARED_FONT_BYTES - offset) return false;
  /* Pin file, decoded scratch, offset and transfer until both locks return.
   * Abandon the entire stage on a partial write or retired generation. */
  PET_FS_ACTIVITY_SCOPE(chunk_io);
  font_upload.writing = true;
  if (route.unlock) route.unlock();
  const bool written = pet_p4_miniapp_stage_font_chunk(font_upload.file, bytes, size);
  const bool current = !route.resume || route.resume(route.owner, route.generation);
  font_upload.writing = false;
  if (!current || !written) {
    font_upload_close();
    remove(FONT_STAGE);
    return false;
  }
  font_upload.received += size;
  font_upload.last_us = esp_timer_get_time();
  return true;
}

#ifdef ESP_PLATFORM
static atomic_bool font_loading = true;
void pet_p4_font_boot_done(void) { atomic_store(&font_loading, false); }

static void font_load_worker(void *unused) {
  (void)unused;
  {
    /* Finish observation before the task deletes itself. */
    PET_FS_ACTIVITY_SCOPE(verify_io);
    if (!pet_shared_font_activate_staged(FONT_STAGE)) remove(FONT_STAGE);
  }
  atomic_store(&font_loading, false);
  vTaskDelete(NULL);
}
#endif

cJSON *pet_p4_font_delivery(const cJSON *payload, pet_font_route_t route,
                           const char *board, bool admitted) {
  cJSON *reply = cJSON_CreateObject();
  if (!reply) return NULL;
  const char *phase = json_string(payload, "phase");
  const char *id = json_string(payload, "transferId");
  bool ok = false;
  pet_p4_font_delivery_expire();
  bool same = *id && !strcmp(font_upload.transfer, id) && font_route_matches(route);
  if (strcmp(json_string(payload, "resource"), "bubble32-v1")) goto done;
  if (!strcmp(phase, "status")) {
    ok = true;
  } else if (font_upload.writing) {
    goto done;
  } else if (!strcmp(phase, "abort") && same) {
    PET_FS_ACTIVITY_SCOPE(abort_io);
    font_upload_close();
    remove(FONT_STAGE);
    ok = true;
  } else if (admitted && !pet_shared_font_ready() && strlen(id) == 32) {
#ifdef ESP_PLATFORM
    if (atomic_load(&font_loading)) goto done;
#endif
    if (!strcmp(phase, "begin")) {
      if (font_upload.file) goto done;
      PET_FS_ACTIVITY_SCOPE(begin_io);
      remove(FONT_STAGE);
      size_t total, used;
      if (esp_spiffs_info("storage", &total, &used) != ESP_OK || used > total ||
          total - used < PET_SHARED_FONT_BYTES + 256 * 1024) goto done;
      FILE *file = fopen(FONT_STAGE, "wb");
      if (!file) goto done;
      font_upload.file = file;
      memcpy(font_upload.transfer, id, 33);
      font_upload.route = route;
      font_upload.received = 0;
      ok = true;
    } else if (same && !strcmp(phase, "chunk")) {
      const char *data = json_string(payload, "data");
      size_t len = strlen(data), decoded = 0;
      if (json_bounded_u64(payload, "offset", PET_SHARED_FONT_BYTES, UINT64_MAX) != font_upload.received ||
          len == 0 || len > 4096) goto done;
      if (!font_upload.scratch) font_upload.scratch = malloc(3072);
      uint8_t *bytes = font_upload.scratch;
      if (bytes && !mbedtls_base64_decode(bytes, 3072, &decoded, (const uint8_t *)data, len)
          && decoded && decoded <= PET_SHARED_FONT_BYTES - font_upload.received) {
        ok = pet_p4_font_write(id, route, font_upload.received, bytes, decoded);
      }
    } else if (same && !strcmp(phase, "commit") && font_upload.received == PET_SHARED_FONT_BYTES) {
      PET_FS_ACTIVITY_SCOPE(commit_io);
      const bool flushed = font_upload.file && fclose(font_upload.file) == 0;
      font_upload.file = NULL;
      font_upload_close();
      if (!flushed) goto done;
#ifdef ESP_PLATFORM
      if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(payload, "asyncCommit"))) {
        atomic_store(&font_loading, true);
        ok = xTaskCreate(font_load_worker, "font-verify", 6144, NULL, tskIDLE_PRIORITY+1, NULL) == pdPASS;
        if (!ok) atomic_store(&font_loading, false);
      } else
#endif
      {
        ok = pet_shared_font_activate_staged(FONT_STAGE);
      }
      if (!ok) remove(FONT_STAGE);
      font_upload.transfer[0] = 0;
    }
  }
  if (ok && (same || !strcmp(phase, "begin"))) font_upload.last_us = esp_timer_get_time();
done:
#ifdef ESP_PLATFORM
  cJSON_AddBoolToObject(reply, "loading", atomic_load(&font_loading));
  cJSON_AddBoolToObject(reply, "asyncCommit", true);
#if PET_P4_SYSTEM_WIFI
  cJSON_AddBoolToObject(reply, "fontStream", true);
#endif
#endif
  cJSON_AddBoolToObject(reply, "ok", ok);
  cJSON_AddBoolToObject(reply, "loaded", pet_shared_font_ready());
  cJSON_AddNumberToObject(reply, "received", font_upload.received);
  cJSON_AddStringToObject(reply, "boardDeviceId", board);
  cJSON_AddStringToObject(reply, "requestId", json_string(payload, "requestId"));
  return reply;
}
