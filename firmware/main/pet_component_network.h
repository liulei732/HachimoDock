#ifndef PET_COMPONENT_NETWORK_H
#define PET_COMPONENT_NETWORK_H
#include "cJSON.h"
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define PET_NETWORK_PROFILE "component-network-v1"
#define PET_NETWORK_POLICY_BYTES 2048
#define PET_NETWORK_REQUEST_BYTES 8192
#define PET_NETWORK_RESPONSE_BYTES (192u * 1024u)
#define PET_NETWORK_MESSAGE_BYTES (64u * 1024u)
#define PET_NETWORK_SLOTS 8
#define PET_NETWORK_QUEUE 4
#define PET_NETWORK_TOTAL_BYTES (256u * 1024u)
enum { PET_SERVICE_SUBMIT = 1, PET_SERVICE_POLL, PET_SERVICE_READ, PET_SERVICE_RELEASE };
enum {
  PET_NET_UNAVAILABLE = -1,
  PET_NET_DENIED = -2,
  PET_NET_INVALID = -3,
  PET_NET_BUSY = -4,
  PET_NET_LIMIT = -5,
  PET_NET_STALE = -6,
  PET_NET_TRANSPORT = -7,
  PET_NET_STORAGE = -8
};
typedef struct {
  char host[96];
  unsigned methods;
} pet_net_origin_t;
typedef struct {
  char host[96];
  char sub[8][129], pub[8][129];
  unsigned subs, pubs;
} pet_net_broker_t;
typedef struct {
  pet_net_origin_t https[4];
  pet_net_broker_t mqtt[2];
  unsigned origins, brokers, records, credential_bytes;
} pet_net_policy_t;
typedef struct {
  uint32_t generation, id;
  char owner[48];
  pet_net_policy_t policy;
  char *request;
} pet_net_job_t;
typedef struct {
  uint32_t generation, id, request_id;
  int status;
  char type[24], topic[129];
  uint8_t *data;
  size_t bytes;
  bool pending;
} pet_net_result_t;
typedef struct {
  pthread_mutex_t mutex;
  uint32_t generation, next_id;
  bool active, overflow;
  char owner[48];
  pet_net_policy_t policy;
  pet_net_job_t jobs[PET_NETWORK_QUEUE];
  unsigned queued;
  pet_net_result_t results[PET_NETWORK_SLOTS];
  size_t retained;
} pet_net_host_t;
typedef struct {
  pet_net_host_t *host;
  uint32_t generation;
} pet_net_lease_t;
bool pet_net_policy_parse(const cJSON *, pet_net_policy_t *);
/* Resolve a credential destination at provision time, never at first use. */
bool pet_net_credential_target(const pet_net_policy_t *, const char *target, char out[104]);
bool pet_net_topic_contains(const char *, const char *, bool);
bool pet_net_public_ipv4(uint32_t host_order);
void pet_net_init(pet_net_host_t *);
void pet_net_destroy(pet_net_host_t *);
uint32_t pet_net_activate(pet_net_host_t *, const char *owner, const cJSON *policy);
int pet_net_call(void *lease, unsigned op, const void *input, unsigned bytes, void *output,
                 unsigned capacity);
bool pet_net_take(pet_net_host_t *, pet_net_job_t *);
void pet_net_request_delete(cJSON *);
void pet_net_job_dispose(pet_net_job_t *);
/* Takes ownership of data on success and failure. No payload is ever logged. */
bool pet_net_complete(pet_net_host_t *, uint32_t generation, uint32_t id, const char *type,
                      int status, const char *topic, uint8_t *data, size_t bytes);
uint32_t pet_net_generation(pet_net_host_t *);
void pet_net_zero_free(void *, size_t);
void *pet_net_alloc(size_t);
#endif
