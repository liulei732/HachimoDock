/* Paired application channel. Boot + board + desktop nonce + device route
 * (big endian) + operation, followed by at most one bounded JSON object. */
#define LAN_APP_PREFIX (LAN_WIDGET_PREFIX_BYTES + 16 + 4 + 1)
#define LAN_APP_JSON_MAX 2048
#define LAN_APP_IMAGE_MAX 3129 /* 2732 Base64 bytes + 397 bytes of maximum-length reference fields */
#define LAN_APP_HEADER 5
#define LAN_APP_LEASE_MS 7000
enum { APP_POLL, APP_VALIDATE, APP_SNAPSHOT, APP_IMAGE };

static struct {
  uint32_t token, pairing;
  uint8_t client[16];
  uint64_t until;
  cJSON *event;
  char *encoded;
} app_channel;
static uint32_t next_app_token;

static unsigned app_status(void) {
  xSemaphoreTake(lock, portMAX_DELAY);
  bool pending = app_channel.event != NULL;
  xSemaphoreGive(lock);
  return pending || pet_app_host_lan_needed() ? 3 : 1;
}

static uint8_t app_widget_ack(uint8_t result, bool hint) {
  return result == 1 && hint && app_status() == 3 ? 3 : result;
}

static void app_retire(void) {
  cJSON_Delete(app_channel.event);
  cJSON_free(app_channel.encoded);
  memset(&app_channel, 0, sizeof(app_channel));
  pet_app_host_lan_route(0, "");
}

bool pet_p4_lan_app_event(uint32_t token, const cJSON *event) {
  if (!lock || !token || xSemaphoreTake(lock, 1) != pdTRUE) return false;
  bool ok = token == app_channel.token && generation == app_channel.pairing &&
    clock_ms() < app_channel.until && volume_available() && atomic_load(&control_trusted) &&
    !atomic_load(&usb_client_active) && pet_app_host_lan_current(token, event);
  /* A heartbeat cannot displace a still-executable press. A switch or exit
   * invalidates the old press synchronously in the host view. */
  if (ok && app_channel.event &&
      cJSON_IsString(cJSON_GetObjectItemCaseSensitive(app_channel.event, "action")) &&
      strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(app_channel.event, "action")), "status") &&
      pet_app_host_lan_current(token, app_channel.event)) {
    ok = false;
  }
  if (ok) {
    char *encoded = cJSON_PrintUnformatted(event);
    cJSON *copy = encoded && strlen(encoded) <= LAN_APP_JSON_MAX ? cJSON_Duplicate(event, true) : NULL;
    if (copy) {
      cJSON_Delete(app_channel.event);
      cJSON_free(app_channel.encoded);
      app_channel.event = copy;
      app_channel.encoded = encoded;
    } else cJSON_free(encoded);
    ok = copy != NULL;
  }
  xSemaphoreGive(lock);
  return ok;
}

/* Caller holds the LAN lock and the serialized device state lane. Reply byte
 * zero is rejection. Poll replies carry capability bits, token, optional event. */
static bool app_exchange(const uint8_t *prefix, const cJSON *p, uint8_t *reply,
                         const char *board, uint64_t now) {
  const uint8_t *client = prefix + LAN_WIDGET_PREFIX_BYTES;
  const uint8_t *wire = client + 16;
  const uint32_t token = ((uint32_t)wire[0]<<24) | ((uint32_t)wire[1]<<16) |
                         ((uint32_t)wire[2]<<8) | wire[3];
  const unsigned op = wire[4];
  if (strlen(board) != LAN_WIDGET_BOARD_BYTES || memcmp(prefix+16, board, LAN_WIDGET_BOARD_BYTES))
    return false;
  const bool live = app_channel.token && app_channel.pairing == generation && now < app_channel.until;
  if (live && memcmp(client, app_channel.client, 16)) return false;
  if (op == APP_POLL && !token) {
    if (!live) {
      app_retire();
      if (next_app_token == UINT32_MAX) return false;
      app_channel.token = ++next_app_token;
      app_channel.pairing = generation;
      memcpy(app_channel.client, client, 16);
    }
    pet_app_host_lan_route(app_channel.token, board);
    app_channel.until = now + LAN_APP_LEASE_MS;
    client_seen_ms = now;
    reply[0] = 1;
#if PET_SERVICE_BRIDGE_ENABLED
    reply[0] |= 2;
#endif
    for (unsigned i=0; i<4; i++) reply[i+1] = app_channel.token >> (24-8*i);
    reply[LAN_APP_HEADER] = 0;
    if (app_channel.event) {
      if (pet_app_host_lan_current(app_channel.token, app_channel.event))
        memcpy(reply+LAN_APP_HEADER, app_channel.encoded, strlen(app_channel.encoded)+1);
      /* Consume before sending. An uncertain poll/ACK never replays a press. */
      cJSON_Delete(app_channel.event);
      cJSON_free(app_channel.encoded);
      app_channel.event = NULL;
      app_channel.encoded = NULL;
    }
    return true;
  }
  if (!live || token != app_channel.token || !pet_app_host_lan_owns(token) || !cJSON_IsObject(p)) return false;
  if (op == APP_VALIDATE) return pet_app_host_lan_current(token, p);
  if (op == APP_SNAPSHOT) return pet_app_host_apply(p, now);
#if PET_SERVICE_BRIDGE_ENABLED
  if (op == APP_IMAGE) {
    const char *operation = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(p, "operation"));
    if (operation && !strcmp(operation, "query")) {
      reply[0] = pet_app_host_image_ready(p, now) ? 2 : 1;
      return true;
    }
    return !operation && pet_app_host_image_apply(p, now);
  }
#endif
  return false;
}
