/* Included in the real credential-vault host harness. */
#include <time.h>
#include <sys/types.h>
#include "pet_p4_mqtt_feed.h"
#include "pet_p4_mqtt_wire.h"
typedef int esp_tls_t;
typedef struct { bool non_block; int timeout_ms; void (*crt_bundle_attach)(void); } esp_tls_cfg_t;
#define ESP_TLS_ERR_SSL_WANT_READ -2
#define ESP_TLS_ERR_SSL_WANT_WRITE -3
#define pdMS_TO_TICKS(n) (n)
static int socket_stub;
static bool fail_commit, revoke_after_fragment;
static bool online = true, clock_valid = true, revoked_at_connect, swap_at_write;
static unsigned connections, closes;
static int64_t tick;
static uint8_t output[1024];
static size_t output_len, response_len, response_pos;
static const uint8_t *response;
static pet_private_feed_t active;
static pet_wifi_endpoint_t address;
static int64_t esp_timer_get_time(void) { return ++tick * 1000; }
static void vTaskDelay(unsigned ms) {
  tick += ms;
}
static unsigned esp_random(void) { return 1; }
static void esp_crt_bundle_attach(void) {}
static bool pet_device_wifi_online(void) { return online; }
static bool pet_wifi_clock_read(uint64_t now, uint64_t *utc, int32_t *offset, uint64_t *age) {
  (void)now; *utc = 1704067200000ULL; *offset = 0; *age = 0; return clock_valid;
}
static bool pet_p4_miniapp_private_feed(pet_private_feed_t *binding, pet_wifi_endpoint_t *ep) {
  *binding = active; *ep = address; return true;
}
static int pet_p4_miniapp_write_if_active(const uint8_t scope[32], int (*send)(void *), void *ctx) {
  if (swap_at_write || memcmp(active.id, scope, 32)) return -1;
  int result = send(ctx);
  if (revoke_after_fragment && output_len && result > 0) {
    revoke_after_fragment = false;
    assert(pet_private_secret_schedule(&active, &(pet_private_secret_t){0}, true));
  }
  return result;
}
static esp_tls_t *pet_live_tls_connect_public_port(const char *host, uint16_t port,
    const esp_tls_cfg_t *cfg, int64_t deadline, pet_mqtt_feed_current_fn current, void *ctx) {
  assert(port == active.port && current(ctx));
  assert(!strcmp(host, address.host) && cfg->crt_bundle_attach);
  assert(cfg->non_block && deadline > tick);
  ++connections;
  if (revoked_at_connect) assert(pet_private_secret_schedule(&active, &(pet_private_secret_t){0}, true));
  return &socket_stub;
}
static ssize_t esp_tls_conn_write(esp_tls_t *s, const void *bytes, size_t n) {
  assert(s == &socket_stub && n && output_len + n < sizeof(output));
  size_t copied = n < 5 ? n : 5;
  memcpy(output + output_len, bytes, copied); output_len += copied; return copied;
}
static ssize_t esp_tls_conn_read(esp_tls_t *s, void *bytes, size_t n) {
  assert(s == &socket_stub && n);
  if (response_pos == response_len) return 0;
  *(uint8_t *)bytes = response[response_pos++]; return 1;
}
static void esp_tls_conn_destroy(esp_tls_t *s) { assert(s == &socket_stub); ++closes; }

#include "mqtt_transfer.inc"
#include "pet_p4_device_write.inc"

