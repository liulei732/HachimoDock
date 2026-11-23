#include "pet_p4_private_feed.h"

#if PET_P4_SYSTEM_WIFI
#include <stdatomic.h>
#include <stdio.h>
#include "bootloader_random.h"
#include "esp_efuse.h"
#include "esp_hmac.h"
#include "esp_random.h"
#include "mbedtls/gcm.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CREDENTIAL_SLOTS 8
static atomic_uint revision;
static atomic_flag writer = ATOMIC_FLAG_INIT;
static atomic_int local_result;
static struct {
  pet_private_feed_t feed;
  pet_private_secret_t secret;
  bool forget;
} local_request;
static uint8_t boot_nonce[12];
static uint32_t nonce_index;
static bool crypto_ready;
static const char purpose[] = "HachimiDock/PetUI/credentials/v1";

typedef struct {
  uint8_t id[32];
  uint8_t iv[12];
  uint8_t tag[16];
  uint8_t ciphertext[sizeof(pet_private_secret_t)];
} credential_t;

static bool credential_key_block(esp_efuse_block_t *block) {
  return esp_efuse_find_purpose(ESP_EFUSE_KEY_PURPOSE_HMAC_UP, block) &&
    esp_efuse_get_key_dis_read(*block) && esp_efuse_get_key_dis_write(*block) &&
    esp_efuse_get_keypurpose_dis_write(*block);
}

void pet_private_storage_init(void) {
  esp_efuse_block_t block;
  if (!credential_key_block(&block)) return;
  /* At entry to app_main there is no joystick/ADC worker. Sample with the
   * main entropy source before starting any peripheral that may use it. */
  bootloader_random_enable();
  esp_fill_random(boot_nonce, sizeof(boot_nonce));
  bootloader_random_disable();
  crypto_ready = true;
}

bool pet_private_storage_ready(void) {
  return crypto_ready;
}

uint32_t pet_private_storage_epoch(void) {
  return atomic_load(&revision);
}

static bool mqtt_endpoint(const char *url, pet_wifi_endpoint_t *endpoint, uint16_t *port) {
  if (strncmp(url, "mqtts://", 8)) return false;
  const char *authority = url + 8;
  const char *colon = strchr(authority, ':');
  const size_t length = colon ? (size_t)(colon - authority) : strlen(authority);
  if (!length || length >= sizeof(endpoint->host) || strpbrk(authority, "/?#@")) return false;
  unsigned number = 8883;
  if (colon) {
    number = 0;
    const char *digits = colon + 1;
    if (!*digits || strlen(digits) > 5) return false;
    for (; *digits; ++digits) {
      if (*digits < '0' || *digits > '9') return false;
      number = number * 10 + (*digits - '0');
    }
    if (!number || number > 65535) return false;
  }
  char https[sizeof(endpoint->host) + 8];
  snprintf(https, sizeof(https), "https://%.*s", (int)length, authority);
  if (!pet_wifi_endpoint_parse(https, endpoint)) return false;
  *port = number;
  return true;
}

static bool topic_valid(const char *topic) {
  if (!topic || !*topic || strlen(topic) > PET_PRIVATE_TOPIC_MAX ||
      topic[0] == '$' || strpbrk(topic, "+#")) return false;
  for (const unsigned char *p = (const unsigned char *)topic; *p; ++p)
    if (*p < 33 || *p > 126) return false;
  return true;
}

