/* Real backend control flow with deterministic transport/storage fakes.
 * Does not claim ESP TLS timing, hardware storage durability or live broker proof. */
#define PET_P4_COMPONENT_NETWORK 1
#define PET_P4_SYSTEM_WIFI 1
#include "component_network_stubs/network_stubs.h"
#include <arpa/inet.h>
#include <assert.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fake_getaddrinfo(const char *, const char *, const struct addrinfo *,
                            struct addrinfo **);
static void fake_freeaddrinfo(struct addrinfo *);
#define getaddrinfo fake_getaddrinfo
#define freeaddrinfo fake_freeaddrinfo
#include "../main/pet_component_network_device.c"
#undef getaddrinfo
#undef freeaddrinfo
static bool secure_ready = true;
static void *worker_arg;
static unsigned subscriptions;
static cJSON *switch_on_read;
static bool private_dns, commit_fail, header_fail;
#ifndef PET_TEST_REAL_VAULT
static bool hash_collision;
#endif
static char active_ns[16], store_ns[16], store_key[16];
static unsigned char store[4300];
static size_t stored;
static struct iterator {
  int unused;
} iterator;
static int opens, writes, read_offset, http_status = 200;
static int64_t announced = 5;
static const char *mqtt_common_name;
static const char *http_body = "hello";
static int fake_getaddrinfo(const char *name, const char *service, const struct addrinfo *hint,
                            struct addrinfo **out) {
  (void)name;
  (void)service;
  assert(hint->ai_family == AF_INET);
  struct addrinfo *a = calloc(1, sizeof *a);
  struct sockaddr_in *s = calloc(1, sizeof *s);
  s->sin_family = AF_INET;
  s->sin_addr.s_addr = htonl(private_dns ? 0x7f000001 : 0x08080808);
  a->ai_addr = (struct sockaddr *)s;
  *out = a;
  return 0;
}
static void fake_freeaddrinfo(struct addrinfo *a) {
  free(a->ai_addr);
  free(a);
}
#ifdef PET_TEST_REAL_VAULT
#include "esp_efuse.h"
#include "esp_hmac.h"
#include "mbedtls/gcm.h"
/* Pre-network feed0 layout and HMAC domain. Build an independent old-format
 * fixture, and reopen it before and after provisioning/forgetting a network app. */
static struct {
  uint8_t id[32], iv[12], tag[16], ciphertext[sizeof(pet_private_secret_t)];
} legacy;
static void check_legacy_feed(bool seed) {
  static const char scope[] = "1\nsensor-panel\napi.example.org\n443\n/old\n";
  static const char domain[] = "HachimiDock/PetUI/credentials/v1";
  if (seed) {
    pet_private_secret_t secret = {.token="old-read-grant"};
    uint8_t key[32];
    assert(!mbedtls_sha256((const uint8_t *)scope, sizeof scope-1, legacy.id,0));
    assert(!mbedtls_sha256((const uint8_t *)domain,sizeof domain,key,0));
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    assert(!mbedtls_gcm_setkey(&gcm,MBEDTLS_CIPHER_ID_AES,key,256));
    assert(!mbedtls_gcm_crypt_and_tag(&gcm,MBEDTLS_GCM_ENCRYPT,sizeof secret,
      legacy.iv,sizeof legacy.iv,legacy.id,sizeof legacy.id,(const uint8_t *)&secret,
      legacy.ciphertext,sizeof legacy.tag,legacy.tag));
    mbedtls_gcm_free(&gcm);
  }
  cJSON *spec=cJSON_Parse("{\"type\":\"bearer\"}");
  pet_wifi_endpoint_t endpoint;
  pet_private_feed_t binding;
  pet_private_secret_t out={0};
  assert(pet_private_feed_parse(&binding,&endpoint,"sensor-panel","https://api.example.org/old",spec));
  assert(pet_private_secret_read(&binding,&out)==PET_PRIVATE_SET && !strcmp(out.token,"old-read-grant"));
  cJSON_Delete(spec);
}
bool esp_efuse_find_purpose(int purpose, esp_efuse_block_t *block) {
  assert(purpose == ESP_EFUSE_KEY_PURPOSE_HMAC_UP); *block = EFUSE_BLK_KEY0; return secure_ready;
}
bool esp_efuse_get_key_dis_read(esp_efuse_block_t b) { return b == EFUSE_BLK_KEY0; }
bool esp_efuse_get_key_dis_write(esp_efuse_block_t b) { return b == EFUSE_BLK_KEY0; }
bool esp_efuse_get_keypurpose_dis_write(esp_efuse_block_t b) { return b == EFUSE_BLK_KEY0; }
int esp_hmac_calculate(hmac_key_id_t id, const void *domain, size_t n, uint8_t *key) {
  assert(!id && secure_ready); return mbedtls_sha256(domain, n, key, 0);
}
void bootloader_random_enable(void) {}
void bootloader_random_disable(void) {}
void esp_fill_random(void *p, size_t n) { memset(p, 0x55, n); }
#else
/* This variant tests storage/transport failures without an SDK installation.
 * The real-vault variant links this same suite with production AES-GCM. */
