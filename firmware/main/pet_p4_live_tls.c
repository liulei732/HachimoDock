#include "pet_p4_live_tls.h"

static pet_wifi_https_stage_t pet_live_tls_failure(esp_tls_t *tls, int result,
                                                          int *error_code) {
  esp_tls_error_handle_t handle = NULL;
  int verify_flags = 0, tls_code = 0;
  esp_err_t error = esp_tls_get_error_handle(tls, &handle) == ESP_OK && handle
    ? esp_tls_get_and_clear_last_error(handle, &tls_code, &verify_flags) : 0;
  *error_code = tls_code ? -tls_code : error;
  if (verify_flags) return PET_WIFI_HTTPS_CERTIFICATE;
  if (error == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME) return PET_WIFI_HTTPS_DNS;
  if (error == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT ||
      error == ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST) return PET_WIFI_HTTPS_TCP;
#ifdef ESP_PLATFORM
  esp_tls_conn_state_t state;
  if (result == 0 && esp_tls_get_conn_state(tls, &state) == ESP_OK &&
      state == ESP_TLS_CONNECTING) return PET_WIFI_HTTPS_TCP;
#endif
  return result == 0 ? PET_WIFI_HTTPS_IO : PET_WIFI_HTTPS_TLS;
}

/* One fresh connection after a transient failure. The caller hasn't sent
 * a request yet. Certificate errors and cancelled playback are terminal. */
esp_tls_t *pet_live_tls_connect_port(const char *host, uint16_t port, const esp_tls_cfg_t *config,
                                              int64_t limit, pet_live_tls_current_fn current,
                                              void *context) {
  for (unsigned attempt = 0; attempt < 2 && current(context); ++attempt) {
    const int64_t start = esp_timer_get_time() / 1000;
    if (start >= limit) break;
    esp_tls_t *tls = esp_tls_init();
    if (!tls) return NULL;
    int64_t deadline = start + config->timeout_ms;
    if (deadline > limit) deadline = limit;
    esp_tls_cfg_t step = *config;
    // Include the first TCP retransmission on a lossy connection, without
    // extending the caller's total request budget.
    step.timeout_ms = deadline - start > 3500 ? 3500 : (int)(deadline - start);
    int result = 0;
    while (esp_timer_get_time() / 1000 < deadline && current(context)) {
      result = esp_tls_conn_new_async(host, (int)strlen(host), port, &step, tls);
      if (result != 0) break;
#ifdef ESP_PLATFORM
      // ESP-TLS 5.5 retains the fd sets that select() clears on a TCP timeout.
      // A fresh attempt can make progress; polling that socket again cannot.
      esp_tls_conn_state_t state;
      if (esp_tls_get_conn_state(tls, &state) == ESP_OK && state == ESP_TLS_CONNECTING) break;
#endif
      vTaskDelay(pdMS_TO_TICKS(10) + 1);
    }
    if (result == 1 && current(context) && esp_timer_get_time() / 1000 < limit) return tls;
    int error;
    const pet_wifi_https_stage_t stage = pet_live_tls_failure(tls, result, &error);
    const bool retry = stage == PET_WIFI_HTTPS_TCP || stage == PET_WIFI_HTTPS_DNS ||
      (result == 0 && stage == PET_WIFI_HTTPS_IO);
    esp_tls_conn_destroy(tls);
    if (result != 1 && current(context)) pet_device_wifi_note_https(stage, error);
    if (!retry) break;
    if (attempt == 0 && current(context)) vTaskDelay(pdMS_TO_TICKS(250) + 1);
  }
  return NULL;
}

#ifdef ESP_PLATFORM
#include "esp_crt_bundle.h"
#include <stdatomic.h>
#include "lwip/dns.h"
#include "lwip/tcpip.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "pet_p4_wifi_endpoint.h"

/* lwIP owns the callback until completion. An abandoned slot stays reserved
 * so a late reply can neither access a caller's stack nor answer a new lookup. */
