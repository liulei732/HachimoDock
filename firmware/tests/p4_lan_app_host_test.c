#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include "cJSON.h"
#include "pet_p4_json_source.h"

#define EXT_RAM_BSS_ATTR
#define pdTRUE 1
#define portMAX_DELAY 10
typedef void (*pet_p4_send_line_fn)(const char *, void *);
typedef struct { const char *board_device_id; uint32_t last_update_ms; } pet_p4_runtime_state_t;

static bool held, online = true, paired = true;
static int mutex;
static int *lock = &mutex;
static atomic_bool forget_pending, usb_client_active, control_trusted = true;
static uint32_t generation = 8;
static uint64_t now = 1000;
static pet_p4_send_line_fn selected;
static void *selected_context;
static uint32_t applied;

static int xSemaphoreTake(int *value, int wait) {
  assert(value == lock);
  (void)wait;
  if (held) return 0;
  held = true;
  return pdTRUE;
}
static void xSemaphoreGive(int *value) {
  assert(value == lock && held);
  held = false;
}
static bool pet_device_wifi_online(void) { return online; }
static uint64_t clock_ms(void) { return now; }
static const char *pet_p4_boot_identity(void) { return "0123456789abcdef"; }
static void pet_app_host_transport(pet_p4_send_line_fn send, void *context, const char *board) {
  assert(!strcmp(board, "p4-0123456789ab"));
  selected = send;
  selected_context = context;
}
static bool pet_app_host_apply(const cJSON *snapshot, uint64_t timestamp) {
  assert(held && timestamp == now);
  applied = cJSON_GetObjectItemCaseSensitive(snapshot, "sequence")->valueint;
  return true;
}

#include "pet_p4_lan_app.inc"

static uint8_t wire[LAN_APP_EVENT_MAX + 6];

static size_t poll(const char *boot, uint8_t owner, uint8_t ack, const char *snapshot) {
  memcpy(wire, boot, 16);
  memset(wire + 16, owner, 16);
  memset(wire + 32, 0, 4);
  wire[35] = ack;
  size_t size = LAN_APP_HEADER;
  if (snapshot) {
    memcpy(wire + size, snapshot, strlen(snapshot));
    size += strlen(snapshot);
  }
  return lan_app_exchange(wire, size, generation);
}

static void tick(pet_p4_runtime_state_t *state) {
  assert(xSemaphoreTake(lock, 0) == pdTRUE);
  lan_app_apply(state, now);
  xSemaphoreGive(lock);
}
static void wired_send(const char *line, void *context) { (void)line; (void)context; }

int main(void) {
  pet_p4_runtime_state_t state = { .board_device_id = "p4-0123456789ab" };
  const char *boot = pet_p4_boot_identity();
  const char *first = "{\"topic\":\"app/event\",\"payload\":{\"sequence\":1}}";
  const char *second = "{\"topic\":\"app/event\",\"payload\":{\"sequence\":2}}";
  atomic_store(&control_trusted, false);
  assert(poll(boot, 1, 0, NULL) == 1 && wire[0] == 0);
  atomic_store(&control_trusted, true);
  assert(poll("ffffffffffffffff", 1, 0, NULL) == 1 && wire[0] == 0);
  assert(poll(boot, 1, 0, NULL) == 5 && wire[0] == 1 && wire[4] == 0);
  tick(&state);
  assert(selected && !pet_p4_lan_app_wired_selected());
  selected("{\"topic\":\"widget/notify\",\"payload\":{}}", selected_context);
  assert(poll(boot, 1, 0, NULL) == 5 && wire[4] == 0);
  selected(first, selected_context);
  selected(second, selected_context);
  assert(poll(boot, 1, 0, NULL) == 5 + strlen(first) && wire[4] == 1);
  assert(!memcmp(wire + 5, first, strlen(first)));
  assert(poll(boot, 1, 2, NULL) == 5 + strlen(first) && wire[4] == 1);
  assert(poll(boot, 2, 1, "{\"sequence\":93}") == 1 && wire[0] == 0);
  assert(poll(boot, 1, 1, "{\"sequence\":1}") == 5 + strlen(second) && wire[4] == 2);
  assert(!memcmp(wire + 5, second, strlen(second)));
  tick(&state);
  assert(applied == 1 && state.last_update_ms == 1);

  assert(poll(boot, 1, 2, "{\"sequence\":2}") == 5 && wire[4] == 0);
  assert(poll(boot, 2, 0, NULL) == 1 && wire[0] == 0);
  now += LAN_APP_LEASE_MS;
  assert(poll(boot, 2, 0, NULL) == 5 && wire[4] == 0);
  void *retired = selected_context;
  tick(&state);
  assert(applied == 1 && selected_context != retired);
  selected(first, retired);
  assert(poll(boot, 2, 0, NULL) == 5 && wire[4] == 0);
  selected(first, selected_context);
  assert(poll(boot, 2, 0, NULL) == 5 + strlen(first) && wire[4] == 1);

  assert(poll(boot, 2, 1, "{\"sequence\":3}") == 5);
  atomic_store(&usb_client_active, true);
  pet_p4_lan_app_wired_route(wired_send, NULL, state.board_device_id, true);
  tick(&state);
  assert(applied == 1 && selected == wired_send && pet_p4_lan_app_wired_selected());
  assert(poll(boot, 2, 1, NULL) == 1 && wire[0] == 0);

  atomic_store(&usb_client_active, false);
  assert(poll(boot, 3, 0, NULL) == 5);
  tick(&state);
  selected(first, selected_context);
  now += LAN_APP_LEASE_MS;
  assert(poll(boot, 3, 1, NULL) == 1);
  tick(&state);
  assert(selected == NULL && applied == 1);
  assert(poll(boot, 4, 0, NULL) == 5 && wire[4] == 0);
  ++generation;
  tick(&state);
  assert(selected == NULL && lan_app_count == 0);
}
