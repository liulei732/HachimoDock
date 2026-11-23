#include "pet_component_network.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *policy =
    "{\"profile\":\"component-network-v1\",\"https\":[{\"origin\":\"https://"
    "api.example.org\",\"methods\":[\"GET\",\"POST\"]}],\"mqtts\":[{\"host\":\"events.example."
    "org\",\"port\":8883,\"subscribe\":[\"sensors/+/report\"],\"publish\":[\"sensors/+/"
    "request\"]}],\"credentials\":{\"records\":8,\"totalBytes\":8192}}";
static int submit(pet_net_lease_t *lease, const char *json) {
  return pet_net_call(lease, 1, json, strlen(json), NULL, 0);
}
static uint32_t poll(pet_net_lease_t *lease, int request, int status) {
  char output[512];
  int n = pet_net_call(lease, 2, NULL, 0, output, sizeof output);
  assert(n > 0);
  cJSON *v = cJSON_Parse(output);
  assert(v);
  assert(cJSON_GetObjectItem(v, "request")->valueint == request);
  assert(cJSON_GetObjectItem(v, "status")->valueint == status);
  uint32_t handle = cJSON_GetObjectItem(v, "handle")->valueint;
  cJSON_Delete(v);
  return handle;
}
int main(int argc, char **argv) {
  if (argc == 2) {
    FILE *f = fopen(argv[1], "rb");
    assert(f);
    char raw[16384];
    size_t n = fread(raw, 1, sizeof(raw) - 1, f);
    fclose(f);
    raw[n] = 0;
    cJSON *cases = cJSON_Parse(raw), *v;
    assert(cases);
    pet_net_policy_t *p = calloc(1, sizeof *p);
    cJSON_ArrayForEach(v, cases) assert(pet_net_policy_parse(cJSON_GetObjectItem(v, "policy"), p) ==
                                        cJSON_IsTrue(cJSON_GetObjectItem(v, "valid")));
    free(p);
    cJSON_Delete(cases);
  }

  assert(pet_net_topic_contains("sensors/+/report", "sensors/room/report", true));
  assert(pet_net_topic_contains("a/#", "a", true));
  assert(!pet_net_topic_contains("sensors/+/report", "sensors/#", true));
  assert(!pet_net_topic_contains("sensors/room/report", "sensors/+/report", true));
  assert(!pet_net_topic_contains("#", "$SYS/status", true));
  assert(!pet_net_topic_contains("a/+", "a/#", true));
  assert(!pet_net_topic_contains("a/#", "a/x/#/bad", true));
  assert(!pet_net_topic_contains("a/#", "a/+", false));
  assert(pet_net_public_ipv4(0x08080808));
  const uint32_t private_ips[] = {0,          0x0a000001, 0x7f000001, 0xc0a80101,
                                  0xac100001, 0x64400001, 0xa9fe0001, 0xc0000201,
                                  0xc6336401, 0xcb007101, 0xe0000001};
  for (unsigned i = 0; i < sizeof private_ips / sizeof *private_ips; i++)
    assert(!pet_net_public_ipv4(private_ips[i]));
  cJSON *p = cJSON_Parse(policy);
  pet_net_policy_t *parsed = calloc(1, sizeof *parsed);
  assert(pet_net_policy_parse(p, parsed));
  cJSON *bad = cJSON_Parse("{\"profile\":\"component-network-v1\",\"profile\":\"component-network-"
                           "v1\",\"credentials\":{\"records\":8,\"totalBytes\":8192}}");
  assert(!pet_net_policy_parse(bad, parsed));
  cJSON_Delete(bad);
  free(parsed);
  pet_net_host_t *h = calloc(1, sizeof *h);
  pet_net_init(h);
  pet_net_lease_t lease = {h, pet_net_activate(h, "sensor-panel", p)};
  assert(lease.generation);
  const char *denied[] = {
      "{\"op\":\"http\",\"origin\":\"https://evil.example.org\",\"path\":\"/\",\"method\":\"GET\"}",
      "{\"op\":\"http\",\"origin\":\"https://api.example.org\",\"path\":\"//"
      "evil.example.org\",\"method\":\"GET\"}",
      "{\"op\":\"http\",\"origin\":\"https://api.example.org\",\"path\":\"/"
      "\",\"method\":\"GET\",\"headers\":{\"Host\":\"evil\"}}",
      "{\"op\":\"mqtt.subscribe\",\"host\":\"events.example.org\",\"topic\":\"sensors/#\"}",
      "{\"op\":\"mqtt.publish\",\"host\":\"events.example.org\",\"topic\":\"sensors/+/"
      "request\",\"payload\":\"x\"}",
      "{\"op\":\"credential.get\",\"key\":\"token\",\"owner\":\"other\"}",
      "{\"op\":\"credential.get\",\"op\":\"credential.erase\",\"key\":\"token\"}"};
  for (unsigned i = 0; i < sizeof denied / sizeof *denied; i++)
    assert(submit(&lease, denied[i]) == PET_NET_DENIED);
  assert(submit(&lease, "{\"op\":\"credential.get\",\"key\":\"x\\u0000hidden\"}") ==
         PET_NET_INVALID);
  assert(submit(&lease, "{\"op\":\"http\",\"body\":[[[[[[[[[[]]]]]]]]]]}") == PET_NET_INVALID);
  int id = submit(&lease, "{\"op\":\"http\",\"origin\":\"https://api.example.org\",\"path\":\"/"
                          "items\",\"method\":\"GET\"}");
  assert(id > 0);
  uint32_t zero_handle = 0;
  assert(pet_net_call(&lease, 4, &zero_handle, 4, NULL, 0) == PET_NET_STALE);
  pet_net_job_t *job = calloc(1, sizeof *job);
  assert(pet_net_take(h, job));
  assert(job->id == (unsigned)id && !strcmp(job->owner, "sensor-panel"));
  unsigned char *data = malloc(6);
  memcpy(data, "hello", 6);
  assert(pet_net_complete(h, lease.generation, id, "http", 200, NULL, data, 5));
  char tiny[3];
  assert(pet_net_call(&lease, 2, NULL, 0, tiny, sizeof tiny) == PET_NET_LIMIT);
  uint32_t handle = poll(&lease, id, 200), input[] = {handle, 1};
  char buf[12] = {0};
  assert(pet_net_call(&lease, 3, input, 8, buf, 2) == 2 && !memcmp(buf, "el", 2));
  input[1] = UINT32_MAX;
  assert(pet_net_call(&lease, 3, input, 8, buf, sizeof buf) == PET_NET_INVALID);
  assert(pet_net_call(&lease, 4, &handle, 4, NULL, 0) == 0);
  assert(pet_net_call(&lease, 3, input, 8, buf, 1) == PET_NET_STALE);
  pet_net_lease_t old = lease;
  lease.generation = pet_net_activate(h, "second-panel", p);
  assert(lease.generation != old.generation);
  assert(submit(&old, "{\"op\":\"credential.get\",\"key\":\"token\"}") == PET_NET_STALE);
  data = malloc(1);
  *data = 9;
  assert(!pet_net_complete(h, old.generation, id, "http", 200, NULL, data, 1));
  pet_net_job_dispose(job);
  free(job);
  for (unsigned i = 0; i < 4; i++)
    assert(submit(&lease, "{\"op\":\"credential.get\",\"key\":\"token\"}") > 0);
  assert(submit(&lease, "{\"op\":\"credential.get\",\"key\":\"token\"}") == PET_NET_BUSY);
  lease.generation = pet_net_activate(h, "second-panel", p);
  for (unsigned i = 0; i < 8; i++)
    assert(
        pet_net_complete(h, lease.generation, 0, "mqtt.message", 0, "sensors/a/report", NULL, 0));
  assert(!pet_net_complete(h, lease.generation, 0, "mqtt.message", 0, "sensors/a/report", NULL, 0));
  char event[512];
  assert(pet_net_call(&lease, 2, NULL, 0, event, 512) > 0 && strstr(event, "overflow"));
  lease.generation = pet_net_activate(h, "second-panel", p);
  assert(pet_net_complete(h, lease.generation, 101, "http", 200, NULL, NULL, 0));
  assert(pet_net_complete(h, lease.generation, 102, "http", 200, NULL, NULL, 0));
  uint32_t first = poll(&lease, 101, 200);
  assert(pet_net_call(&lease, 4, &first, 4, NULL, 0) == 0);
  assert(pet_net_complete(h, lease.generation, 103, "http", 200, NULL, NULL, 0));
  poll(&lease, 102, 200);
  poll(&lease, 103, 200);
  lease.generation = pet_net_activate(h, "second-panel", p);
  assert(pet_net_complete(h, lease.generation, 0, "http", 200, NULL,
                          calloc(1, PET_NETWORK_RESPONSE_BYTES), PET_NETWORK_RESPONSE_BYTES));
  assert(pet_net_complete(h, lease.generation, 0, "mqtt.message", 0, NULL,
                          calloc(1, PET_NETWORK_MESSAGE_BYTES), PET_NETWORK_MESSAGE_BYTES));
  assert(h->retained == PET_NETWORK_TOTAL_BYTES);
  assert(!pet_net_complete(h, lease.generation, 0, "mqtt.message", 0, NULL, calloc(1, 1), 1));
  pet_net_activate(h, NULL, NULL);
  assert(pet_net_call(&lease, 2, NULL, 0, event, 512) == PET_NET_STALE);
  pet_net_destroy(h);
  free(h);
  cJSON_Delete(p);
  puts("network policy, ownership, stale completion, queues, chunk reads and correlation passed");
}