enum { DNS_FREE, DNS_PENDING, DNS_ABANDONED, DNS_READY };
typedef struct {
  atomic_uint state;
  char host[sizeof(((pet_wifi_endpoint_t *)0)->host)];
  uint32_t address;
} public_dns_slot_t;
static public_dns_slot_t public_dns_slots[2];

static void public_dns_complete(const char *name, const ip_addr_t *answer, void *context) {
  (void)name;
  public_dns_slot_t *slot = context;
  slot->address = 0;
  /* ESP lwIP returns DNS_MAX_HOST_IP entries, including for cached answers. */
  for (unsigned i = 0; answer && i < DNS_MAX_HOST_IP; ++i) {
    if (!IP_IS_V4(&answer[i])) continue;
    uint32_t candidate = ip4_addr_get_u32(ip_2_ip4(&answer[i]));
    if (pet_wifi_endpoint_public_ipv4(ntohl(candidate))) {
      slot->address = candidate;
      break;
    }
  }
  unsigned pending = DNS_PENDING;
  if (!atomic_compare_exchange_strong(&slot->state, &pending, DNS_READY))
    atomic_store(&slot->state, DNS_FREE);
}

static void public_dns_start(void *context) {
  public_dns_slot_t *slot = context;
  ip_addr_t answer[DNS_MAX_HOST_IP] = {0};
  if (atomic_load(&slot->state) == DNS_ABANDONED) {
    atomic_store(&slot->state, DNS_FREE);
    return;
  }
  err_t result = dns_gethostbyname_addrtype(slot->host, answer, public_dns_complete, slot, LWIP_DNS_ADDRTYPE_IPV4);
  if (result != ERR_INPROGRESS)
    public_dns_complete(NULL, result == ERR_OK ? answer : NULL, slot);
}

static bool public_dns_resolve(const char *host, int64_t deadline, pet_live_tls_current_fn current,
                               void *context, char ip[INET_ADDRSTRLEN]) {
  const int64_t start = esp_timer_get_time() / 1000;
  if (!host || strlen(host) >= sizeof(public_dns_slots[0].host) || start >= deadline || !current(context))
    return false;
  if (deadline > start + 2000) deadline = start + 2000;
  for (size_t i = 0; i < sizeof(public_dns_slots)/sizeof(public_dns_slots[0]); i++) {
    public_dns_slot_t *slot = &public_dns_slots[i];
    unsigned state = DNS_FREE;
    if (!atomic_compare_exchange_strong(&slot->state, &state, DNS_PENDING)) continue;
    strcpy(slot->host, host);
    if (tcpip_try_callback(public_dns_start, slot) != ERR_OK) {
      atomic_store(&slot->state, DNS_FREE);
      return false;
    }
    while (current(context) && esp_timer_get_time() / 1000 < deadline) {
      if (atomic_load(&slot->state) == DNS_READY) {
        uint32_t address = slot->address;
        atomic_store(&slot->state, DNS_FREE);
        return address &&
               inet_ntop(AF_INET, &address, ip, INET_ADDRSTRLEN) != NULL;
      }
      vTaskDelay(pdMS_TO_TICKS(20) + 1);
    }
    state = DNS_PENDING;
    if (!atomic_compare_exchange_strong(&slot->state, &state, DNS_ABANDONED))
      atomic_store(&slot->state, DNS_FREE); /* Completion won the race. */
    return false;
  }
  return false;
}

esp_tls_t *pet_live_tls_connect_public_port(const char *host, uint16_t port,
    const esp_tls_cfg_t *config, int64_t deadline, pet_live_tls_current_fn current,
    void *context) {
  char ip[INET_ADDRSTRLEN] = {0};
  if (!public_dns_resolve(host, deadline, current, context, ip)) {
    if (current(context)) pet_device_wifi_note_https(PET_WIFI_HTTPS_DNS, 0);
    return NULL;
  }
  esp_tls_cfg_t public_config = *config;
  public_config.common_name = host;
  public_config.skip_common_name = false;
  public_config.cacert_buf = NULL;
  public_config.cacert_bytes = 0;
  public_config.crt_bundle_attach = esp_crt_bundle_attach;
  return pet_live_tls_connect_port(ip, port, &public_config, deadline, current, context);
}
#endif
