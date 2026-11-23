// Include the real foreground broker and drive its host-queue callbacks without radio I/O.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
typedef struct {
  uint16_t value;
} ble_uuid_t;
typedef struct {
  ble_uuid_t u;
} ble_uuid16_t;
#define BLE_UUID16_INIT(id)                                                                        \
  {                                                                                                \
    {                                                                                              \
      id                                                                                           \
    }                                                                                              \
  }
#define ble_uuid_u16(u) ((u)->value)
typedef struct {
  unsigned type;
  uint8_t val[6];
} ble_addr_t;
enum {
  BLE_ADDR_PUBLIC,
  BLE_ADDR_RANDOM,
  BLE_HS_EDONE = 14,
  BLE_HS_EAPP = 8,
  BLE_GAP_EVENT_CONNECT,
  BLE_GAP_EVENT_DISCONNECT,
  BLE_ERR_REM_USER_CONN_TERM = 19,
  BLE_HS_CONN_HANDLE_NONE = 65535
};
struct ble_gap_event {
  int type;
  struct {
    int status;
    uint16_t conn_handle;
  } connect;
};
struct os_mbuf {
  size_t len;
  uint8_t bytes[32];
};
struct ble_gatt_error {
  int status;
};
struct ble_gatt_svc {
  uint16_t start_handle, end_handle;
  ble_uuid16_t uuid;
};
struct ble_gatt_attr {
  uint16_t handle;
  struct os_mbuf *om;
};
#define OS_MBUF_PKTLEN(m) ((m)->len)
#define portMUX_TYPE int
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
static uint32_t now;
static int close_result;
static bool keyboard, wifi6 = true;
static unsigned connects, terminations, cancels, dropped, passed, chip = 13, states = 64;
static ble_addr_t dest;
static uint16_t service, characteristic;
static int (*gap)(struct ble_gap_event *, void *);
static int (*svc)(uint16_t, const struct ble_gatt_error *, const struct ble_gatt_svc *, void *);
static int (*read_value)(uint16_t, const struct ble_gatt_error *, struct ble_gatt_attr *, void *);
static void *token;
static int64_t esp_timer_get_time(void) { return (int64_t)now * 1000; }
unsigned pet_p4_hosted_radio_chip_id(void) { return chip; }
bool pet_p4_is_wifi6_hardware(void) { return wifi6; }
int pet_ble_shortcut_available(void) { return 1; }
bool pet_ble_keyboard_enabled(void) { return keyboard; }
int ble_hs_hci_cmd_tx(uint16_t cmd, const void *req, uint8_t n, void *out, uint8_t len) {
  assert(cmd == 0x201c && !req && !n && len == 8);
  ((uint8_t *)out)[0] = states;
  return 0;
}
static int ble_gap_connect(unsigned own, const ble_addr_t *peer, int timeout, void *params,
                           int (*cb)(struct ble_gap_event *, void *), void *arg) {
  assert(own == 0 && timeout == 4000 && !params);
  connects++;
  dest = *peer;
  gap = cb;
  token = arg;
  return 0;
}
static int ble_gattc_disc_svc_by_uuid(uint16_t h, const ble_uuid_t *uuid,
                                      int (*cb)(uint16_t, const struct ble_gatt_error *,
                                                const struct ble_gatt_svc *, void *),
                                      void *arg) {
  assert(h == 7 && arg == token);
  service = uuid->value;
  svc = cb;
  return 0;
}
static int ble_gattc_read_by_uuid(uint16_t h, uint16_t first, uint16_t last, const ble_uuid_t *uuid,
                                  int (*cb)(uint16_t, const struct ble_gatt_error *,
                                            struct ble_gatt_attr *, void *),
                                  void *arg) {
  assert(h == 7 && first == 8 && last == 19 && arg == token);
  characteristic = uuid->value;
  read_value = cb;
  return 0;
}
static int ble_gap_terminate(uint16_t h, int reason) {
  assert(h == 7 && reason == 19);
  terminations++;
  return close_result;
}
static int ble_gap_conn_cancel(void) {
  cancels++;
  return 0;
}
static int os_mbuf_copydata(const struct os_mbuf *m, size_t off, size_t n, void *dst) {
  if (off + n > m->len)
    return 1;
  memcpy(dst, m->bytes + off, n);
  return 0;
}
static void os_mbuf_free_chain(struct os_mbuf *m) {
  (void)m;
  dropped++;
}
int __real_ble_hs_hci_evt_acl_process(struct os_mbuf *m) {
  (void)m;
  passed++;
  return 0;
}
#include "pet_p4_ble_accessory.c"

