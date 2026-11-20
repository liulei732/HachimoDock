// Authenticated component messages. The LAN lock owns the mailbox and lease;
// the runtime task alone chooses the active component route and applies replies.
#define LAN_APP_MODE 22
#define LAN_APP_HEADER 36
#define LAN_APP_SNAPSHOT_MAX 2048
#define LAN_APP_EVENT_MAX 3072
#define LAN_APP_CAPACITY 4
#define LAN_APP_LEASE_MS 3500

typedef struct {
  uint32_t id;
  uint64_t expires;
  size_t length;
  char line[LAN_APP_EVENT_MAX];
} lan_app_event_t;

static EXT_RAM_BSS_ATTR lan_app_event_t lan_app_events[LAN_APP_CAPACITY];
static unsigned lan_app_head, lan_app_count;
static uint32_t lan_app_route, lan_app_pair_generation, lan_app_event_id;
static uint32_t lan_app_selected;
static uint64_t lan_app_until;
static uint8_t lan_app_owner[16];
static cJSON *lan_app_pending;

static bool lan_app_allowed(void) {
  return paired && !atomic_load(&forget_pending) && atomic_load(&control_trusted) &&
    !atomic_load(&usb_client_active) && pet_device_wifi_online();
}

static void lan_app_reset(void) {
  lan_app_count = 0;
  lan_app_head = 0;
  lan_app_until = 0;
  cJSON_Delete(lan_app_pending);
  lan_app_pending = NULL;
}

static bool lan_app_current(uint64_t now) {
  return lan_app_until > now && lan_app_pair_generation == generation && lan_app_allowed();
}

static void lan_app_pop(void) {
  lan_app_head = (lan_app_head + 1) % LAN_APP_CAPACITY;
  --lan_app_count;
}

static void lan_app_send_event(const char *line, void *context) {
  static const char prefix[] = "{\"topic\":\"app/event\",";
  const size_t length = strlen(line);
  if (length > LAN_APP_EVENT_MAX || strncmp(line, prefix, sizeof(prefix) - 1) ||
      xSemaphoreTake(lock, 0) != pdTRUE) return;
  const uint64_t now = clock_ms();
  if (lan_app_current(now) && (uintptr_t)context == lan_app_route &&
      lan_app_count < LAN_APP_CAPACITY && lan_app_event_id < UINT32_MAX) {
    lan_app_event_t *event = &lan_app_events[(lan_app_head + lan_app_count) % LAN_APP_CAPACITY];
    event->id = ++lan_app_event_id;
    event->expires = now + LAN_APP_LEASE_MS;
    event->length = length;
    memcpy(event->line, line, length);
    ++lan_app_count;
  }
  xSemaphoreGive(lock);
}

static size_t lan_app_exchange(uint8_t *plain, size_t size, uint32_t request_generation) {
  const uint32_t ack = ((uint32_t)plain[32] << 24) | ((uint32_t)plain[33] << 16) |
    ((uint32_t)plain[34] << 8) | plain[35];
  if (strlen(pet_p4_boot_identity()) != 16 || memcmp(plain, pet_p4_boot_identity(), 16)) {
    plain[0] = 0;
    return 1;
  }
  cJSON *snapshot = NULL;
  plain[size] = 0;
  if (size > LAN_APP_HEADER &&
      pet_json_source_text_limit_bytes(plain + LAN_APP_HEADER, size - LAN_APP_HEADER, 12))
    snapshot = cJSON_ParseWithOpts((char *)plain + LAN_APP_HEADER, NULL, true);

  xSemaphoreTake(lock, portMAX_DELAY);
  const uint64_t now = clock_ms();
  size_t reply = 1;
  plain[0] = 0;
  if (request_generation == generation && lan_app_allowed()) {
    const bool live = lan_app_current(now);
    const bool same_owner = live && !memcmp(plain + 16, lan_app_owner, 16);
    const bool open = !live && !ack && size == LAN_APP_HEADER;
    if (open) {
      lan_app_reset();
      if (++lan_app_route == 0) ++lan_app_route;
      lan_app_event_id = 0;
      lan_app_pair_generation = generation;
      memcpy(lan_app_owner, plain + 16, 16);
    }
    if (same_owner || open) {
      lan_app_until = now + LAN_APP_LEASE_MS;
      plain[0] = 1;
      memset(plain + 1, 0, 4);
      reply = 5;
      if (same_owner && cJSON_IsObject(snapshot)) {
        cJSON_Delete(lan_app_pending);
        lan_app_pending = snapshot;
        snapshot = NULL;
      }
      while (lan_app_count &&
             (lan_app_events[lan_app_head].id == ack || lan_app_events[lan_app_head].expires <= now))
        lan_app_pop();
      if (lan_app_count) {
        const lan_app_event_t *event = &lan_app_events[lan_app_head];
        plain[1] = event->id >> 24;
        plain[2] = event->id >> 16;
        plain[3] = event->id >> 8;
        plain[4] = event->id;
        memcpy(plain + reply, event->line, event->length);
        reply += event->length;
      }
    }
  }
  xSemaphoreGive(lock);
  cJSON_Delete(snapshot);
  return reply;
}

static void lan_app_apply(pet_p4_runtime_state_t *state, uint64_t now) {
  const uint32_t route = lan_app_current(now) ? lan_app_route : 0;
  if (!route && lan_app_until) lan_app_reset();
  cJSON *snapshot = lan_app_pending;
  lan_app_pending = NULL;

  if (route != lan_app_selected) {
    lan_app_selected = route;
    pet_app_host_transport(route ? lan_app_send_event : NULL, (void *)(uintptr_t)route,
                           state->board_device_id);
  }
  if (route && snapshot && pet_app_host_apply(snapshot, now)) ++state->last_update_ms;
  cJSON_Delete(snapshot);
}

void pet_p4_lan_app_wired_route(pet_p4_send_line_fn send, void *context,
                                const char *board, bool bound) {
  if (bound) lan_app_selected = 0;
  if (!lan_app_selected) pet_app_host_transport(send, context, board);
}

bool pet_p4_lan_app_wired_selected(void) {
  return !lan_app_selected;
}