static bool feed_binding(pet_private_feed_t *out, pet_wifi_endpoint_t *endpoint, const char *component,
                 const char *url, pet_private_kind_t kind, const char *topic) {
  if (!component || !*component || strlen(component) > 47 || !url) return false;
  for (const char *p = component; *p; ++p)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || strchr("-_.", *p))) return false;
  *out = (pet_private_feed_t){.kind = kind, .port = 443};
  if (kind == PET_PRIVATE_BEARER || kind == PET_HTTPS_SUBMIT) {
    /* Credentials belong in Authorization. Disallow query strings in this
     * first profile so an exported declaration cannot hide them in a URL. */
    if (topic || strchr(url, '?') || !pet_wifi_endpoint_parse(url, endpoint) ||
        !strncmp(endpoint->path, "//", 2)) return false;
  } else if (kind == PET_PRIVATE_MQTT || kind == PET_PUBLIC_MQTT || kind == PET_MQTT_PUBLISH) {
    if (!topic_valid(topic) || !mqtt_endpoint(url, endpoint, &out->port)) return false;
    strcpy(out->topic, topic);
  } else {
    return false;
  }
  char key[48 + sizeof(endpoint->host) + sizeof(endpoint->path) + sizeof(out->topic) + 24];
  const int length = snprintf(key, sizeof(key), "%u\n%s\n%s\n%u\n%s\n%s",
    (unsigned)kind, component, endpoint->host, out->port, endpoint->path, out->topic);
  return length > 0 && (size_t)length < sizeof(key) &&
    !mbedtls_sha256((const uint8_t *)key, (size_t)length, out->id, 0);
}

#if PET_P4_SCOPED_WRITE
/* A group prefix permits native uninstall to revoke superseded destinations
 * without storing app names in the vault. The remaining 160 bits bind the
 * full destination, method and value and the entire id is authenticated. */
enum { OWNER_BYTES = 12 };
static bool owner_hash(const char *component, uint8_t hash[32]) {
  return component && !mbedtls_sha256((const uint8_t *)component, strlen(component), hash, 0);
}

static bool bind_owner(pet_private_feed_t *feed, const char *component) {
  uint8_t owner[32];
  if (!owner_hash(component, owner)) return false;
  memcpy(feed->id, owner, OWNER_BYTES);
  return true;
}
#endif

bool pet_private_feed_parse(pet_private_feed_t *out, pet_wifi_endpoint_t *endpoint,
                            const char *component, const char *url, const cJSON *connection) {
  if (!cJSON_IsObject(connection)) return false;
  const cJSON *kind = cJSON_GetObjectItemCaseSensitive(connection, "type");
  const cJSON *topic = cJSON_GetObjectItemCaseSensitive(connection, "topic");
  if (!cJSON_IsString(kind)) return false;
  const int fields = cJSON_GetArraySize(connection);
#if PET_P4_SCOPED_WRITE
  const bool post = !strcmp(kind->valuestring, "https-submit");
  if (post || !strcmp(kind->valuestring, "mqtt-publish")) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(connection, "value");
    if (fields != (post ? 2 : 3) || !cJSON_IsString(value) || (!post && !cJSON_IsString(topic))) return false;
    const char *v = value->valuestring;
    if (!*v || strlen(v) >= sizeof(out->value) ||
        strspn(v, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != strlen(v) ||
        !feed_binding(out, endpoint, component, url, post ? PET_HTTPS_SUBMIT : PET_MQTT_PUBLISH,
                      post ? NULL : topic->valuestring)) return false;
    const char *target = post ? endpoint->path : out->topic;
    if (strlen(endpoint->host) > 32 || strlen(target) > 40) return false;
    strcpy(out->value, v);
    uint8_t scope[32 + sizeof(out->value)];
    memcpy(scope, out->id, sizeof(out->id));
    memcpy(scope + 32, v, strlen(v));
    return !mbedtls_sha256(scope, 32 + strlen(v), out->id, 0) && bind_owner(out, component);
  }
#endif
  if (fields == 1 && !strcmp(kind->valuestring, "bearer"))
    return feed_binding(out, endpoint, component, url, PET_PRIVATE_BEARER, NULL);
  if (fields == 2 && !strcmp(kind->valuestring, "mqtt-public") && cJSON_IsString(topic))
    return feed_binding(out, endpoint, component, url, PET_PUBLIC_MQTT, topic->valuestring);
  if (fields == 2 && !strcmp(kind->valuestring, "mqtt") && cJSON_IsString(topic))
    return feed_binding(out, endpoint, component, url, PET_PRIVATE_MQTT, topic->valuestring);
  return false;
}

