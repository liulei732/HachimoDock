#include "pet_component_network_device.h"
#if PET_COMPONENT_NETWORK_ENABLED
#include "pet_p4_private_feed.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "mqtt_client.h"
#include "nvs.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static pet_net_host_t *host;
static atomic_bool mqtt_ready;
static atomic_uint mqtt_messages;
static atomic_int last_http_status, last_transport_status;
static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t credential_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *field(const cJSON *o, const char *key) {
  return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, key));
}
static bool hash_name(const char *value, char out[15]) {
  unsigned char digest[32];
  if (mbedtls_sha256((const unsigned char *)value, strlen(value), digest, 0)) return false;
  static const char hex[] = "0123456789abcdef";
  out[0] = 'c';
  for (unsigned i = 0; i < 6; i++) {
    out[1 + 2 * i] = hex[digest[i] >> 4];
    out[2 + 2 * i] = hex[digest[i] & 15];
  }
  out[13] = 0;
  return true;
}
typedef struct {
  char owner[48], key[33], target[104];
  uint32_t size;
  uint8_t iv[12], tag[16];
  uint8_t value[];
} credential_t;
static bool record_valid(const credential_t *r, size_t n, const char *owner, const char *key) {
  return n >= sizeof *r + 1 && n <= sizeof *r + 4097 && memchr(r->owner, 0, sizeof r->owner) &&
         memchr(r->key, 0, sizeof r->key) && memchr(r->target, 0, sizeof r->target) &&
         !strcmp(owner, r->owner) && (!key || !strcmp(key, r->key)) &&
         r->size <= 4097 && r->size + sizeof *r == n && r->size > 0;
}
static uint8_t *open_record(credential_t *r) {
  uint8_t *plain = pet_net_alloc(r->size);
  if (plain && pet_private_network_crypt(false, r, offsetof(credential_t, iv),
      r->iv, r->tag, r->value, plain, r->size) && !plain[r->size - 1]) return plain;
  pet_net_zero_free(plain, r->size);
  return NULL;
}
/* Verify stored metadata before counting, overwriting or retiring a record. */
static bool stored_record(credential_t *r, size_t n, const char *owner, const char *slot) {
  char name[15];
  if (!record_valid(r, n, owner, NULL) || !hash_name(r->key, name) || strcmp(name, slot))
    return false;
  uint8_t *plain = open_record(r);
  bool valid = plain != NULL;
  pet_net_zero_free(plain, r->size);
  return valid;
}
static int credential_get_inner(const char *owner, const char *key, const char *target,
                                uint8_t **out, size_t *length) {
  if (!pet_private_storage_ready()) return PET_NET_STORAGE;
  char ns[15], name[15];
  if (!hash_name(owner, ns) || !hash_name(key, name)) return PET_NET_STORAGE;
  nvs_handle_t h;
  esp_err_t e = nvs_open(ns, NVS_READONLY, &h);
  if (e == ESP_ERR_NVS_NOT_FOUND)
    return 404;
  if (e != ESP_OK)
    return PET_NET_STORAGE;
  size_t n = 0;
  e = nvs_get_blob(h, name, NULL, &n);
  if (e == ESP_ERR_NVS_NOT_FOUND) {
    nvs_close(h);
    return 404;
  }
  size_t capacity = n;
  credential_t *r =
      e == ESP_OK && n >= sizeof *r + 1 && n <= sizeof *r + 4097 ? pet_net_alloc(n) : NULL;
  bool ok = r && nvs_get_blob(h, name, r, &n) == ESP_OK && record_valid(r, n, owner, key);
  nvs_close(h);
  if (ok) {
    /* NULL target is a presence query and can never return the secret. */
    ok = target ? (out && length && !strcmp(target, r->target)) : !out && !length;
    uint8_t *plain = ok ? open_record(r) : NULL;
    ok = plain != NULL;
    if (ok && out) { *out = plain; *length = r->size - 1; }
    else pet_net_zero_free(plain, r->size);
  }
  pet_net_zero_free(r, capacity);
  return ok ? 200 : PET_NET_STORAGE;
}
static int credential_change_inner(const pet_net_job_t *j, const char *key, const char *value, const char *target) {
  char bound[104];
  if (!pet_private_storage_ready()) return PET_NET_STORAGE;
  if (value && !pet_net_credential_target(&j->policy, target, bound)) return PET_NET_DENIED;
  char ns[15], name[15];
  if (!hash_name(j->owner, ns) || !hash_name(key, name)) return PET_NET_STORAGE;
  nvs_handle_t h;
  if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK)
    return PET_NET_STORAGE;
  nvs_iterator_t it = NULL;
  esp_err_t e = nvs_entry_find("nvs", ns, NVS_TYPE_ANY, &it);
  unsigned count = 0;
  size_t total = 0;
  bool ok = e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND;
  while (e == ESP_OK && ok) {
    nvs_entry_info_t info;
    if (nvs_entry_info(it, &info) != ESP_OK) { ok = false; break; }
    size_t n = 0;
    ok = info.type == NVS_TYPE_BLOB && nvs_get_blob(h, info.key, NULL, &n) == ESP_OK &&
         n >= sizeof(credential_t) + 1 && n <= sizeof(credential_t) + 4097;
    size_t capacity = n;
    credential_t *r = ok ? pet_net_alloc(n) : NULL;
    ok = r && nvs_get_blob(h, info.key, r, &n) == ESP_OK && stored_record(r, n, j->owner, info.key);
    if (ok && strcmp(info.key, name)) {
      count++;
      total += r->size - 1;
    }
    if (ok && !strcmp(info.key, name) && strcmp(r->key, key))
      ok = false;
    pet_net_zero_free(r, capacity);
    if (ok)
      e = nvs_entry_next(&it);
  }
  if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND)
    ok = false;
  nvs_release_iterator(it);
  if (value && (count >= j->policy.records || strlen(value) > j->policy.credential_bytes ||
                total > j->policy.credential_bytes - strlen(value)))
    ok = false;
  if (ok && value) {
    size_t bytes = strlen(value) + 1, n = sizeof(credential_t) + bytes;
    credential_t *r = pet_net_alloc(n);
    ok = r != NULL;
    if (r) {
      memset(r, 0, n);
      snprintf(r->owner, 48, "%s", j->owner);
      snprintf(r->key, 33, "%s", key);
      r->size = bytes;
      strcpy(r->target, bound);
      ok = pet_private_network_crypt(true, r, offsetof(credential_t, iv),
            r->iv, r->tag, (const uint8_t *)value, r->value, bytes) &&
           nvs_set_blob(h, name, r, n) == ESP_OK;
      pet_net_zero_free(r, n);
    }
  } else if (ok) {
    e = nvs_erase_key(h, name);
    ok = e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND;
  }
  if (ok)
    ok = nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok ? 204 : PET_NET_STORAGE;
}
static bool credential_forget_inner(const char *owner) {
  if (!owner || !*owner)
    return false;
  /* Validate every record's full owner before erasing a hashed namespace. */
  char ns[15];
  if (!hash_name(owner, ns)) return false;
  nvs_handle_t h;
  esp_err_t opened = nvs_open(ns, NVS_READONLY, &h);
  if (opened == ESP_ERR_NVS_NOT_FOUND)
    return true;
  if (opened != ESP_OK)
    return false;
  nvs_iterator_t it = NULL;
  esp_err_t e = nvs_entry_find("nvs", ns, NVS_TYPE_ANY, &it);
  bool ok = e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND;
  while (e == ESP_OK && ok) {
    nvs_entry_info_t info;
    if (nvs_entry_info(it, &info) != ESP_OK) { ok = false; break; }
    size_t n = 0;
    ok = info.type == NVS_TYPE_BLOB && nvs_get_blob(h, info.key, NULL, &n) == ESP_OK &&
         n >= sizeof(credential_t) + 1 && n <= sizeof(credential_t) + 4097;
    size_t capacity = n;
    credential_t *r = ok ? pet_net_alloc(n) : NULL;
    ok = r && nvs_get_blob(h, info.key, r, &n) == ESP_OK && stored_record(r, n, owner, info.key);
    pet_net_zero_free(r, capacity);
    if (ok)
      e = nvs_entry_next(&it);
  }
  if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND)
    ok = false;
  nvs_release_iterator(it);
  nvs_close(h);
  if (ok) {
    if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
    ok = nvs_erase_all(h) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
  }
  return ok;
}
static int credential_get(const char *owner, const char *key, const char *target, uint8_t **out, size_t *n) {
  pthread_mutex_lock(&credential_lock);
  int result = credential_get_inner(owner, key, target, out, n);
  pthread_mutex_unlock(&credential_lock);
  return result;
}
static int credential_change(const pet_net_job_t *j, const char *key, const char *value, const char *target) {
  pthread_mutex_lock(&credential_lock);
  int result = pet_net_generation(host) == j->generation ? credential_change_inner(j, key, value, target)
                                                         : PET_NET_STALE;
  pthread_mutex_unlock(&credential_lock);
  return result;
}
bool pet_net_device_forget(const char *owner) {
  if (!owner || !*owner)
    return false;
  pthread_mutex_lock(&init_lock);
  bool current = false;
  pthread_mutex_lock(&credential_lock);
  if (host) {
    pthread_mutex_lock(&host->mutex);
    current = host->active && !strcmp(host->owner, owner);
    pthread_mutex_unlock(&host->mutex);
    if (current)
      pet_net_activate(host, NULL, NULL);
  }
  bool ok = credential_forget_inner(owner);
  pthread_mutex_unlock(&credential_lock);
  pthread_mutex_unlock(&init_lock);
  return ok;
}
static bool resolve_public(const char *name, char ip[INET_ADDRSTRLEN]) {
  struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *list = NULL;
  if (getaddrinfo(name, NULL, &hints, &list) != 0)
    return false;
  bool ok = true, found = false;
  for (struct addrinfo *p = list; p; p = p->ai_next) {
    const struct sockaddr_in *a = (const struct sockaddr_in *)p->ai_addr;
    if (!pet_net_public_ipv4(ntohl(a->sin_addr.s_addr))) {
      ok = false;
      break;
    }
    if (!found)
      found = inet_ntop(AF_INET, &a->sin_addr, ip, INET_ADDRSTRLEN) != NULL;
  }
  freeaddrinfo(list);
  return ok && found;
}
static int http_request(const pet_net_job_t *j, const cJSON *r, uint8_t **out, size_t *length) {
  char ip[INET_ADDRSTRLEN];
  const char *origin = field(r, "origin"), *name = origin + 8;
  *length = 0;
  if (!resolve_public(name, ip))
    return PET_NET_DENIED;
  if (pet_net_generation(host) != j->generation)
    return PET_NET_STALE;
  esp_http_client_config_t cfg = {.host = ip,
                                  .port = 443,
                                  .path = field(r, "path"),
                                  .transport_type = HTTP_TRANSPORT_OVER_SSL,
                                  .common_name = name,
                                  .crt_bundle_attach = esp_crt_bundle_attach,
                                  .timeout_ms = 5000,
                                  .disable_auto_redirect = true,
                                  .max_authorization_retries = -1,
                                  .buffer_size = 2048,
                                  .buffer_size_tx = 4096};
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c)
    return PET_NET_BUSY;
  int result = PET_NET_TRANSPORT;
  uint8_t *secret = NULL;
  size_t secret_size = 0;
  char *authorization = NULL;
  if (esp_http_client_set_header(c, "Host", name) != ESP_OK ||
      esp_http_client_set_header(c, "Accept", "application/json") != ESP_OK)
    goto done;
  const cJSON *headers = cJSON_GetObjectItemCaseSensitive(r, "headers"), *v;
  cJSON_ArrayForEach(v, headers) if (esp_http_client_set_header(c, v->string, v->valuestring) !=
                                     ESP_OK) goto done;
  const cJSON *auth = cJSON_GetObjectItemCaseSensitive(r, "authorization");
  if (auth) {
    if (credential_get(j->owner, field(auth, "credential"), origin, &secret, &secret_size) != 200) {
      result = PET_NET_STORAGE;
      goto done;
    }
    size_t n = strlen(field(auth, "prefix")) + secret_size + 1;
    authorization = pet_net_alloc(n);
    if (!authorization) {
      result = PET_NET_BUSY;
      goto done;
    }
    snprintf(authorization, n, "%s%s", field(auth, "prefix"), (char *)secret);
    if (strpbrk(authorization, "\r\n")) {
      result = PET_NET_DENIED;
      goto done;
    }
    if (esp_http_client_set_header(c, "Authorization", authorization) != ESP_OK)
      goto done;
  }
  const char *body = field(r, "body");
  int n = body ? (int)strlen(body) : 0;
  if (esp_http_client_set_method(
          c, !strcmp(field(r, "method"), "POST") ? HTTP_METHOD_POST : HTTP_METHOD_GET) != ESP_OK)
    goto done;
  if (pet_net_generation(host) != j->generation) { result = PET_NET_STALE; goto done; }
  if (esp_http_client_open(c, n) != ESP_OK)
    goto done;
  if (n && esp_http_client_write(c, body, n) != n)
    goto done;
  int64_t announced = esp_http_client_fetch_headers(c);
  if (announced < 0)
    goto done;
  if (announced > PET_NETWORK_RESPONSE_BYTES) {
    result = PET_NET_LIMIT;
    goto done;
  }
  int status = esp_http_client_get_status_code(c);
  if (status >= 300 && status < 400) {
    result = PET_NET_DENIED;
    goto done;
  }
  *out = pet_net_alloc(PET_NETWORK_RESPONSE_BYTES + 1);
  if (!*out) {
    result = PET_NET_BUSY;
    goto done;
  }
  int64_t deadline = esp_timer_get_time() + 12000000;
  for (;;) {
    if (pet_net_generation(host) != j->generation) {
      result = PET_NET_STALE;
      break;
    }
    if (esp_timer_get_time() > deadline)
      break;
    size_t remaining = PET_NETWORK_RESPONSE_BYTES - *length;
    if (!remaining) {
      if (esp_http_client_is_complete_data_received(c)) {
        result = status;
        break;
      }
      result = PET_NET_LIMIT;
      break;
    }
    int got =
        esp_http_client_read(c, (char *)*out + *length, remaining > 4096 ? 4096 : (int)remaining);
    if (got < 0)
      break;
    if (!got) {
      if (esp_http_client_is_complete_data_received(c))
        result = status;
      break;
    }
    *length += (size_t)got;
  }
  if (result > 0) {
    uint8_t *exact = pet_net_alloc(*length + 1);
    if (exact) {
      memcpy(exact, *out, *length);
      exact[*length] = 0;
      pet_net_zero_free(*out, PET_NETWORK_RESPONSE_BYTES + 1);
      *out = exact;
    } else
      result = PET_NET_BUSY;
  }
