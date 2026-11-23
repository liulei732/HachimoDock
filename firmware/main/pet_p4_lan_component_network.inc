/* Dedicated request under the existing paired AEAD transport. The outer
 * generation, boot and association are checked while admitting the operation.
 * No JSON/topic dispatch, guest memory reads or credential exports here. */
#define LAN_COMPONENT_NET_MODE 23
enum {
  LAN_NET_STATUS = 1, LAN_NET_SET = 2, LAN_NET_PREFIX = 21,
  LAN_NET_MAX = LAN_NET_PREFIX + 48 + 33 + 104 + 4097, LAN_NET_REPLY = 512
};

static const char *lan_net_string(const uint8_t **at, size_t *left, size_t max, bool empty) {
  size_t bound = *left > max + 1 ? max + 1 : *left;
  const uint8_t *end = memchr(*at, 0, bound);
  if (!end || (!empty && end == *at)) return NULL;
  const char *s = (const char *)*at;
  size_t n = end - *at + 1;
  *at += n;
  *left -= n;
  return s;
}

/* The reply reuses the wiped request buffer. Errors are constant and never
 * include a request string. A positive write receipt means queued, not saved. */
static size_t lan_component_net_request(uint8_t *body, size_t size, uint32_t request_generation) {
  cJSON *status = NULL;
  int queued = PET_NET_DENIED;
  bool setting = false;
  if (size >= LAN_NET_PREFIX && size <= LAN_NET_MAX) {
    uint32_t association = bundle_u32(body + 16);
    /* OTA owns its own mutex. Observe its admission gate before taking the
     * pairing lock, like the other serialized LAN write paths. */
    bool can_set = body[20] == LAN_NET_SET && !pet_p4_ota_active();
    xSemaphoreTake(lock, portMAX_DELAY);
    const char *boot = pet_p4_boot_identity();
    bool admitted = volume_available() && generation == request_generation &&
      atomic_load(&control_trusted) && boot && strlen(boot) == 16 &&
      !memcmp(boot, body, 16) && association && association == pet_device_wifi_association();
    if (admitted && body[20] == LAN_NET_STATUS && size == LAN_NET_PREFIX)
      status = pet_net_device_status();
    if (admitted && can_set && !atomic_load(&usb_client_active) &&
        !pet_p4_diagnostics_reboot_pending()) {
      const uint8_t *at = body + LAN_NET_PREFIX;
      size_t left = size - LAN_NET_PREFIX;
      const char *owner = lan_net_string(&at, &left, 47, false);
      const char *key = lan_net_string(&at, &left, 32, false);
      const char *target = lan_net_string(&at, &left, 103, true);
      const char *value = lan_net_string(&at, &left, 4096, false);
      if (owner && key && target && value && !left) {
        setting = true;
        queued = pet_net_device_provision_target(owner, key, value, *target ? target : NULL);
      }
    }
    xSemaphoreGive(lock);
  }
  mbedtls_platform_zeroize(body, size);
  size_t n;
  if (status && cJSON_PrintPreallocated(status, (char *)body, LAN_NET_REPLY, false)) {
    n = strlen((char *)body);
  } else if (setting) {
    n = snprintf((char *)body, LAN_NET_REPLY, "{\"ok\":%s,\"queued\":%s,\"request\":%d}",
      queued > 0 ? "true" : "false", queued > 0 ? "true" : "false", queued);
  } else {
    static const char denied[] = "{\"ok\":false}";
    memcpy(body, denied, sizeof denied);
    n = sizeof denied - 1;
  }
  cJSON_Delete(status);
  return n;
}