static bool secret_valid(const pet_private_feed_t *feed, const pet_private_secret_t *secret) {
  if (feed->kind == PET_PRIVATE_BEARER || feed->kind == PET_HTTPS_SUBMIT) {
    if (secret->user[0] || !secret->token[0]) return false;
    for (const char *p = secret->token; *p; ++p)
      if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || strchr("-._~+/=", *p))) return false;
  } else if (feed->kind == PET_MQTT_PUBLISH && (!secret->user[0] || !secret->token[0])) return false;
  else if (secret->token[0] && !secret->user[0]) return false;
  return true;
}

static bool setup_key(mbedtls_gcm_context *gcm, const char *label, size_t length) {
  esp_efuse_block_t block;
  uint8_t key[32];
  const bool ok = credential_key_block(&block) &&
    esp_hmac_calculate((hmac_key_id_t)(block - EFUSE_BLK_KEY0), label, length, key) == ESP_OK &&
    mbedtls_gcm_setkey(gcm, MBEDTLS_CIPHER_ID_AES, key, 256) == 0;
  mbedtls_platform_zeroize(key, sizeof(key));
  return ok;
}

enum { READ_GRANT, SAVE_GRANT, REVOKE_GRANT, CLAIM_GRANT, ACK_GRANT };

/* All nonce allocation uses the same writer guard as feed mutations. */
static bool next_iv(uint8_t iv[12]) {
  if (nonce_index == UINT32_MAX) return false;
  memcpy(iv, boot_nonce, sizeof(boot_nonce));
  const uint32_t index = nonce_index++;
  for (unsigned i = 0; i < 4; ++i)
    iv[8 + i] ^= (uint8_t)(index >> (24 - 8 * i));
  return true;
}

#if PET_P4_COMPONENT_NETWORK
bool pet_private_network_crypt(bool seal, const void *binding, size_t binding_size,
                               uint8_t iv[12], uint8_t tag[16], const uint8_t *input,
                               uint8_t *output, size_t size) {
  static const char domain[] = "HachimiDock/PetUI/component-network/v1";
  if (!crypto_ready || !binding || !binding_size || !input || !output || !size ||
      atomic_flag_test_and_set(&writer)) return false;
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  bool ok = setup_key(&gcm, domain, sizeof(domain));
  if (ok && seal)
    ok = next_iv(iv) && !mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, size,
      iv, 12, binding, binding_size, input, output, 16, tag);
  else if (ok)
    ok = !mbedtls_gcm_auth_decrypt(&gcm, size, iv, 12, binding, binding_size,
                                    tag, 16, input, output);
  mbedtls_gcm_free(&gcm);
  if (!ok) mbedtls_platform_zeroize(output, size);
  atomic_flag_clear(&writer);
  return ok;
}
#endif

static bool seal_record(nvs_handle_t nvs, const char *slot, mbedtls_gcm_context *gcm,
                        credential_t *record, const pet_private_secret_t *plain) {
  if (!next_iv(record->iv)) return false;
  return !mbedtls_gcm_crypt_and_tag(gcm, MBEDTLS_GCM_ENCRYPT, sizeof(*plain),
      record->iv, sizeof(record->iv), record->id, sizeof(record->id), (const uint8_t *)plain,
      record->ciphertext, sizeof(record->tag), record->tag) &&
      nvs_set_blob(nvs, slot, record, sizeof(*record)) == ESP_OK && nvs_commit(nvs) == ESP_OK;
}

/* Writes replace one authenticated NVS blob. A write grant with empty token
 * and internal state byte is a spent grant. Such records cannot be provisioned
 * by an app, the keyboard, or the LAN credential protocol. */