done:
  esp_http_client_cleanup(c);
  pet_net_zero_free(secret, secret ? secret_size + 1 : 0);
  pet_net_zero_free(authorization, authorization ? strlen(authorization) : 0);
  if (result < 0) {
    pet_net_zero_free(*out, *out ? PET_NETWORK_RESPONSE_BYTES + 1 : 0);
    *out = NULL;
    *length = 0;
  }
  return result;
}
typedef struct {
  esp_mqtt_client_handle_t client;
  uint32_t generation;
  char broker[96];
  uint8_t *message;
  size_t used, expected;
  char topic[129];
  pet_net_broker_t policy;
  char subscriptions[8][129];
  int subscription_qos[8];
  unsigned subscription_count;
  bool was_connected; /* Only the MQTT event task writes this while running. */
} mqtt_session_t;
static mqtt_session_t mqtt;
/* The registration cookie distinguishes reconnecting clients even within one
 * lease or if the allocator recycles the SDK handle. Retire before stop/destroy. */
static uintptr_t next_cookie;
static atomic_uintptr_t active_cookie;
static atomic_bool restore_subscriptions;
static void mqtt_event_current(int32_t event, void *value) {
  esp_mqtt_event_handle_t e = value;
  if (pet_net_generation(host) != mqtt.generation)
    return;
  if (event == MQTT_EVENT_CONNECTED) {
    atomic_store(&restore_subscriptions, mqtt.was_connected);
    mqtt.was_connected = true;
    atomic_store(&mqtt_ready, true);
    atomic_store(&last_transport_status, 0);
  }
  if (event == MQTT_EVENT_DISCONNECTED || event == MQTT_EVENT_ERROR) {
    atomic_store(&mqtt_ready, false);
    atomic_store(&restore_subscriptions, false);
    pet_net_zero_free(mqtt.message, mqtt.expected);
    mqtt.message = NULL;
    mqtt.used = mqtt.expected = 0;
    if (event == MQTT_EVENT_ERROR)
      atomic_store(&last_transport_status, PET_NET_TRANSPORT);
  }
  if (event == MQTT_EVENT_CONNECTED || event == MQTT_EVENT_DISCONNECTED ||
      event == MQTT_EVENT_SUBSCRIBED || event == MQTT_EVENT_UNSUBSCRIBED ||
      event == MQTT_EVENT_PUBLISHED || event == MQTT_EVENT_ERROR) {
    const char *type = event == MQTT_EVENT_CONNECTED      ? "mqtt.connected"
                       : event == MQTT_EVENT_DISCONNECTED ? "mqtt.disconnected"
                       : event == MQTT_EVENT_SUBSCRIBED   ? "mqtt.subscribed"
                       : event == MQTT_EVENT_UNSUBSCRIBED ? "mqtt.unsubscribed"
                       : event == MQTT_EVENT_PUBLISHED    ? "mqtt.published"
                                                          : "mqtt.error";
    int status = event == MQTT_EVENT_ERROR ? PET_NET_TRANSPORT : e->msg_id;
    if (event == MQTT_EVENT_SUBSCRIBED && e->data_len > 0 && e->data &&
        (unsigned char)e->data[0] >= 128)
      status = PET_NET_DENIED;
    pet_net_complete(host, mqtt.generation, 0, type, status, NULL, NULL, 0);
  } else if (event == MQTT_EVENT_DATA) {
    if (e->current_data_offset == 0) {
      pet_net_zero_free(mqtt.message, mqtt.expected);
      mqtt.message = NULL;
      mqtt.expected = mqtt.used = 0;
      if (e->total_data_len < 0 || e->total_data_len > PET_NETWORK_MESSAGE_BYTES ||
          e->topic_len < 1 || e->topic_len > 128 || !e->topic) {
        pet_net_complete(host, mqtt.generation, 0, "mqtt.message", PET_NET_LIMIT, NULL, NULL, 0);
        return;
      }
      mqtt.expected = (size_t)e->total_data_len;
      mqtt.message = pet_net_alloc(mqtt.expected);
      if (!mqtt.message) {
        pet_net_complete(host, mqtt.generation, 0, "mqtt.message", PET_NET_BUSY, NULL, NULL, 0);
        return;
      }
      memcpy(mqtt.topic, e->topic, e->topic_len);
      mqtt.topic[e->topic_len] = 0;
      bool allowed = false;
      if (!memchr(e->topic, 0, e->topic_len))
        for (unsigned i = 0; i < mqtt.policy.subs; i++)
          if (pet_net_topic_contains(mqtt.policy.sub[i], mqtt.topic, false))
            allowed = true;
      if (!allowed) {
        pet_net_zero_free(mqtt.message, mqtt.expected);
        mqtt.message = NULL;
        pet_net_complete(host, mqtt.generation, 0, "mqtt.message", PET_NET_DENIED, NULL, NULL, 0);
        return;
      }
    }
    if (!mqtt.message)
      return;
    if (e->current_data_offset < 0 || (size_t)e->current_data_offset != mqtt.used ||
        e->data_len < 0 || (size_t)e->data_len > mqtt.expected - mqtt.used ||
        e->total_data_len != (int)mqtt.expected) {
      pet_net_zero_free(mqtt.message, mqtt.expected);
      mqtt.message = NULL;
      pet_net_complete(host, mqtt.generation, 0, "mqtt.message", PET_NET_INVALID, NULL, NULL, 0);
      return;
    }
    if (e->data_len && !e->data) {
      pet_net_zero_free(mqtt.message, mqtt.expected);
      mqtt.message = NULL;
      return;
    }
    if (e->data_len)
      memcpy(mqtt.message + mqtt.used, e->data, e->data_len);
    mqtt.used += e->data_len;
    if (mqtt.used == mqtt.expected) {
      uint8_t *data = mqtt.message;
      mqtt.message = NULL;
      if (pet_net_complete(host, mqtt.generation, 0, "mqtt.message", 0, mqtt.topic, data,
                           mqtt.used))
        atomic_fetch_add(&mqtt_messages, 1);
    }
  }
}
static void mqtt_event(void *arg, esp_event_base_t base, int32_t event, void *value) {
  (void)base;
  pthread_mutex_lock(&init_lock);
  if (arg && (uintptr_t)arg == atomic_load(&active_cookie))
    mqtt_event_current(event, value);
  pthread_mutex_unlock(&init_lock);
}
static void mqtt_close(void) {
  pthread_mutex_lock(&init_lock);
  atomic_store(&active_cookie, 0);
  atomic_store(&restore_subscriptions, false);
  pthread_mutex_unlock(&init_lock);
  atomic_store(&mqtt_ready, false);
  if (mqtt.client) {
    esp_mqtt_client_stop(mqtt.client);
    esp_mqtt_client_destroy(mqtt.client);
  }
  pet_net_zero_free(mqtt.message, mqtt.expected);
  memset(&mqtt, 0, sizeof mqtt);
}
static int mqtt_request(const pet_net_job_t *j, const cJSON *r) {
  if (pet_net_generation(host) != j->generation) return PET_NET_STALE;
  const char *op = field(r, "op");
  if (!strcmp(op, "mqtt.disconnect")) {
    mqtt_close();
    return 0;
  }
  if (!strcmp(op, "mqtt.connect")) {
    mqtt_close();
    char ip[INET_ADDRSTRLEN];
    const char *name = field(r, "host");
    if (!resolve_public(name, ip))
      return PET_NET_DENIED;
    if (pet_net_generation(host) != j->generation)
      return PET_NET_STALE;
    uint8_t *secret = NULL;
    size_t n = 0;
    const char *key = field(r, "passwordCredential");
    char target[104];
    snprintf(target, sizeof target, "mqtts://%s", name);
    if (key && credential_get(j->owner, key, target, &secret, &n) != 200)
      return PET_NET_STORAGE;
    esp_mqtt_client_config_t cfg = {.broker.address.hostname = ip,
                                    .broker.address.port = 8883,
                                    .broker.address.transport = MQTT_TRANSPORT_OVER_SSL,
                                    .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
                                    .broker.verification.common_name = mqtt.broker,
                                    .credentials.client_id = field(r, "clientId"),
                                    .credentials.username = field(r, "username"),
                                    .credentials.authentication.password = (const char *)secret,
                                    .session.keepalive = 45,
                                    .network.timeout_ms = 5000,
                                    .network.reconnect_timeout_ms = 10000,
                                    .buffer.size = 4096,
                                    .task.stack_size = 8192,
                                    .outbox.limit = 16384};
    if (next_cookie == UINTPTR_MAX) { pet_net_zero_free(secret, secret ? n + 1 : 0); return PET_NET_LIMIT; }
    mqtt.generation = j->generation;
    for (unsigned i = 0; i < j->policy.brokers; i++)
      if (!strcmp(name, j->policy.mqtt[i].host))
        mqtt.policy = j->policy.mqtt[i];
    snprintf(mqtt.broker, 96, "%s", name);
    mqtt.client = esp_mqtt_client_init(&cfg);
    pet_net_zero_free(secret, secret ? n + 1 : 0);
    if (!mqtt.client)
      return PET_NET_BUSY;
    uintptr_t cookie = ++next_cookie;
    atomic_store(&active_cookie, cookie);
    if (pet_net_generation(host) != j->generation ||
        esp_mqtt_client_register_event(mqtt.client, ESP_EVENT_ANY_ID, mqtt_event, (void *)cookie) != ESP_OK ||
        esp_mqtt_client_start(mqtt.client) != ESP_OK) {
      mqtt_close();
      return PET_NET_TRANSPORT;
    }
    return 0;
  }
  if (!mqtt.client || !atomic_load(&mqtt_ready) || mqtt.generation != j->generation || strcmp(field(r, "host"), mqtt.broker))
    return PET_NET_UNAVAILABLE;
  const cJSON *q = cJSON_GetObjectItemCaseSensitive(r, "qos");
  int qos = cJSON_IsNumber(q) ? q->valueint : 0;
  unsigned slot = 0;
  while (slot < mqtt.subscription_count && strcmp(mqtt.subscriptions[slot], field(r, "topic")))
    slot++;
  if (!strcmp(op, "mqtt.subscribe") && slot == mqtt.subscription_count && slot == 8)
    return PET_NET_LIMIT;
  int id =
      !strcmp(op, "mqtt.subscribe") ? esp_mqtt_client_subscribe(mqtt.client, field(r, "topic"), qos)
      : !strcmp(op, "mqtt.unsubscribe")
          ? esp_mqtt_client_unsubscribe(mqtt.client, field(r, "topic"))
          : esp_mqtt_client_publish(mqtt.client, field(r, "topic"), field(r, "payload"), 0, qos, 0);
  if (id >= 0 && !strcmp(op, "mqtt.subscribe")) {
    if (slot == mqtt.subscription_count)
      snprintf(mqtt.subscriptions[mqtt.subscription_count++], 129, "%s", field(r, "topic"));
    mqtt.subscription_qos[slot] = qos;
  }
  if (id >= 0 && !strcmp(op, "mqtt.unsubscribe") && slot < mqtt.subscription_count) {
    memmove(mqtt.subscriptions + slot, mqtt.subscriptions + slot + 1,
            (mqtt.subscription_count - slot - 1) * sizeof mqtt.subscriptions[0]);
    memmove(mqtt.subscription_qos + slot, mqtt.subscription_qos + slot + 1,
            (mqtt.subscription_count - slot - 1) * sizeof mqtt.subscription_qos[0]);
    mqtt.subscription_count--;
  }
  return id < 0 ? PET_NET_TRANSPORT : id;
}
/* The worker owns desired subscriptions. Never call SDK subscribe from its
 * event callback; the reconnect event only schedules replay. */
