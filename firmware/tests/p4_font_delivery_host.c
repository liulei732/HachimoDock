/* Exercise the production storage transaction with an isolated filesystem. */
#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "pet_p4_shared_font_id.h"

#define ESP_OK 0
static int64_t clock_us = 1000;
static int64_t esp_timer_get_time(void) { return clock_us; }
static bool ready;
static bool pet_shared_font_ready(void) { return ready; }
static int esp_spiffs_info(const char *label, size_t *total, size_t *used) {
    assert(strcmp(label, "storage") == 0);
    *total = 2 * 1024 * 1024; *used = 0; return 0;
}
static const char *json_string(const cJSON *p, const char *key) {
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(p, key));
    return s ? s : "";
}
static uint64_t json_bounded_u64(const cJSON *p, const char *key, uint64_t max, uint64_t fallback) {
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(p, key);
    return cJSON_IsNumber(n) && n->valuedouble >= 0 && n->valuedouble <= max ? (uint64_t)n->valuedouble : fallback;
}
/* A known fixture chunk; base64 itself is supplied by mbedTLS on the board. */
static int mbedtls_base64_decode(uint8_t *out, size_t max, size_t *decoded, const uint8_t *s, size_t n) {
    if (max < 3 || n != 4 || memcmp(s, "YWJj", 4)) return -1;
    memcpy(out, "abc", 3); *decoded = 3; return 0;
}
static bool pet_shared_font_activate_staged(const char *path) {
    assert(!strcmp(path, "/spiffs/bubble32-v1.tmp"));
    ready = true;
    return true;
}
static const char *local_name(const char *p) {
    assert(strncmp(p, "/spiffs/", 8) == 0);
    return p + 8;
}
#include "pet_p4_storage_lane.h"
#include "pet_p4_font_delivery.h"
static pet_storage_lane_t lane;
static bool lane_locked = true, lan_locked = true, lose_route, short_write, revoke_lease;
static uint32_t generation = 7;
static uintptr_t worker(void *unused) { (void)unused; return 1; }
static void unlock(void *unused) { (void)unused; assert(lane_locked); lane_locked = false; }
static void relock(void *unused) { (void)unused; assert(!lane_locked); lane_locked = true; }
static bool storage_scope_begin(pet_storage_scope_t *scope, bool activation) {
    assert(!activation); return pet_storage_scope_begin(&lane, scope);
}
static void storage_scope_finish(pet_storage_scope_t *scope) {
    if (scope->active) assert(pet_storage_scope_finish(scope));
}
static bool storage_work_current(void) { return !revoke_lease; }
static void lan_unlock(void) { assert(lan_locked && lane_locked); lan_locked = false; }
static bool lan_resume(void *unused, uint32_t expected) {
    (void)unused; assert(!lan_locked && lane_locked); lan_locked = true;
    return expected == generation;
}
static bool checking_lan;
void pet_p4_font_delivery_expire(void);
static size_t test_write(const void *data, size_t unit, size_t size, FILE *file) {
    assert(!lane_locked && lane.occupied && (!checking_lan || !lan_locked));
    clock_us += 16000000;
    pet_p4_font_delivery_expire(); /* Main loop during a delayed physical write. */
    if (lose_route) generation++;
    return fwrite(data, unit, short_write ? size - 1 : size, file);
}
#define fwrite test_write
#include "font_stage.inc"
#undef fwrite
#define fopen(p, mode) fopen(local_name(p), mode)
#define remove(p) remove(local_name(p))
#include "pet_p4_font_delivery.inc"
#undef fopen
#undef remove

static void step(const char *phase, pet_font_route_t owner, bool admitted, unsigned offset, bool ok, unsigned received) {
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "phase", phase);
    cJSON_AddStringToObject(request, "resource", "bubble32-v1");
    cJSON_AddStringToObject(request, "transferId", "12345678123456781234567812345678");
    cJSON_AddStringToObject(request, "requestId", "request");
    cJSON_AddStringToObject(request, "data", "YWJj");
    cJSON_AddNumberToObject(request, "offset", offset);
    cJSON *reply = pet_p4_font_delivery(request, owner, "board", admitted);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(reply, "ok")) == ok);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(reply, "received")) == received);
    assert(strcmp(json_string(reply, "boardDeviceId"), "board") == 0);
    cJSON_Delete(reply);
    cJSON_Delete(request);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    int usb_owner = 1;
    pet_font_route_t usb = {.owner = &usb_owner};
    pet_font_route_t wifi = {.generation = generation, .unlock = lan_unlock, .resume = lan_resume};
    assert(pet_storage_lane_configure(&lane, unlock, relock, worker, NULL));
    assert(pet_storage_lane_enter(&lane));
    step("begin", usb, false, 0, false, 0);
    step("begin", usb, true, 0, true, 0);
    step("chunk", wifi, true, 0, false, 0);
    step("chunk", usb, true, 0, true, 3);
    step("chunk", usb, true, 0, false, 3);
    step("commit", usb, true, 3, false, 3);
    step("begin", wifi, true, 0, false, 3);
    clock_us += 16000000;
    step("begin", wifi, true, 0, true, 0);
    step("abort", usb, true, 0, false, 0);
    checking_lan = true;
    step("chunk", wifi, true, 0, true, 3);
    const char *id = "12345678123456781234567812345678";
    assert(!pet_p4_font_write(id, usb, 3, (const uint8_t *)"raw", 3));
    assert(!pet_p4_font_write(id, wifi, 0, (const uint8_t *)"raw", 3));
    assert(!pet_p4_font_write(id, wifi, 3, (const uint8_t *)"raw", 4097));
    assert(pet_p4_font_write(id, wifi, 3, (const uint8_t *)"raw", 3));
    step("abort", wifi, true, 0, true, 6);
    assert(!fopen("bubble32-v1.tmp", "rb"));
    for (int failure = 0; failure < 3; failure++) {
        wifi.generation = generation;
        step("begin", wifi, true, 0, true, 0);
        short_write = revoke_lease = lose_route = false;
        assert(pet_p4_font_write(id, wifi, 0, (const uint8_t *)"one", 3));
        short_write = failure == 0;
        revoke_lease = failure == 1;
        lose_route = failure == 2;
        assert(!pet_p4_font_write(id, wifi, 3, (const uint8_t *)"two", 3));
        assert(!font_upload.file && font_upload.received == 3 && !fopen("bubble32-v1.tmp", "rb"));
        assert(!lane.occupied && lan_locked && lane_locked);
    }
    /* Retry on the new generation, including the final short binary record. */
    lose_route = false;
    wifi.generation = generation;
    step("begin", wifi, true, 0, true, 0);
    FILE *font = fopen(argv[1], "rb");
    assert(font);
    uint8_t bytes[4096];
    size_t offset = 0, size;
    while ((size = fread(bytes, 1, sizeof(bytes), font)) != 0) {
        assert(pet_p4_font_write(id, wifi, offset, bytes, size));
        offset += size;
    }
    assert(!ferror(font) && offset == PET_SHARED_FONT_BYTES);
    fclose(font);
    step("commit", wifi, true, offset, true, offset);
    assert(pet_storage_lane_leave(&lane));
    assert(ready);
    step("status", usb, false, 0, true, offset);
    step("begin", wifi, true, 0, false, offset);
}