static pet_private_result_t credential(const pet_private_feed_t *feed, uint8_t op,
                                       pet_private_secret_t *secret) {
  if (!pet_private_storage_ready()) return PET_PRIVATE_NO_STORAGE;
  nvs_handle_t nvs;
  esp_err_t open = nvs_open("pet_credentials", op == READ_GRANT ? NVS_READONLY : NVS_READWRITE, &nvs);
  if (open == ESP_ERR_NVS_NOT_FOUND) return PET_PRIVATE_ABSENT;
  if (open != ESP_OK) return PET_PRIVATE_STORAGE_ERROR;
  credential_t record;
  pet_private_secret_t plain = {0};
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);
  int found = -1, free_slot = -1;
  char slot[] = "feed0";
  pet_private_result_t result = PET_PRIVATE_STORAGE_ERROR;
  for (int i = 0; i < CREDENTIAL_SLOTS; ++i) {
    slot[4] = '0' + i;
    size_t length = sizeof(record);
    esp_err_t status = nvs_get_blob(nvs, slot, &record, &length);
    if (status == ESP_ERR_NVS_NOT_FOUND) {
      if (free_slot < 0) free_slot = i;
    } else if (status != ESP_OK || length != sizeof(record)) {
      goto done;
    } else if (!memcmp(record.id, feed->id, sizeof(record.id))) {
      found = i;
      break;
    }
  }
  if (found < 0 && op != SAVE_GRANT) { result = PET_PRIVATE_ABSENT; goto done; }
  if (op == REVOKE_GRANT) {
    if (nvs_erase_key(nvs, slot) == ESP_OK && nvs_commit(nvs) == ESP_OK) {
      atomic_fetch_add(&revision, 1);
      result = PET_PRIVATE_ABSENT;
    }
    goto done;
  }
  if ((found < 0 && free_slot < 0) || !setup_key(&gcm, purpose, sizeof(purpose))) goto done;
  /* Old read-only credentials remain replaceable with the original wire command. */
  if (found >= 0 && (op != SAVE_GRANT || pet_feed_is_write(feed->kind))) {
    if (mbedtls_gcm_auth_decrypt(&gcm, sizeof(plain), record.iv, sizeof(record.iv), record.id,
        sizeof(record.id), record.tag, sizeof(record.tag), record.ciphertext, (uint8_t *)&plain) ||
        plain.user[PET_PRIVATE_USER_MAX] || plain.token[PET_PRIVATE_TOKEN_MAX]) goto done;
#if PET_P4_SCOPED_WRITE
    if (pet_feed_is_write(feed->kind) && !plain.token[0]) {
      if (plain.user[0] == 1) {
        result = PET_WRITE_UNKNOWN;
        if (op == ACK_GRANT) {
          plain.user[0] = 2;
          if (seal_record(nvs, slot, &gcm, &record, &plain)) result = PET_WRITE_ACCEPTED;
        }
        goto done;
      }
      if (plain.user[0] != 2) goto done;
      if (op != SAVE_GRANT) { result = PET_WRITE_SPENT; goto done; }
    } else
#endif
    if (!secret_valid(feed, &plain)) goto done;
  }
  if (op == SAVE_GRANT) {
    slot[4] = '0' + (found < 0 ? free_slot : found);
    memcpy(record.id, feed->id, sizeof(record.id));
    if (seal_record(nvs, slot, &gcm, &record, secret)) {
      atomic_fetch_add(&revision, 1);
      result = PET_PRIVATE_SET;
    }
  } else if (op == READ_GRANT || op == CLAIM_GRANT) {
    *secret = plain;
    result = PET_PRIVATE_SET;
#if PET_P4_SCOPED_WRITE
    if (op == CLAIM_GRANT) {
      memset(&plain, 0, sizeof(plain));
      plain.user[0] = 1;
      if (!seal_record(nvs, slot, &gcm, &record, &plain)) result = PET_PRIVATE_STORAGE_ERROR;
    }
#endif
  }