static void mqtt_maintain(uint32_t gen) {
  if (!mqtt.client) return;
  if (mqtt.generation != gen) { mqtt_close(); return; }
  if (!atomic_exchange(&restore_subscriptions, false)) return;
  for (unsigned i = 0; i < mqtt.subscription_count && atomic_load(&mqtt_ready); i++) {
    if (pet_net_generation(host) != gen) return;
    if (esp_mqtt_client_subscribe(mqtt.client, mqtt.subscriptions[i], mqtt.subscription_qos[i]) < 0)
      pet_net_complete(host, gen, 0, "mqtt.error", PET_NET_TRANSPORT, NULL, NULL, 0);
  }
}
static void execute_job(pet_net_job_t *j) {
  cJSON *r = cJSON_Parse(j->request);
  const char *op = r ? field(r, "op") : NULL;
  int status = PET_NET_INVALID;
  uint8_t *data = NULL;
  size_t bytes = 0;
  if (op && !strcmp(op, "http")) {
    status = http_request(j, r, &data, &bytes);
    pthread_mutex_lock(&init_lock);
    if (pet_net_generation(host) == j->generation) atomic_store(&last_http_status, status);
    pthread_mutex_unlock(&init_lock);
  } else if (op && !strncmp(op, "credential.", 11)) {
    if (!strcmp(op, "credential.get"))
      status = credential_get(j->owner, field(r, "key"), NULL, NULL, NULL);
    else
      status = credential_change(j, field(r, "key"),
                                 !strcmp(op, "credential.set") ? field(r, "value") : NULL, field(r, "target"));
  } else if (op)
    status = mqtt_request(j, r);
  pet_net_complete(host, j->generation, j->id, op ? op : "error", status, NULL, data, bytes);
  pet_net_request_delete(r);
  pet_net_job_dispose(j);
}
static void worker(void *arg) {
  pet_net_job_t *j = arg;
  for (;;) {
    uint32_t gen = pet_net_generation(host);
    mqtt_maintain(gen);
    if (!pet_net_take(host, j)) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    execute_job(j);
  }
}
bool pet_net_device_storage_ready(void) { return pet_private_storage_ready(); }
bool pet_net_device_activate(const char *owner, const cJSON *policy, pet_net_lease_t *lease) {
  pthread_mutex_lock(&init_lock);
  if (policy && cJSON_GetObjectItemCaseSensitive(policy, "credentials") && !pet_net_device_storage_ready())
    policy = NULL;
  if (!host && owner && policy) {
    host = pet_net_alloc(sizeof *host);
    if (host) {
      pet_net_init(host);
      pet_net_job_t *job = pet_net_alloc(sizeof *job);
      if (!job || xTaskCreate(worker, "component_net", 12288, job, 3, NULL) != pdPASS) {
        free(job);
        pet_net_destroy(host);
        free(host);
        host = NULL;
      }
    }
  }
  pthread_mutex_lock(&credential_lock);
  uint32_t gen = host ? pet_net_activate(host, owner, policy) : 0;
  pthread_mutex_unlock(&credential_lock);
  atomic_store(&mqtt_ready, false);
  atomic_store(&last_transport_status, 0);
  atomic_store(&mqtt_messages, 0);
  atomic_store(&last_http_status, 0);
  if (lease)
    *lease = (pet_net_lease_t){host, gen};
  pthread_mutex_unlock(&init_lock);
  return gen != 0;
}