static uint32_t owner;
static void open_panel(unsigned kind) {
  now += 10;
  pet_accessory_select(++owner, kind);
  pet_accessory_shown(owner, now);
  now += 301;
  pet_accessory_shown(owner, now);
}
static unsigned status(void) {
  pet_accessory_view_t v;
  assert(pet_accessory_view(owner, &v));
  return v.status;
}
static void click(void) {
  pet_accessory_key(owner, true, false, now);
  pet_accessory_key(owner, false, true, now);
}
static void connect_ok(void) {
  struct ble_gap_event e = {.type = BLE_GAP_EVENT_CONNECT, .connect.conn_handle = 7};
  gap(&e, token);
}
static void disconnected(void) {
  struct ble_gap_event e = {.type = BLE_GAP_EVENT_DISCONNECT};
  gap(&e, token);
}
static void begin(unsigned kind) {
  open_panel(kind);
  assert(pet_accessory_edit(1, false, now));
  click();
  assert(pet_accessory_poll(0, false, NULL));
  assert(dest.val[5] == 0x10 && dest.type == BLE_ADDR_PUBLIC);
  connect_ok();
}
static void get_range(void) {
  struct ble_gatt_error ok = {0}, done = {BLE_HS_EDONE};
  struct ble_gatt_svc range = {8, 19, BLE_UUID16_INIT(service)};
  assert(!svc(7, &ok, &range, token));
  assert(!svc(7, &done, NULL, token));
}
static void measurement(unsigned kind, uint16_t service_id, uint16_t char_id, struct os_mbuf *bytes,
                        int expected) {
  begin(kind);
  assert(service == service_id);
  get_range();
  assert(characteristic == char_id);
  struct ble_gatt_error ok = {0}, done = {BLE_HS_EDONE};
  struct ble_gatt_attr a = {10, bytes};
  assert(!read_value(7, &ok, &a, token));
  assert(!read_value(7, &done, NULL, token));
  pet_accessory_view_t v;
  assert(pet_accessory_view(owner, &v));
  assert(v.status == PET_ACCESSORY_VALUE && v.value == expected);
  disconnected();
  assert(!pet_accessory_poll(0, false, NULL));
}
int main(void) {
  pet_accessory_sync();
  assert(pet_accessory_ready());
  wifi6 = false;
  assert(!pet_accessory_ready());
  wifi6 = true;
  states = 0;
  pet_accessory_sync();
  assert(!pet_accessory_ready());
  states = 64;
  chip = 0;
  pet_accessory_sync();
  assert(!pet_accessory_ready());
  chip = 13;
  pet_accessory_sync();
  open_panel(PET_ACCESSORY_BATTERY);
  pet_accessory_key(owner, false, true, now);
  assert(!pet_accessory_poll(0, false, NULL));
  click();
  assert(!pet_accessory_poll(0, false, NULL) && status() == PET_ACCESSORY_ADDRESS);
  assert(pet_accessory_edit(1, false, now));
  pet_accessory_key(owner, true, false, now);
  pet_accessory_key(owner, false, false, now);
  pet_accessory_key(owner, false, true, now);
  assert(!pet_accessory_poll(0, false, NULL));
  keyboard = true;
  click();
  assert(status() == PET_ACCESSORY_BUSY);
  keyboard = false;
  uint8_t bond[6] = {0, 0, 0, 0, 0, 0x10};
  click();
  assert(!pet_accessory_poll(0, false, bond) && status() == PET_ACCESSORY_BUSY);
  click();
  assert(!pet_accessory_poll(0, true, NULL) && status() == PET_ACCESSORY_BUSY);
  click();
  now += 501;
  assert(!pet_accessory_poll(0, false, NULL) && status() == PET_ACCESSORY_FAILED);
  now += 1300;
  click();
  assert(!pet_accessory_poll(0, false, NULL));
  assert(!connects && !terminations);
  struct os_mbuf batt = {1, {81}}, temp = {2, {0xd8, 0xfd}};
  measurement(PET_ACCESSORY_BATTERY, 0x180f, 0x2a19, &batt, 81);
  measurement(PET_ACCESSORY_TEMPERATURE, 0x181a, 0x2a6e, &temp, -552);
  begin(PET_ACCESSORY_BATTERY);
  get_range();
  struct ble_gatt_error ok = {0}, done = {BLE_HS_EDONE};
  struct ble_gatt_attr bad = {9, &temp};
  assert(read_value(7, &ok, &bad, token) && status() == PET_ACCESSORY_FAILED);
  disconnected();
  begin(PET_ACCESSORY_BATTERY);
  get_range();
  struct ble_gatt_attr value = {9, &batt};
  assert(!read_value(7, &ok, &value, token));
  assert(read_value(7, &ok, &value, token));
  disconnected();
  begin(PET_ACCESSORY_TEMPERATURE);
  get_range();
  temp.bytes[0] = 0;
  temp.bytes[1] = 0x80;
  assert(read_value(7, &ok, &bad, token) && status() == PET_ACCESSORY_FAILED);
  disconnected();
  begin(PET_ACCESSORY_BATTERY);
  void *old = token;
  get_range();
  open_panel(PET_ACCESSORY_TEMPERATURE);
  assert(read_value(7, &done, NULL, old));
  assert(pet_accessory_poll(0, false, NULL));
  assert(status() == PET_ACCESSORY_EDIT);
  disconnected();
  begin(PET_ACCESSORY_TEMPERATURE);
  get_range();
  keyboard = true;
  assert(read_value(7, &done, NULL, token));
  assert(pet_accessory_poll(0, true, NULL));
  assert(status() == PET_ACCESSORY_FAILED);
  disconnected();
  keyboard = false;
  begin(PET_ACCESSORY_BATTERY);
  now += 8001;
  assert(pet_accessory_poll(0, false, NULL));
  assert(status() == PET_ACCESSORY_FAILED);
  disconnected();
  open_panel(PET_ACCESSORY_BATTERY);
  pet_accessory_edit(1, false, now);
  click();
  pet_accessory_poll(0, false, NULL);
  click();
  assert(pet_accessory_poll(0, false, NULL) && cancels);
  connect_ok();
  disconnected();
  struct os_mbuf packet = {9, {7, 0x20, 5, 0, 1, 0, 4, 0, 0x07}};
  begin(PET_ACCESSORY_BATTERY);
  assert(!__wrap_ble_hs_hci_evt_acl_process(&packet) && passed == 1);
  packet.bytes[6] = 6;
  assert(!__wrap_ble_hs_hci_evt_acl_process(&packet) && dropped == 1);
  packet.bytes[6] = 4;
  packet.bytes[8] = 0x12;
  assert(!__wrap_ble_hs_hci_evt_acl_process(&packet) && dropped == 2);
  disconnected();
  assert(!__wrap_ble_hs_hci_evt_acl_process(&packet) && passed == 2);
  begin(PET_ACCESSORY_BATTERY);
  pet_accessory_reset();
  assert(!pet_accessory_ready() && status() == PET_ACCESSORY_FAILED);
  pet_accessory_sync();
  begin(PET_ACCESSORY_BATTERY);
  now += 8001;
  close_result = 1;
  unsigned before = terminations;
  for (int i = 0; i < 8; i++) {
    assert(pet_accessory_poll(0, false, NULL));
    now += 251;
  }
  assert(terminations == before + 3 && status() == PET_ACCESSORY_FAILED);
  disconnected();
  pet_accessory_select(0, 0);
  pet_accessory_view_t v;
  assert(!pet_accessory_view(owner, &v));
}