done:
  mbedtls_gcm_free(&gcm);
  nvs_close(nvs);
  if ((op == READ_GRANT || op == CLAIM_GRANT) && result != PET_PRIVATE_SET)
    mbedtls_platform_zeroize(secret, sizeof(*secret));
  mbedtls_platform_zeroize(&plain, sizeof(plain));
  mbedtls_platform_zeroize(&record, sizeof(record));
  return result;
}

static pet_private_result_t write_credential(const pet_private_feed_t *feed, uint8_t op,
                                              pet_private_secret_t *secret) {
  if (atomic_flag_test_and_set(&writer)) return PET_PRIVATE_BUSY;
  pet_private_result_t result = credential(feed, op, secret);
  atomic_flag_clear(&writer);
  return result;
}

#if PET_P4_SCOPED_WRITE
bool pet_private_retire_component(const char *component) {
  uint8_t owner[32];
  if (!owner_hash(component, owner) || atomic_flag_test_and_set(&writer)) return false;
  nvs_handle_t nvs;
  esp_err_t status = nvs_open("pet_credentials", NVS_READONLY, &nvs);
  bool ok = status == ESP_ERR_NVS_NOT_FOUND;
  if (status == ESP_OK) {
    ok = true;
    unsigned matches = 0;
    char slot[] = "feed0";
    credential_t record;
    for (int i = 0; i < CREDENTIAL_SLOTS && ok; ++i) {
      slot[4] = '0' + i;
      size_t n = sizeof(record);
      status = nvs_get_blob(nvs, slot, &record, &n);
      if (status == ESP_ERR_NVS_NOT_FOUND) continue;
      ok = status == ESP_OK && n == sizeof(record);
      if (ok && !memcmp(owner, record.id, OWNER_BYTES)) matches |= 1U << i;
    }
    nvs_close(nvs);
    if (ok && matches) {
      ok = nvs_open("pet_credentials", NVS_READWRITE, &nvs) == ESP_OK;
      if (ok) {
        for (int i = 0; i < CREDENTIAL_SLOTS && ok; ++i) {
          if (!(matches & (1U << i))) continue;
          slot[4] = '0' + i;
          ok = nvs_erase_key(nvs, slot) == ESP_OK;
        }
        ok = ok && nvs_commit(nvs) == ESP_OK;
        nvs_close(nvs);
      }
    }
  }
  /* Invalidate an approval sampled before retirement even if it had not yet
   * acquired a credential slot. */
  atomic_fetch_add(&revision, 1);
  atomic_flag_clear(&writer);
  return ok;
}

bool pet_write_guard(uint32_t epoch) {
  if (atomic_flag_test_and_set(&writer)) return false;
  if (pet_private_storage_epoch() == epoch) return true;
  atomic_flag_clear(&writer);
  return false;
}

void pet_write_unlock(void) { atomic_flag_clear(&writer); }


pet_private_result_t pet_write_claim(const pet_private_feed_t *feed, uint32_t epoch,
                                     pet_private_secret_t *secret) {
  if (!pet_feed_is_write(feed->kind) || !pet_write_guard(epoch)) return PET_PRIVATE_REFUSED;
  pet_private_result_t result = credential(feed, CLAIM_GRANT, secret);
  pet_write_unlock();
  return result;
}

bool pet_write_ack(const pet_private_feed_t *feed, uint32_t epoch) {
  if (!pet_write_guard(epoch)) return false;
  bool saved = credential(feed, ACK_GRANT, NULL) == PET_WRITE_ACCEPTED;
  pet_write_unlock();
  return saved;
}
#endif

