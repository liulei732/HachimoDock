#pragma once

#if PET_P4_SYSTEM_WIFI
#include "pet_p4_wifi_endpoint.h"
#include "cJSON.h"

#define PET_PRIVATE_FEED_CAPABILITY "p4-private-feed-v1"
#define PET_PRIVATE_FEED_REQUEST_MAX 960
#define PET_PRIVATE_TOPIC_MAX 127
#define PET_PRIVATE_USER_MAX 64
#define PET_PRIVATE_TOKEN_MAX 256

typedef enum {
  PET_PRIVATE_NONE,
  PET_PRIVATE_BEARER,
  PET_PRIVATE_MQTT,
  PET_PUBLIC_MQTT,
  PET_HTTPS_SUBMIT,
  PET_MQTT_PUBLISH,
} pet_private_kind_t;

static inline bool pet_feed_is_write(pet_private_kind_t kind) {
#if PET_P4_SCOPED_WRITE
  return kind == PET_HTTPS_SUBMIT || kind == PET_MQTT_PUBLISH;
#else
  (void)kind;
  return false;
#endif
}

static inline bool pet_feed_requires_account(pet_private_kind_t kind) {
  return kind == PET_PRIVATE_BEARER || kind == PET_PRIVATE_MQTT || pet_feed_is_write(kind);
}

typedef struct {
  pet_private_kind_t kind;
  uint16_t port;
  char topic[PET_PRIVATE_TOPIC_MAX + 1];
#if PET_P4_SCOPED_WRITE
  char value[25];
#endif
  uint8_t id[32];
} pet_private_feed_t;

typedef struct {
  char user[PET_PRIVATE_USER_MAX + 1];
  char token[PET_PRIVATE_TOKEN_MAX + 1];
} pet_private_secret_t;

typedef enum {
  PET_PRIVATE_REFUSED,
  PET_PRIVATE_SET,
  PET_PRIVATE_ABSENT,
  PET_PRIVATE_NO_STORAGE,
  PET_PRIVATE_STORAGE_ERROR,
  PET_PRIVATE_BUSY,
  PET_WRITE_UNKNOWN,
  PET_WRITE_ACCEPTED,
  PET_WRITE_SPENT,
} pet_private_result_t;

bool pet_private_feed_parse(pet_private_feed_t *out, pet_wifi_endpoint_t *endpoint,
                            const char *component, const char *url, const cJSON *connection);
void pet_private_storage_init(void);
bool pet_private_storage_ready(void);
#if PET_P4_COMPONENT_NETWORK
/* Native component-network vault only. The caller supplies the complete binding
 * as authenticated data and distinct input/output buffers. */
bool pet_private_network_crypt(bool seal, const void *binding, size_t binding_size,
                               uint8_t iv[12], uint8_t tag[16], const uint8_t *input,
                               uint8_t *output, size_t size);
#endif
uint32_t pet_private_storage_epoch(void);
bool pet_private_secret_schedule_at(const pet_private_feed_t *binding,
                                    const pet_private_secret_t *secret, bool forget, uint32_t epoch);
static inline bool pet_private_secret_schedule(const pet_private_feed_t *binding,
                                               const pet_private_secret_t *secret, bool forget) {
  return pet_private_secret_schedule_at(binding, secret, forget, pet_private_storage_epoch());
}
pet_private_result_t pet_private_local_result(void);
pet_private_result_t pet_private_secret_read(const pet_private_feed_t *binding,
                                              pet_private_secret_t *out);
#if PET_P4_SCOPED_WRITE
bool pet_private_retire_component(const char *component);
bool pet_write_guard(uint32_t epoch);
void pet_write_unlock(void);
pet_private_result_t pet_write_claim(const pet_private_feed_t *, uint32_t, pet_private_secret_t *);
bool pet_write_ack(const pet_private_feed_t *, uint32_t);
#endif
/* Called only on the authenticated LAN worker. The request contains a boot
 * prefix that the caller must verify before handing off the remainder. */
pet_private_result_t pet_private_credentials_request(const uint8_t *request, size_t size);
#endif