bool pet_private_storage_ready(void) { return secure_ready; }
bool pet_private_network_crypt(bool seal, const void *binding, size_t binding_size,
                               uint8_t iv[12], uint8_t tag[16], const uint8_t *input,
                               uint8_t *output, size_t size) {
  (void)iv;
  if (!secure_ready) return false;
  unsigned char mac = 0;
  for (size_t i=0;i<binding_size;i++) mac ^= ((const uint8_t *)binding)[i];
  for (size_t i=0;i<size;i++) {
    output[i] = input[i] ^ 0x55;
    mac ^= seal ? output[i] : input[i];
  }
  if (seal) tag[0] = mac;
  return tag[0] == mac;
}
int mbedtls_sha256(const unsigned char *s, size_t n, unsigned char *out, int unused) {
  (void)unused;
  unsigned h = 2166136261u;
  for (size_t i = 0; i < n; i++)
    h = (h ^ s[i]) * 16777619u;
  memset(out, 0, 32);
  if (!hash_collision)
    memcpy(out, &h, 4);
  return 0;
}
#endif
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h) {
  (void)mode;
#ifdef PET_TEST_REAL_VAULT
  if (!strcmp(ns,"pet_credentials")) assert(mode==NVS_READONLY);
#endif
  snprintf(active_ns, 16, "%s", ns);
  *h = 1;
  return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *n) {
  (void)h;
#ifdef PET_TEST_REAL_VAULT
  if (!strcmp(active_ns,"pet_credentials")) {
    if (strcmp(key,"feed0")) return ESP_ERR_NVS_NOT_FOUND;
    assert(out && *n>=sizeof legacy);
    memcpy(out,&legacy,sizeof legacy);
    *n=sizeof legacy;
    return ESP_OK;
  }
#endif
  if (!stored || strcmp(active_ns, store_ns) || strcmp(key, store_key))
    return ESP_ERR_NVS_NOT_FOUND;
  if (!out) {
    *n = stored;
    return ESP_OK;
  }
  if (*n < stored)
    return 2;
  memcpy(out, store, stored);
  *n = stored;
  return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *in, size_t n) {
  (void)h;
  assert(n <= sizeof store);
  memcpy(store, in, n);
  stored = n;
  snprintf(store_ns, 16, "%s", active_ns);
  snprintf(store_key, 16, "%s", key);
  return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) {
  (void)h;
  if (strcmp(store_key, key))
    return ESP_ERR_NVS_NOT_FOUND;
  stored = 0;
  return ESP_OK;
}
esp_err_t nvs_erase_all(nvs_handle_t h) {
  (void)h;
  stored = 0;
  return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {
  (void)h;
  return commit_fail ? 2 : ESP_OK;
}
esp_err_t nvs_entry_find(const char *partition, const char *ns, int type, nvs_iterator_t *out) {
  (void)partition;
  (void)type;
  if (stored && !strcmp(ns, store_ns)) {
    *out = &iterator;
    return ESP_OK;
  }
  *out = NULL;
  return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_entry_next(nvs_iterator_t *it) {
  *it = NULL;
  return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_entry_info(nvs_iterator_t it, nvs_entry_info_t *out) {
  (void)it;
  out->type = NVS_TYPE_BLOB;
  snprintf(out->key, 16, "%s", store_key);
  return ESP_OK;
}
void nvs_release_iterator(nvs_iterator_t it) { (void)it; }
void esp_crt_bundle_attach(void) {}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c) {
  assert(!strcmp(c->host, "8.8.8.8") && !strcmp(c->common_name, "api.example.org"));
  assert(c->crt_bundle_attach && c->disable_auto_redirect && c->port == 443 &&
         c->transport_type == HTTP_TRANSPORT_OVER_SSL);
  read_offset = 0;
  return (void *)1;
}
int esp_http_client_set_header(esp_http_client_handle_t c, const char *k, const char *v) {
  (void)c;
  if (!strcmp(k, "Host"))
    assert(!strcmp(v, "api.example.org"));
  return header_fail ? 2 : ESP_OK;
}
int esp_http_client_set_method(esp_http_client_handle_t c, int method) {
  (void)c;
  (void)method;
  return ESP_OK;
}
int esp_http_client_open(esp_http_client_handle_t c, int n) {
  (void)c;
  (void)n;
  opens++;
  return ESP_OK;
}
int esp_http_client_write(esp_http_client_handle_t c, const char *data, int n) {
  (void)c;
  (void)data;
  writes++;
  return n;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c) {
  (void)c;
  return announced;
}
int esp_http_client_get_status_code(esp_http_client_handle_t c) {
  (void)c;
  return http_status;
}
int esp_http_client_read(esp_http_client_handle_t c, char *out, int n) {
  (void)c;
  if (switch_on_read) {
    pet_net_activate(host, "replacement", switch_on_read);
    switch_on_read = NULL;
  }
  int remaining = (int)strlen(http_body) - read_offset;
  if (n > remaining)
    n = remaining;
  memcpy(out, http_body + read_offset, n);
  read_offset += n;
  return n;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) {
  (void)c;
  return read_offset == (int)strlen(http_body);
}
void esp_http_client_cleanup(esp_http_client_handle_t c) { (void)c; }
esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *c) {
  assert(!strcmp(c->broker.address.hostname, "8.8.8.8"));
  assert(c->broker.address.port == 8883 && c->broker.address.transport == MQTT_TRANSPORT_OVER_SSL &&
         c->broker.verification.crt_bundle_attach);
  assert(c->outbox.limit == 16384 && c->buffer.size == 4096);
  mqtt_common_name = c->broker.verification.common_name;
  return (void *)1;
}
int esp_mqtt_client_register_event(esp_mqtt_client_handle_t c, int id,
                                   void (*f)(void *, esp_event_base_t, int32_t, void *), void *p) {
  (void)c;
  (void)id;
  (void)f;
  (void)p;
  return ESP_OK;
}
int esp_mqtt_client_start(esp_mqtt_client_handle_t c) {
  (void)c;
  return ESP_OK;
}
int esp_mqtt_client_stop(esp_mqtt_client_handle_t c) {
  (void)c;
  return ESP_OK;
}
int esp_mqtt_client_destroy(esp_mqtt_client_handle_t c) {
  (void)c;
  return ESP_OK;
}
int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t c, const char *topic, int qos) {
  (void)c;
  (void)topic;
  (void)qos;
  subscriptions++;
  return 7;
}
int esp_mqtt_client_unsubscribe(esp_mqtt_client_handle_t c, const char *topic) {
  (void)c;
  (void)topic;
  return 8;
}
int esp_mqtt_client_publish(esp_mqtt_client_handle_t c, const char *topic, const char *payload,
                            int n, int qos, int retain) {
  (void)c;
  (void)topic;
  (void)payload;
  (void)n;
  (void)qos;
  assert(!retain);
  return 9;
}
int64_t esp_timer_get_time(void) { return 100; }
void vTaskDelete(void *p) { (void)p; }
void vTaskDelay(int n) { (void)n; }
int xTaskCreate(void (*f)(void *), const char *name, int stack, void *p, int priority, void *task) {
  (void)f;
  (void)name;
  (void)stack;
  worker_arg = p;
  (void)priority;
  (void)task;
  return pdPASS;
}
static int request(pet_net_job_t *j, const char *s) {
  cJSON *r = cJSON_Parse(s);
  assert(r);
  int result = mqtt_request(j, r);
  cJSON_Delete(r);
  return result;
}
int main(void) {
  const char *policy =
      "{\"profile\":\"component-network-v1\",\"https\":[{\"origin\":\"https://"
      "api.example.org\",\"methods\":[\"GET\"]}],\"mqtts\":[{\"host\":\"events.example.org\","
      "\"port\":8883,\"subscribe\":[\"sensors/+/"
      "report\"],\"publish\":[]}],\"credentials\":{\"records\":1,\"totalBytes\":16}}";
  cJSON *p = cJSON_Parse(policy);
  pet_net_lease_t lease;
  secure_ready = false;
  assert(!pet_net_device_activate("sensor-panel", p, &lease));
  assert(pet_net_device_provision("sensor-panel","token","private")==PET_NET_STORAGE);
  assert(!stored);
  secure_ready = true;
#ifdef PET_TEST_REAL_VAULT
  pet_private_storage_init();
  check_legacy_feed(true);
#endif
  assert(pet_net_device_activate("sensor-panel", p, &lease));
  assert(pet_net_device_provision(NULL, "token", "private") == PET_NET_INVALID);
  assert(pet_net_device_provision("different-panel", "token", "private") == PET_NET_UNAVAILABLE);
  const char *target = "https://api.example.org";
  assert(pet_net_device_provision("sensor-panel", "token", "private")==PET_NET_DENIED);
  int provisioned = pet_net_device_provision_target("sensor-panel", "token", "private", target);
  assert(provisioned > 0);
  cJSON *status = pet_net_device_status();
  assert(status && cJSON_IsTrue(cJSON_GetObjectItem(status, "active")));
  assert(cJSON_GetObjectItem(status, "pendingJobs")->valueint == 1);
  char *status_json = cJSON_PrintUnformatted(status);
  assert(status_json && !strstr(status_json, "private") && !strstr(status_json, "token"));
  free(status_json);
  cJSON_Delete(status);
  pet_net_job_t queued;
  assert(pet_net_take(host, &queued));
  assert(queued.id == (uint32_t)provisioned && !strcmp(queued.owner, "sensor-panel"));
  execute_job(&queued);
  const char *get = "{\"op\":\"credential.get\",\"key\":\"token\"}";
  int get_id = pet_net_call(&lease,PET_SERVICE_SUBMIT,get,strlen(get),NULL,0);
  assert(get_id>0 && pet_net_take(host,&queued));
  execute_job(&queued);
  for (int i=0;i<2;i++) {
    char meta[512], byte=0;
    assert(pet_net_call(&lease,PET_SERVICE_POLL,NULL,0,meta,sizeof meta)>0);
    cJSON *result=cJSON_Parse(meta);
    assert(result && !strstr(meta,"private"));
    assert(cJSON_GetObjectItem(result,"status")->valueint == (i ? 200 : 204));
    assert(cJSON_GetObjectItem(result,"request")->valueint == (i ? get_id : provisioned));
    assert(cJSON_GetObjectItem(result,"bytes")->valueint == 0);
    uint32_t read[] = {cJSON_GetObjectItem(result,"handle")->valueint,0};
    assert(pet_net_call(&lease,PET_SERVICE_READ,read,sizeof read,&byte,1)==0);
    assert(pet_net_call(&lease,PET_SERVICE_RELEASE,read,4,NULL,0)==0);
    cJSON_Delete(result);
  }
  pet_net_job_t *j = calloc(1, sizeof *j);
  j->generation = lease.generation;
  strcpy(j->owner, "sensor-panel");
  assert(pet_net_policy_parse(p, &j->policy));
  assert(credential_change(j, "token", "private", target) == 204);
  uint8_t *data = NULL;
  size_t n = 0;
  assert(credential_get("sensor-panel", "token", target, &data, &n) == 200 && n == 7 &&
         !memcmp(data, "private", 7));
  pet_net_zero_free(data, n + 1);
  data = NULL;
  for (size_t i=0;i+7<=stored;i++) assert(memcmp(store+i,"private",7));
  assert(credential_get("sensor-panel","token",NULL,NULL,NULL)==200);
  assert(credential_get("sensor-panel","token",NULL,&data,&n)==PET_NET_STORAGE && !data);
  assert(credential_get("sensor-panel","token","mqtts://api.example.org",&data,&n)==PET_NET_STORAGE);
  assert(credential_get("sensor-panel","token","https://other.example.org",&data,&n)==PET_NET_STORAGE);
  ((credential_t *)store)->target[8] ^= 1;
  assert(credential_get("sensor-panel","token",NULL,NULL,NULL)==PET_NET_STORAGE);
  ((credential_t *)store)->target[8] ^= 1;
  store[stored-1] ^= 1;
  assert(credential_get("sensor-panel","token",target,&data,&n)==PET_NET_STORAGE);
  store[stored-1] ^= 1;
  assert(credential_change(j, "second", "value", target) == PET_NET_STORAGE);
  assert(credential_get("different-panel", "token", target, &data, &n) == 404);
  commit_fail = true;
  assert(!pet_net_device_forget("sensor-panel"));
  commit_fail = false;
  lease.generation = pet_net_activate(host, "sensor-panel", p);
  assert(credential_change(j, "token", "stale", target) == PET_NET_STALE);
  j->generation = lease.generation;
#ifndef PET_TEST_REAL_VAULT
  hash_collision = true;
  stored = 0;
  assert(credential_change(j, "token", "private", target) == 204);
  assert(!credential_forget_inner("different-panel") && stored);
  stored = 0;
  hash_collision = false;
#endif
  cJSON *r = cJSON_Parse("{\"op\":\"http\",\"origin\":\"https://api.example.org\",\"path\":\"/"
                         "items\",\"method\":\"GET\"}");
  assert(http_request(j, r, &data, &n) == 200 && n == 5 && !memcmp(data, "hello", 5));
  pet_net_zero_free(data, n + 1);
  data = NULL;
  private_dns = true;
  int prior = opens;
  assert(http_request(j, r, &data, &n) == PET_NET_DENIED && opens == prior);
  private_dns = false;
  announced = PET_NETWORK_RESPONSE_BYTES + 1;
  assert(http_request(j, r, &data, &n) == PET_NET_LIMIT && !data);
  announced = 5;
  http_status = 302;
  assert(http_request(j, r, &data, &n) == PET_NET_DENIED && !data);
  http_status = 200;
  header_fail = true;
  prior = opens;
  assert(http_request(j, r, &data, &n) == PET_NET_TRANSPORT && opens == prior);
  header_fail = false;
  cJSON_Delete(r);
  assert(request(j, "{\"op\":\"mqtt.connect\",\"host\":\"events.example.org\",\"clientId\":"
                    "\"sensor-test\"}") == 0);
  assert(!strcmp(mqtt_common_name, "events.example.org"));
  esp_mqtt_event_t e = {.topic = "sensors/one/report",
                        .topic_len = 18,
                        .data = "abc",
                        .data_len = 3,
                        .total_data_len = 6};
  void *cookie = (void *)atomic_load(&active_cookie);
  mqtt_event(cookie,NULL,MQTT_EVENT_CONNECTED,&e);
  char connected[512];
  assert(pet_net_call(&lease, PET_SERVICE_POLL, NULL, 0, connected, 512)>0);
  mqtt_event(cookie, NULL, MQTT_EVENT_DATA, &e);
  assert(mqtt.used == 3 && mqtt.message);
  e.current_data_offset = 3;
  e.data = "def";
  mqtt_event(cookie, NULL, MQTT_EVENT_DATA, &e);
  assert(!mqtt.message && host->retained == 6);
  char event[512];
  assert(pet_net_call(&lease, 2, NULL, 0, event, 512) > 0);
  cJSON *v = cJSON_Parse(event);
  uint32_t input[] = {cJSON_GetObjectItem(v, "handle")->valueint, 0};
  char out[7] = {0};
  assert(pet_net_call(&lease, 3, input, 8, out, 6) == 6 && !strcmp(out, "abcdef"));
  assert(pet_net_call(&lease, 4, input, 4, NULL, 0) == 0);
  cJSON_Delete(v);
  e.current_data_offset = 0;
  e.total_data_len = PET_NETWORK_MESSAGE_BYTES + 1;
  mqtt_event(cookie, NULL, MQTT_EVENT_DATA, &e);
  assert(!mqtt.message);
  e.total_data_len = 6;
  e.topic = "other/one/report";
  e.topic_len = 16;
  mqtt_event(cookie, NULL, MQTT_EVENT_DATA, &e);
  assert(!mqtt.message);
  e.topic = "sensors/one/report";
  e.topic_len = 18;
  e.current_data_offset = 0;
  mqtt_event(cookie, NULL, MQTT_EVENT_DATA, &e);
  assert(mqtt.message);
  e.current_data_offset = 4;
  mqtt_event(cookie, NULL, MQTT_EVENT_DATA, &e);
  assert(!mqtt.message);
  for (int i = 0; i < 8; i++) {
    char s[160];
    snprintf(s, sizeof s,
             "{\"op\":\"mqtt.subscribe\",\"host\":\"events.example.org\",\"topic\":\"sensors/%d/"
             "report\"}",
             i);
    assert(request(j, s) == 7);
  }
  assert(request(j, "{\"op\":\"mqtt.subscribe\",\"host\":\"events.example.org\",\"topic\":"
                    "\"sensors/ninth/report\"}") == PET_NET_LIMIT);
  assert(subscriptions == 8);
  e.current_data_offset=0;
  mqtt_event(cookie,NULL,MQTT_EVENT_DATA,&e);
  assert(mqtt.message);
  mqtt_event(cookie,NULL,MQTT_EVENT_DISCONNECTED,&e);
  assert(!mqtt.message && !atomic_load(&mqtt_ready));
  mqtt_event(cookie,NULL,MQTT_EVENT_CONNECTED,&e);
  mqtt_maintain(j->generation);
  assert(subscriptions==16);
  /* The response started under the old lease; switch after socket read and
   * before completion. Neither its body nor its status reaches the new app. */
  const char *http = "{\"op\":\"http\",\"origin\":\"https://api.example.org\",\"method\":\"GET\",\"path\":\"/\"}";
  assert(pet_net_call(&lease,PET_SERVICE_SUBMIT,http,strlen(http),NULL,0)>0);
  assert(pet_net_take(host,&queued));
  switch_on_read=p;
  execute_job(&queued);
  assert(!switch_on_read && !host->retained);
  assert(pet_net_call(&lease,PET_SERVICE_POLL,NULL,0,event,sizeof event)==PET_NET_STALE);
  lease.generation=pet_net_generation(host);
  assert(pet_net_call(&lease,PET_SERVICE_POLL,NULL,0,event,sizeof event)==0);
  mqtt_maintain(lease.generation);
  assert(!mqtt.client && !atomic_load(&mqtt_ready));
  mqtt_event(cookie,NULL,MQTT_EVENT_CONNECTED,&e);
  assert(!atomic_load(&mqtt_ready));
#ifdef PET_TEST_REAL_VAULT
  assert(pet_net_device_forget("sensor-panel"));
  check_legacy_feed(false);
#endif
  free(worker_arg);
  pet_net_destroy(host);
  free(host);
  host = NULL;
  free(j);
  cJSON_Delete(p);
  puts("Backend DNS pinning, TLS settings, quotas, isolation, deletion failure and MQTT fragments "
       "passed");
}