static void credential_worker(void *unused) {
  (void)unused;
  pet_private_result_t result = credential(&local_request.feed, local_request.forget ? REVOKE_GRANT : SAVE_GRANT,
                                             &local_request.secret);
  mbedtls_platform_zeroize(&local_request, sizeof(local_request));
  atomic_flag_clear(&writer);
  atomic_store(&local_result, result == PET_PRIVATE_BUSY ? PET_PRIVATE_STORAGE_ERROR : result);
  vTaskDelete(NULL);
}

bool pet_private_secret_schedule_at(const pet_private_feed_t *feed, const pet_private_secret_t *secret,
                                    bool forget, uint32_t epoch) {
  if (!pet_private_storage_ready() || (!forget && !secret_valid(feed, secret)) ||
      atomic_load(&local_result) == PET_PRIVATE_BUSY || atomic_flag_test_and_set(&writer)) return false;
  if (epoch != pet_private_storage_epoch()) { atomic_flag_clear(&writer); return false; }
  atomic_fetch_add(&revision, 1);
  local_request.feed = *feed;
  local_request.secret = *secret;
  local_request.forget = forget;
  atomic_store(&local_result, PET_PRIVATE_BUSY);
  if (xTaskCreate(credential_worker, "pet_feed_cred", 4096, NULL, 2, NULL) == pdPASS) return true;
  mbedtls_platform_zeroize(&local_request, sizeof(local_request));
  atomic_flag_clear(&writer);
  atomic_store(&local_result, PET_PRIVATE_STORAGE_ERROR);
  return false;
}

pet_private_result_t pet_private_local_result(void) {
  return atomic_load(&local_result);
}

pet_private_result_t pet_private_secret_read(const pet_private_feed_t *feed,
                                              pet_private_secret_t *out) {
  return credential(feed, 0, out);
}

static bool take_text(const uint8_t **at, size_t *remaining, char *out, size_t capacity, size_t n) {
  if (n >= capacity || n > *remaining) return false;
  for (size_t i = 0; i < n; ++i) if ((*at)[i] < 32 || (*at)[i] > 126) return false;
  memcpy(out, *at, n);
  out[n] = 0;
  *at += n;
  *remaining -= n;
  return true;
}

pet_private_result_t pet_private_credentials_request(const uint8_t *at, size_t remaining) {
  if (remaining < 9 || at[0] > 2) return PET_PRIVATE_REFUSED;
  const uint8_t command = at[0];
  const pet_private_kind_t kind = at[1];
  if (kind != PET_PRIVATE_BEARER && kind != PET_PRIVATE_MQTT) return PET_PRIVATE_REFUSED;
  const unsigned id = at[2], url = at[3] * 256u + at[4], topic = at[5];
  const unsigned user = at[6], token = at[7] * 256u + at[8];
  if ((command != 1 && (user || token)) || (kind == PET_PRIVATE_BEARER && topic))
    return PET_PRIVATE_REFUSED;
  at += 9;
  remaining -= 9;
  char component[48], address[392], filter[PET_PRIVATE_TOPIC_MAX + 1];
  pet_private_secret_t secret = {0};
  pet_wifi_endpoint_t endpoint;
  pet_private_feed_t feed;
  pet_private_result_t result = PET_PRIVATE_REFUSED;
  if (take_text(&at, &remaining, component, sizeof(component), id) &&
      take_text(&at, &remaining, address, sizeof(address), url) &&
      take_text(&at, &remaining, filter, sizeof(filter), topic) &&
      take_text(&at, &remaining, secret.user, sizeof(secret.user), user) &&
      take_text(&at, &remaining, secret.token, sizeof(secret.token), token) && !remaining &&
      feed_binding(&feed, &endpoint, component, address, kind, topic ? filter : NULL) &&
      (command != 1 || secret_valid(&feed, &secret))) {
    result = command ? write_credential(&feed, command, &secret) : credential(&feed, 0, &secret);
  }
  mbedtls_platform_zeroize(&secret, sizeof(secret));
  return result;
}
#endif