static void reply(const void *bytes, size_t n) {
  response = bytes; response_len = n; response_pos = 0;
  output_len = 0; memset(output, 0, sizeof(output));
}
static void activate(const char *id, const char *url, const char *json) {
  cJSON *conn = cJSON_Parse(json);
  assert(pet_private_feed_parse(&active, &address, id, url, conn));
  cJSON_Delete(conn);
}
static void exercise_scoped_writes(void) {
  const char *post = "{\"type\":\"https-submit\",\"value\":\"arrived\"}";
  const char *url = "https://checkin.example.org/visits";
  const char *mqtt = "{\"type\":\"mqtt-publish\",\"topic\":\"room/light\",\"value\":\"on\"}";
  const char *broker = "mqtts://desk.example.org";
  const char *ok = "HTTP/1.1 202 Accepted\r\n";
  const pet_private_secret_t bearer = {.token="fixtureBearer"};
  const pet_private_secret_t login = {.user="fixture",.token="test-password"};
  pet_private_secret_t secret;
  activate("class-checkin", url, post);
  const pet_private_feed_t attendance = active;
  assert(pet_private_secret_schedule(&attendance, &bearer, false));
  activate("unrelated-desk", url, post);
  assert(!pet_device_write_start(&attendance));
  assert(pet_device_write_start(&active) && pet_device_write_result() == PET_PRIVATE_ABSENT);
  activate("class-checkin", url, "{\"type\":\"https-submit\",\"value\":\"left\"}");
  assert(pet_private_secret_read(&active, &secret) == PET_PRIVATE_ABSENT);
  activate("class-checkin", url, post);
  assert(!connections && !output_len);
  online = false; assert(pet_device_write_start(&active) && !connections); online = true;
  clock_valid = false; assert(pet_device_write_start(&active) && !connections); clock_valid = true;
  reply(ok, strlen(ok)); revoked_at_connect = true;
  assert(pet_device_write_start(&active) && !output_len && pet_device_write_result() == PET_PRIVATE_REFUSED);
  revoked_at_connect = false;
  assert(pet_private_secret_schedule(&active, &bearer, false));
  assert(pet_device_write_start(&active) && pet_device_write_result() == PET_WRITE_ACCEPTED);
  assert(!strcmp((char *)output, "POST /visits HTTP/1.1\r\nHost: checkin.example.org\r\n"
    "Authorization: Bearer fixtureBearer\r\nContent-Type: text/plain\r\nContent-Length: 7\r\n"
    "Connection: close\r\n\r\narrived"));
  assert(pet_private_secret_schedule(&active, &bearer, false));
  reply(ok, strlen(ok)); revoke_after_fragment = true;
  assert(pet_device_write_start(&active) && output_len == 5 && pet_device_write_result() == PET_WRITE_UNKNOWN);
  assert(pet_private_secret_read(&active, &secret) == PET_PRIVATE_ABSENT);
  assert(pet_private_secret_schedule(&active, &bearer, false));
  reply(ok, strlen(ok)); fail_commit = true;
  assert(pet_device_write_start(&active) && !output_len && pet_device_write_result() == PET_PRIVATE_STORAGE_ERROR);
  fail_commit = false;
  assert(pet_private_secret_schedule(&active, &bearer, true));
  assert(pet_private_secret_schedule(&active, &bearer, false));
  assert(pet_device_write_start(&active) && pet_device_write_result() == PET_WRITE_ACCEPTED);
  unsigned before = connections;
  assert(pet_device_write_start(&active) && before == connections && pet_device_write_result() == PET_WRITE_SPENT);
  assert(pet_private_secret_schedule(&active, &bearer, false));
  const char *redirect = "HTTP/1.1 307 Temporary Redirect\r\n";
  reply(redirect, strlen(redirect));
  assert(pet_device_write_start(&active) && pet_device_write_result() == PET_WRITE_UNKNOWN);
  pet_private_storage_init(); /* Reopen persistent spent record after reset. */
  assert(pet_private_secret_read(&active, &secret) == PET_WRITE_UNKNOWN && !secret.token[0]);
  assert(pet_private_secret_schedule(&active, &bearer, false) && pet_private_local_result() == PET_WRITE_UNKNOWN);
  before = connections;
  assert(pet_device_write_start(&active) && before == connections);
  assert(pet_private_secret_schedule(&active, &bearer, true));
  assert(pet_private_secret_schedule(&active, &bearer, false));
  reply(ok, strlen(ok)); swap_at_write = true;
  assert(pet_device_write_start(&active) && !output_len && pet_device_write_result() == PET_PRIVATE_REFUSED);
  assert(pet_private_secret_read(&active, &secret) == PET_PRIVATE_SET);
  swap_at_write = false;

  activate("desk-light", broker, mqtt);
  assert(!pet_private_secret_schedule(&active, &bearer, false));
  assert(pet_private_secret_schedule(&active, &login, false));
  const uint8_t ack[] = {0x20,2,0,0,0x40,2,0,1};
  reply(ack, sizeof(ack));
  assert(pet_device_write_start(&active) && pet_device_write_result() == PET_WRITE_ACCEPTED);
  size_t connect_end = output[1] + 2u;
  const uint8_t publish[] = {0x32,16,0,10,'r','o','o','m','/','l','i','g','h','t',0,1,'o','n'};
  assert(output[0] == 0x10 && output[9] == 0xc2 && output_len == connect_end + sizeof(publish));
  assert(!memcmp(output + connect_end, publish, sizeof(publish)));
  assert(pet_private_secret_schedule(&active, &login, false));
  uint8_t wrong_ack[] = {0x20,2,0,0,0x40,2,0,2};
  reply(wrong_ack, sizeof(wrong_ack));
  assert(pet_device_write_start(&active) && pet_device_write_result() == PET_WRITE_UNKNOWN);
  assert(pet_private_secret_schedule(&active, &login, true));
  assert(pet_private_secret_schedule(&active, &login, false));
  uint8_t refused[] = {0x20,2,0,5};
  reply(refused, sizeof(refused));
  assert(pet_device_write_start(&active) && pet_device_write_result() == PET_PRIVATE_REFUSED);
  assert(output_len == output[1]+2u && pet_private_secret_read(&active, &secret) == PET_PRIVATE_SET);
  uint32_t old_epoch = pet_private_storage_epoch();
  assert(pet_private_retire_component("class-checkin"));
  assert(pet_private_secret_read(&attendance, &secret) == PET_PRIVATE_ABSENT);
  assert(pet_private_secret_read(&active, &secret) == PET_PRIVATE_SET);
  assert(!pet_private_secret_schedule_at(&attendance, &bearer, false, old_epoch));
  cJSON *retired = cJSON_Parse("{\"type\":\"github-account\"}");
  assert(!pet_private_feed_parse(&active, &address, "github-card", "https://api.github.com/user", retired));
  cJSON_Delete(retired);
  activate("private-feed", "https://api.example.org/list", "{\"type\":\"bearer\"}");
  assert(!pet_device_write_start(&active));
  assert(pet_private_secret_schedule(&active, &bearer, false));
  assert(pet_private_secret_read(&active, &secret) == PET_PRIVATE_SET);
  assert(!strcmp(secret.token, bearer.token));
  assert(connections == closes);
}