/* Wired provisioning is write-only and scoped to the active declared owner.
 * An accepted request is queued; the guest receives the ordinary completion. */
int pet_net_device_provision(const char *owner, const char *key, const char *value) {
  return pet_net_device_provision_target(owner, key, value, NULL);
}
int pet_net_device_provision_target(const char *owner, const char *key, const char *value,
                                    const char *target) {
  if (!owner || !key || !value)
    return PET_NET_INVALID;
  if (!pet_net_device_storage_ready()) return PET_NET_STORAGE;
  pthread_mutex_lock(&init_lock);
  pet_net_lease_t lease = {0};
  if (host) {
    pthread_mutex_lock(&host->mutex);
    if (host->active && !strcmp(host->owner, owner))
      lease = (pet_net_lease_t){host, host->generation};
    pthread_mutex_unlock(&host->mutex);
  }
  int result = PET_NET_UNAVAILABLE;
  if (lease.generation) {
    cJSON *r = cJSON_CreateObject();
    if (r && (!target || cJSON_AddStringToObject(r, "target", target)) && cJSON_AddStringToObject(r, "op", "credential.set") &&
        cJSON_AddStringToObject(r, "key", key) && cJSON_AddStringToObject(r, "value", value)) {
      char *json = cJSON_PrintUnformatted(r);
      if (json) {
        result = pet_net_call(&lease, PET_SERVICE_SUBMIT, json, strlen(json), NULL, 0);
        pet_net_zero_free(json, strlen(json));
      }
    }
    pet_net_request_delete(r);
  }
  pthread_mutex_unlock(&init_lock);
  return result;
}
cJSON *pet_net_device_status(void) {
  cJSON *r = cJSON_CreateObject();
  if (!r)
    return NULL;
  pthread_mutex_lock(&init_lock);
  bool active = false;
  if (host) {
    pthread_mutex_lock(&host->mutex);
    active = host->active;
    cJSON_AddStringToObject(r, "component", host->owner);
    cJSON_AddNumberToObject(r, "pendingJobs", host->queued);
    cJSON_AddNumberToObject(r, "retainedBytes", host->retained);
    pthread_mutex_unlock(&host->mutex);
  }
  cJSON_AddBoolToObject(r, "ok", true);
  cJSON_AddBoolToObject(r, "active", active);
  cJSON_AddBoolToObject(r, "mqttConnected", active && atomic_load(&mqtt_ready));
  cJSON_AddNumberToObject(r, "receivedMessages", atomic_load(&mqtt_messages));
  cJSON_AddNumberToObject(r, "httpStatus", atomic_load(&last_http_status));
  cJSON_AddNumberToObject(r, "transportStatus", atomic_load(&last_transport_status));
  pthread_mutex_unlock(&init_lock);
  return r;
}
#endif
