#include "pet_component_network.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif
void *pet_net_alloc(size_t n) {
#ifdef ESP_PLATFORM
  return heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  return malloc(n ? n : 1);
#endif
}
void pet_net_zero_free(void *p, size_t n) {
  if (p) {
    volatile unsigned char *v = p;
    while (n--)
      *v++ = 0;
    free(p);
  }
}
static const char *str(const cJSON *o, const char *key) {
  return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, key));
}
static bool text(const char *s, size_t max, bool empty) {
  if (!s || (!empty && !*s) || strlen(s) > max)
    return false;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++)
    if (*p < 32 || *p == 127)
      return false;
  return true;
}
static bool keys(const cJSON *o, const char *allowed) {
  if (!cJSON_IsObject(o))
    return false;
  for (const cJSON *p = o->child; p; p = p->next) {
    if (!p->string)
      return false;
    for (const cJSON *q = p->next; q; q = q->next)
      if (q->string && !strcmp(p->string, q->string))
        return false;
    const char *at = allowed;
    bool found = false;
    size_t n = strlen(p->string);
    while (*at) {
      const char *end = strchr(at, '|');
      size_t len = end ? (size_t)(end - at) : strlen(at);
      if (len == n && !memcmp(at, p->string, n))
        found = true;
      if (!end)
        break;
      at = end + 1;
    }
    if (!found)
      return false;
  }
  return true;
}
static bool domain(const char *s) {
  if (!text(s, 95, false) || !strchr(s, '.'))
    return false;
  size_t label = 0;
  bool letters = false;
  unsigned char prior = 0;
  for (const unsigned char *p = (const unsigned char *)s;; p++) {
    unsigned char c = *p;
    if (!c || c == '.') {
      if (!label || label > 63 || prior == '-')
        return false;
      label = 0;
      if (!c)
        break;
    } else {
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') || (!label && c == '-'))
        return false;
      label++;
      if (c >= 'a' && c <= 'z')
        letters = true;
    }
    prior = c;
  }
  return letters;
}
static bool token(const char *s) {
  if (!text(s, 32, false))
    return false;
  for (; *s; s++)
    if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
          *s == '_' || *s == '-' || *s == '.'))
      return false;
  return true;
}
static bool filter(const char *s, bool wild) {
  if (!text(s, 128, false))
    return false;
  for (const char *p = s; *p; p++)
    if (*p == '+' || *p == '#') {
      if (!wild || (p != s && p[-1] != '/') || (p[1] && p[1] != '/') || (*p == '#' && p[1]))
        return false;
    }
  return true;
}
bool pet_net_topic_contains(const char *allowed, const char *request, bool subscribe) {
  if (!filter(allowed, true) || !filter(request, subscribe))
    return false;
  if (request[0] == '$' && (allowed[0] == '+' || allowed[0] == '#'))
    return false;
  const char *a = allowed, *r = request;
  for (;;) {
    const char *ae = strchr(a, '/'), *re = strchr(r, '/');
    size_t an = ae ? (size_t)(ae - a) : strlen(a), rn = re ? (size_t)(re - r) : strlen(r);
    if (an == 1 && *a == '#')
      return true;
    if (rn == 1 && *r == '#')
      return false;
    if (!(an == 1 && *a == '+') && (an != rn || memcmp(a, r, an)))
      return false;
    if (!ae || !re) {
      if (!ae && !re)
        return true;
      return !re && ae && !strcmp(ae + 1, "#");
    }
    a = ae + 1;
    r = re + 1;
  }
}
bool pet_net_public_ipv4(uint32_t ip) {
  unsigned a = ip >> 24, b = (ip >> 16) & 255, c = (ip >> 8) & 255;
  return !(a == 0 || a == 10 || a == 127 || a >= 224 || (a == 100 && b >= 64 && b <= 127) ||
           (a == 169 && b == 254) || (a == 172 && b >= 16 && b <= 31) ||
           (a == 192 && (b == 168 || (b == 0 && (c == 0 || c == 2)))) ||
           (a == 198 && (b == 18 || b == 19 || (b == 51 && c == 100))) ||
           (a == 203 && b == 0 && c == 113));
}
static bool number(const cJSON *o, const char *key, unsigned low, unsigned high, unsigned *out) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  if (!cJSON_IsNumber(v) || v->valuedouble < low || v->valuedouble > high ||
      (unsigned)v->valuedouble != v->valuedouble)
    return false;
  *out = (unsigned)v->valuedouble;
  return true;
}
bool pet_net_policy_parse(const cJSON *root, pet_net_policy_t *out) {
  pet_net_policy_t *p = out;
  memset(p, 0, sizeof *p);
  if (!keys(root, "profile|https|mqtts|credentials") || !str(root, "profile") ||
      strcmp(str(root, "profile"), PET_NETWORK_PROFILE))
    return false;
  const cJSON *list = cJSON_GetObjectItemCaseSensitive(root, "https"), *v, *item;
  if (list && !cJSON_IsArray(list))
    return false;
  cJSON_ArrayForEach(v, list) {
    if (p->origins == 4 || !keys(v, "origin|methods"))
      return false;
    const char *origin = str(v, "origin");
    if (!origin || strncmp(origin, "https://", 8) || !domain(origin + 8))
      return false;
    pet_net_origin_t *dst = &p->https[p->origins++];
    snprintf(dst->host, sizeof dst->host, "%s", origin + 8);
    const cJSON *methods = cJSON_GetObjectItemCaseSensitive(v, "methods");
    if (!cJSON_IsArray(methods))
      return false;
    cJSON_ArrayForEach(item, methods) {
      const char *m = cJSON_GetStringValue(item);
      unsigned bit = m && !strcmp(m, "GET") ? 1 : m && !strcmp(m, "POST") ? 2 : 0;
      if (!bit || (dst->methods & bit))
        return false;
      dst->methods |= bit;
    }
    if (!dst->methods)
      return false;
    for (unsigned j = 0; j + 1 < p->origins; j++)
      if (!strcmp(p->https[j].host, dst->host))
        return false;
  }
  list = cJSON_GetObjectItemCaseSensitive(root, "mqtts");
  if (list && !cJSON_IsArray(list))
    return false;
  cJSON_ArrayForEach(v, list) {
    unsigned port;
    if (p->brokers == 2 || !keys(v, "host|port|subscribe|publish") || !domain(str(v, "host")) ||
        !number(v, "port", 8883, 8883, &port))
      return false;
    pet_net_broker_t *dst = &p->mqtt[p->brokers++];
    snprintf(dst->host, sizeof dst->host, "%s", str(v, "host"));
    for (unsigned mode = 0; mode < 2; mode++) {
      const cJSON *topics = cJSON_GetObjectItemCaseSensitive(v, mode ? "publish" : "subscribe");
      if (!cJSON_IsArray(topics))
        return false;
      unsigned *count = mode ? &dst->pubs : &dst->subs;
      char (*filters)[129] = mode ? dst->pub : dst->sub;
      cJSON_ArrayForEach(item, topics) {
        const char *s = cJSON_GetStringValue(item);
        if (*count == 8 || !filter(s, true))
          return false;
        snprintf(filters[(*count)++], 129, "%s", s);
      }
    }
    for (unsigned j = 0; j + 1 < p->brokers; j++)
      if (!strcmp(p->mqtt[j].host, dst->host))
        return false;
  }
  const cJSON *cred = cJSON_GetObjectItemCaseSensitive(root, "credentials");
  if (cred && (!keys(cred, "records|totalBytes") || !number(cred, "records", 1, 8, &p->records) ||
               !number(cred, "totalBytes", 1, 8192, &p->credential_bytes)))
    return false;
  if (!p->origins && !p->brokers && !p->records)
    return false;
  return true;
}
bool pet_net_credential_target(const pet_net_policy_t *p, const char *target, char out[104]) {
  if (!target && p->origins + p->brokers != 1) return false;
  for (unsigned i = 0; i < p->origins + p->brokers; ++i) {
    if (i < p->origins) snprintf(out, 104, "https://%s", p->https[i].host);
    else snprintf(out, 104, "mqtts://%s", p->mqtt[i - p->origins].host);
    if (!target || !strcmp(out, target)) return true;
  }
  return false;
}
static bool headers(const cJSON *o) {
  if (!o)
    return true;
  if (!cJSON_IsObject(o))
    return false;
  unsigned n = 0;
  for (const cJSON *v = o->child; v; v = v->next) {
    if (++n > 8 || !text(v->string, 48, false) || !text(cJSON_GetStringValue(v), 512, true))
      return false;
    for (const char *c = v->string; *c; c++)
      if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
            *c == '-'))
        return false;
    for (const cJSON *w = v->next; w; w = w->next)
      if (w->string && !strcasecmp(v->string, w->string))
        return false;
    if (!strcasecmp(v->string, "Host") || !strcasecmp(v->string, "Content-Length") ||
        !strcasecmp(v->string, "Transfer-Encoding") || !strcasecmp(v->string, "Connection") ||
        !strncasecmp(v->string, "Proxy-", 6) || !strcasecmp(v->string, "Authorization"))
      return false;
  }
  return true;
}
static bool request_valid(const pet_net_policy_t *p, const cJSON *r) {
  const char *op = str(r, "op");
  if (!op)
    return false;
  if (!strcmp(op, "http")) {
    if (!keys(r, "op|origin|path|method|body|headers|authorization"))
      return false;
    const char *origin = str(r, "origin"), *path = str(r, "path"), *method = str(r, "method"),
               *body = str(r, "body");
    if (!origin || strncmp(origin, "https://", 8) || !domain(origin + 8) ||
        !text(path, 1024, false) || path[0] != '/' || path[1] == '/' || strchr(path, '\\') ||
        strchr(path, '#') || !method || !headers(cJSON_GetObjectItemCaseSensitive(r, "headers")))
      return false;
    unsigned bit = !strcmp(method, "GET") ? 1 : !strcmp(method, "POST") ? 2 : 0;
    bool allow = false;
    for (unsigned i = 0; i < p->origins; i++)
      if (!strcmp(origin + 8, p->https[i].host) && (p->https[i].methods & bit))
        allow = true;
    if (!allow || (body && strlen(body) > 4096) ||
        (!strcmp(method, "GET") && cJSON_GetObjectItemCaseSensitive(r, "body")) ||
        (cJSON_GetObjectItemCaseSensitive(r, "body") && !body))
      return false;
    const cJSON *auth = cJSON_GetObjectItemCaseSensitive(r, "authorization");
    return !auth || (p->records && keys(auth, "credential|prefix") &&
                     token(str(auth, "credential")) && text(str(auth, "prefix"), 32, true));
  }
  if (!strcmp(op, "credential.set") || !strcmp(op, "credential.get") ||
      !strcmp(op, "credential.erase")) {
    if (!p->records || !keys(r, !strcmp(op, "credential.set") ? "op|key|value|target" : "op|key") ||
        !token(str(r, "key")))
      return false;
    const char *value = str(r, "value");
    char target[104];
    if (!strcmp(op, "credential.set") &&
        ((cJSON_GetObjectItemCaseSensitive(r, "target") && !str(r, "target")) ||
         !pet_net_credential_target(p, str(r, "target"), target))) return false;
    return strcmp(op, "credential.set") ||
           (value && strlen(value) <= 4096 && strlen(value) <= p->credential_bytes);
  }
  if (!strcmp(op, "mqtt.disconnect"))
    return keys(r, "op");
  const char *host = str(r, "host");
  const pet_net_broker_t *b = NULL;
  if (!host)
    return false;
  for (unsigned i = 0; i < p->brokers; i++)
    if (!strcmp(host, p->mqtt[i].host))
      b = &p->mqtt[i];
  if (!b)
    return false;
  if (!strcmp(op, "mqtt.connect")) {
    if (!keys(r, "op|host|clientId|username|passwordCredential") ||
        !text(str(r, "clientId"), 64, false))
      return false;
    const char *user = str(r, "username"), *pw = str(r, "passwordCredential");
    return (!cJSON_GetObjectItemCaseSensitive(r, "username") || text(user, 128, true)) &&
           (!cJSON_GetObjectItemCaseSensitive(r, "passwordCredential") ||
            (p->records && token(pw)));
  }
  bool sub = !strcmp(op, "mqtt.subscribe"), unsub = !strcmp(op, "mqtt.unsubscribe"),
       pub = !strcmp(op, "mqtt.publish");
  if (!sub && !unsub && !pub)
    return false;
  if (!keys(r, pub ? "op|host|topic|payload|qos" : "op|host|topic|qos"))
    return false;
  const char *topic = str(r, "topic");
  if (!filter(topic, !pub))
    return false;
  unsigned qos = 0;
  if (cJSON_GetObjectItemCaseSensitive(r, "qos") && !number(r, "qos", 0, 1, &qos))
    return false;
  if (pub) {
    const char *payload = str(r, "payload");
    if (!payload || strlen(payload) > 4096)
      return false;
  }
  for (unsigned i = 0; i < (pub ? b->pubs : b->subs); i++)
    if (pet_net_topic_contains(pub ? b->pub[i] : b->sub[i], topic, !pub))
      return true;
  return false;
}
static void dispose_result(pet_net_result_t *r) {
  pet_net_zero_free(r->data, r->bytes);
  memset(r, 0, sizeof *r);
}
void pet_net_request_delete(cJSON *request) {
  /* cJSON owns a second decoded copy of provisioning text. */
  cJSON *value = cJSON_GetObjectItemCaseSensitive(request, "value");
  if (cJSON_IsString(value) && value->valuestring) {
    volatile char *p = value->valuestring;
    while (*p) *p++ = 0;
  }
  cJSON_Delete(request);
}
void pet_net_job_dispose(pet_net_job_t *j) {
  pet_net_zero_free(j->request, j->request ? strlen(j->request) : 0);
  memset(j, 0, sizeof *j);
}
void pet_net_init(pet_net_host_t *h) {
  memset(h, 0, sizeof *h);
  pthread_mutex_init(&h->mutex, NULL);
  h->next_id = 1;
}
uint32_t pet_net_activate(pet_net_host_t *h, const char *owner, const cJSON *policy) {
  pet_net_policy_t *p = pet_net_alloc(sizeof *p);
  bool ok = owner && text(owner, 47, false) && p && pet_net_policy_parse(policy, p);
  pthread_mutex_lock(&h->mutex);
  h->active = false;
  if (h->generation != UINT32_MAX)
    h->generation++;
  else
    ok = false;
  for (unsigned i = 0; i < PET_NETWORK_QUEUE; i++)
    pet_net_job_dispose(&h->jobs[i]);
  h->queued = 0;
  for (unsigned i = 0; i < PET_NETWORK_SLOTS; i++)
    dispose_result(&h->results[i]);
  h->retained = 0;
  h->overflow = false;
  if (ok) {
    h->policy = *p;
    snprintf(h->owner, 48, "%s", owner);
    h->active = true;
  } else {
    memset(&h->policy, 0, sizeof h->policy);
    h->owner[0] = 0;
  }
  uint32_t gen = h->active ? h->generation : 0;
  pthread_mutex_unlock(&h->mutex);
  free(p);
  return gen;
}
void pet_net_destroy(pet_net_host_t *h) {
  pet_net_activate(h, NULL, NULL);
  pthread_mutex_destroy(&h->mutex);
}
uint32_t pet_net_generation(pet_net_host_t *h) {
  pthread_mutex_lock(&h->mutex);
  uint32_t gen = h->active ? h->generation : 0;
  pthread_mutex_unlock(&h->mutex);
  return gen;
}
bool pet_net_take(pet_net_host_t *h, pet_net_job_t *job) {
  pthread_mutex_lock(&h->mutex);
  bool ok = h->active && h->queued;
  if (ok) {
    *job = h->jobs[0];
    memmove(h->jobs, h->jobs + 1, (--h->queued) * sizeof *h->jobs);
    memset(&h->jobs[h->queued], 0, sizeof *h->jobs);
  }
  pthread_mutex_unlock(&h->mutex);
  return ok;
}
bool pet_net_complete(pet_net_host_t *h, uint32_t gen, uint32_t id, const char *type, int status,
                      const char *topic, uint8_t *data, size_t bytes) {
  pthread_mutex_lock(&h->mutex);
  bool ok = false;
  if (h->active && h->generation == gen && text(type, 23, false) &&
      (!topic || text(topic, 128, true)) && bytes <= PET_NETWORK_RESPONSE_BYTES &&
      bytes <= PET_NETWORK_TOTAL_BYTES - h->retained && (!bytes || data)) {
    for (unsigned i = 0; i < PET_NETWORK_SLOTS; i++)
      if (!h->results[i].id) {
        if (h->next_id >= INT_MAX)
          break;
        pet_net_result_t *r = &h->results[i];
        r->id = h->next_id++;
        r->generation = gen;
        r->status = status;
        r->data = data;
        r->bytes = bytes;
        r->pending = true;
        snprintf(r->type, sizeof r->type, "%s", type);
        snprintf(r->topic, sizeof r->topic, "%s", topic ? topic : "");
        /* Correlation is carried separately from the opaque response handle. */
        r->request_id = id;
        r->status = status;
        h->retained += bytes;
        ok = true;
        break;
      }
  }
  if (!ok && h->active && h->generation == gen)
    h->overflow = true;
  pthread_mutex_unlock(&h->mutex);
  if (!ok)
    pet_net_zero_free(data, bytes);
  return ok;
}
static bool json_bounded(const char *s, size_t n) {
  unsigned depth = 0;
  bool quoted = false, escaped = false;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = s[i];
    if (quoted) {
      if (escaped)
        escaped = false;
      else if (c == '\\')
        escaped = true;
      else if (c == '"')
        quoted = false;
    } else if (c == '"')
      quoted = true;
    else if (c == '{' || c == '[') {
      if (++depth > 8)
        return false;
    } else if (c == '}' || c == ']') {
      if (!depth)
        return false;
      depth--;
    }
  }
  return !quoted && !depth;
}
int pet_net_call(void *context, unsigned op, const void *input, unsigned bytes, void *output,
                 unsigned capacity) {
  pet_net_lease_t *lease = context;
  if (!lease || !lease->host)
    return PET_NET_UNAVAILABLE;
  pet_net_host_t *h = lease->host;
  cJSON *request = NULL;
  char *copy = NULL;
  if (op == PET_SERVICE_SUBMIT) {
    if (!input || !bytes || bytes > PET_NETWORK_REQUEST_BYTES || memchr(input, 0, bytes) ||
        !json_bounded(input, bytes))
      return PET_NET_INVALID;
    copy = pet_net_alloc(bytes + 1);
    if (!copy)
      return PET_NET_BUSY;
    memcpy(copy, input, bytes);
    copy[bytes] = 0;
    if (strstr(copy, "\\u0000")) {
      pet_net_zero_free(copy, bytes + 1);
      return PET_NET_INVALID;
    }
    request = cJSON_ParseWithLengthOpts(copy, bytes + 1, NULL, true);
    if (!request) {
      pet_net_zero_free(copy, bytes + 1);
      return PET_NET_INVALID;
    }
  }
  pthread_mutex_lock(&h->mutex);
  int result = PET_NET_STALE;
  if (!h->active || lease->generation != h->generation)
    goto done;
  if (op == PET_SERVICE_SUBMIT) {
    result = PET_NET_DENIED;
    if (!request_valid(&h->policy, request))
      goto done;
    result = PET_NET_BUSY;
    if (h->queued == PET_NETWORK_QUEUE || h->next_id >= INT_MAX)
      goto done;
    pet_net_job_t *j = &h->jobs[h->queued++];
    j->generation = h->generation;
    j->id = h->next_id++;
    j->policy = h->policy;
    snprintf(j->owner, 48, "%s", h->owner);
    j->request = copy;
    copy = NULL;
    result = (int)j->id;
  } else if (op == PET_SERVICE_POLL) {
    result = 0;
    if (!output || capacity > 512 || capacity < 2) {
      result = PET_NET_INVALID;
      goto done;
    }
    if (h->overflow) {
      const char *msg = "{\"type\":\"overflow\",\"status\":-5}";
      size_t n = strlen(msg);
      if (capacity <= n) {
        result = PET_NET_LIMIT;
        goto done;
      }
      memcpy(output, msg, n + 1);
      h->overflow = false;
      result = (int)n;
      goto done;
    }
    unsigned oldest = PET_NETWORK_SLOTS;
    for (unsigned i = 0; i < PET_NETWORK_SLOTS; i++)
      if (h->results[i].id && h->results[i].pending &&
          (oldest == PET_NETWORK_SLOTS || h->results[i].id < h->results[oldest].id))
        oldest = i;
    for (unsigned i = oldest; i < PET_NETWORK_SLOTS; i++)
      if (h->results[i].id && h->results[i].pending) {
        pet_net_result_t *r = &h->results[i];
        cJSON *event = cJSON_CreateObject();
        if (!event) {
          result = PET_NET_BUSY;
          break;
        }
        bool complete = cJSON_AddStringToObject(event, "type", r->type) &&
                        cJSON_AddNumberToObject(event, "handle", r->id) &&
                        cJSON_AddNumberToObject(event, "request", r->request_id) &&
                        cJSON_AddNumberToObject(event, "status", r->status) &&
                        cJSON_AddNumberToObject(event, "bytes", r->bytes) &&
                        cJSON_AddStringToObject(event, "topic", r->topic);
        if (!complete) {
          cJSON_Delete(event);
          result = PET_NET_BUSY;
          break;
        }
        if (!cJSON_PrintPreallocated(event, output, (int)capacity, false))
          result = PET_NET_LIMIT;
        else {
          r->pending = false;
          result = (int)strlen(output);
        }
        cJSON_Delete(event);
        break;
      }
  } else if (op == PET_SERVICE_READ || op == PET_SERVICE_RELEASE) {
    if (!input || bytes != (op == PET_SERVICE_READ ? 8u : 4u)) {
      result = PET_NET_INVALID;
      goto done;
    }
    uint32_t handle, offset = 0;
    memcpy(&handle, input, 4);
    if (bytes == 8)
      memcpy(&offset, (const uint8_t *)input + 4, 4);
    result = PET_NET_STALE;
    for (unsigned i = 0; i < PET_NETWORK_SLOTS; i++)
      if (handle && h->results[i].id == handle) {
        pet_net_result_t *r = &h->results[i];
        if (op == PET_SERVICE_RELEASE) {
          h->retained -= r->bytes;
          dispose_result(r);
          result = 0;
          break;
        }
        if (!output || capacity > 4096 || offset > r->bytes) {
          result = PET_NET_INVALID;
          break;
        }
        size_t n = r->bytes - offset;
        if (n > capacity)
          n = capacity;
        if (n)
          memcpy(output, r->data + offset, n);
        result = (int)n;
        break;
      }
  } else
    result = PET_NET_INVALID;
done:
  pthread_mutex_unlock(&h->mutex);
  pet_net_request_delete(request);
  pet_net_zero_free(copy, copy ? bytes + 1 : 0);
  return result;
}
