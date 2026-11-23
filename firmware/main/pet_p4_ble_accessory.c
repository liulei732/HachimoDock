#include "pet_p4_ble_accessory.h"
#if PET_BLE_ACCESSORY
#include "pet_ble_accessory_values.h"
#include "pet_p4_device_wifi.h"
#include "pet_p4_ble_shortcut.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "host/ble_hs.h"
#include "nimble/hci_common.h"
#include <stdatomic.h>
#include <string.h>

extern bool pet_p4_is_wifi6_hardware(void);
extern int ble_hs_hci_cmd_tx(uint16_t, const void *, uint8_t, void *, uint8_t);
static atomic_bool capable;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
// Only the physical UI can write an address or request. Nothing is persisted.
static struct {
  uint32_t owner, sequence, approved_at;
  unsigned kind;
  uint64_t shown, frame;
  bool down;
  pet_accessory_view_t view;
} ui;

static uint32_t ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

bool pet_accessory_ready(void) {
  return atomic_load(&capable) && pet_ble_shortcut_available() > 0 && pet_p4_is_wifi6_hardware();
}

void pet_accessory_select(uint32_t owner, unsigned kind) {
  portENTER_CRITICAL(&lock);
  if (ui.owner != owner) {
    uint32_t next = ui.sequence + 1;
    memset(&ui, 0, sizeof(ui));
    ui.sequence = next ? next : 1;
    ui.owner = owner;
    ui.kind = kind;
  }
  portEXIT_CRITICAL(&lock);
}

bool pet_accessory_view(uint32_t owner, pet_accessory_view_t *out) {
  portENTER_CRITICAL(&lock);
  bool current = owner && owner == ui.owner;
  if (current)
    *out = ui.view;
  portEXIT_CRITICAL(&lock);
  return current;
}
void pet_accessory_shown(uint32_t owner, uint64_t now) {
  portENTER_CRITICAL(&lock);
  if (owner && owner == ui.owner) {
    if (!ui.shown)
      ui.shown = now;
    ui.frame = now;
  }
  portEXIT_CRITICAL(&lock);
}
static bool current_input(uint64_t t) {
  return ui.owner && ui.shown && t >= ui.shown + 300 && t <= ui.frame + 1200;
}
bool pet_accessory_edit(int delta, bool move, uint64_t t) {
  portENTER_CRITICAL(&lock);
  bool accepted = current_input(t) && ui.view.status != PET_ACCESSORY_WAIT;
  if (accepted) {
    unsigned pos = ui.view.cursor;
    if (move)
      ui.view.cursor = (pos + (delta > 0 ? 1 : 12)) % 13;
    else if (pos == 12)
      ui.view.random = !ui.view.random;
    else
      ui.view.digits[pos] = (ui.view.digits[pos] + (delta > 0 ? 1 : 15)) % 16;
    ui.view.status = PET_ACCESSORY_EDIT;
    ui.view.value = 0;
    ui.down = false;
  }
  portEXIT_CRITICAL(&lock);
  return accepted;
}
void pet_accessory_key(uint32_t owner, bool down, bool click, uint64_t t) {
  bool available = pet_accessory_ready() && !pet_ble_keyboard_enabled();
  portENTER_CRITICAL(&lock);
  if (owner && owner == ui.owner && current_input(t)) {
    if (down)
      ui.down = true;
    else if (click && ui.down) {
      ui.down = false;
      if (++ui.sequence == 0)
        ++ui.sequence;
      ui.view.value = 0;
      ui.approved_at = (uint32_t)t;
      ui.view.status = ui.view.status == PET_ACCESSORY_WAIT ? PET_ACCESSORY_EDIT
                       : available                          ? PET_ACCESSORY_WAIT
                                                            : PET_ACCESSORY_BUSY;
    } else if (!click)
      ui.down = false;
  } else
    ui.down = false;
  portEXIT_CRITICAL(&lock);
}

// Host-queue state lives through cancellation until the connect/disconnect callback.
static struct {
  uint32_t seq, deadline, retry_after;
  uint16_t handle, first, last;
  unsigned kind;
  bool ending, got;
  uint8_t close_attempts;
  int value;
} op;
static bool active(void) {
  portENTER_CRITICAL(&lock);
  bool valid = op.seq && ui.owner && ui.sequence == op.seq && ui.view.status == PET_ACCESSORY_WAIT;
  portEXIT_CRITICAL(&lock);
  return valid && !op.ending && (int32_t)(op.deadline - ms()) > 0 && !pet_ble_keyboard_enabled();
}
static void report(unsigned status) {
  portENTER_CRITICAL(&lock);
  if (op.seq == ui.sequence && ui.owner && ui.view.status == PET_ACCESSORY_WAIT) {
    ui.view.status = status;
    ui.view.value = status == PET_ACCESSORY_VALUE ? op.value : 0;
  }
  portEXIT_CRITICAL(&lock);
}
static void finish(unsigned status) {
  report(status);
  if (op.close_attempts >= 3 || (op.ending && (int32_t)(op.retry_after - ms()) > 0))
    return;
  op.ending = true;
  op.retry_after = ms() + 250;
  // Retain exclusive ownership until the GAP completion or a host reset.
  // Retry only rejected teardown commands, with a finite controller budget.
  int rc = op.handle != BLE_HS_CONN_HANDLE_NONE
               ? ble_gap_terminate(op.handle, BLE_ERR_REM_USER_CONN_TERM)
               : ble_gap_conn_cancel();
  op.close_attempts = rc ? op.close_attempts + 1 : 3;
}
static bool same(uint16_t handle, void *arg) {
  return op.seq == (uint32_t)(uintptr_t)arg && handle == op.handle && active();
}
static int read_cb(uint16_t handle, const struct ble_gatt_error *err, struct ble_gatt_attr *attr,
                   void *arg) {
  if (!same(handle, arg))
    return BLE_HS_EAPP;
  if (err->status == BLE_HS_EDONE) {
    finish(op.got ? PET_ACCESSORY_VALUE : PET_ACCESSORY_FAILED);
    return 0;
  }
  uint8_t bytes[2];
  if (err->status || op.got || !attr || !attr->om || attr->handle < op.first ||
      attr->handle > op.last || OS_MBUF_PKTLEN(attr->om) > sizeof(bytes) ||
      os_mbuf_copydata(attr->om, 0, OS_MBUF_PKTLEN(attr->om), bytes) ||
      !pet_accessory_decode(op.kind, bytes, OS_MBUF_PKTLEN(attr->om), &op.value)) {
    finish(PET_ACCESSORY_FAILED);
    return BLE_HS_EAPP;
  }
  op.got = true;
  return 0;
}
static int svc_cb(uint16_t handle, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc,
                  void *arg) {
  if (!same(handle, arg))
    return BLE_HS_EAPP;
  if (err->status == BLE_HS_EDONE && op.first) {
    ble_uuid16_t chr = BLE_UUID16_INIT(op.kind == PET_ACCESSORY_BATTERY ? 0x2a19 : 0x2a6e);
    if (!ble_gattc_read_by_uuid(handle, op.first, op.last, &chr.u, read_cb, arg))
      return 0;
  } else if (!err->status && svc && !op.first && svc->start_handle &&
             svc->end_handle >= svc->start_handle &&
             ble_uuid_u16(&svc->uuid.u) == (op.kind == PET_ACCESSORY_BATTERY ? 0x180f : 0x181a)) {
    op.first = svc->start_handle;
    op.last = svc->end_handle;
    return 0;
  }
  finish(PET_ACCESSORY_FAILED);
  return BLE_HS_EAPP;
}
static int accessory_gap(struct ble_gap_event *event, void *arg) {
  if (!op.seq || (uint32_t)(uintptr_t)arg != op.seq)
    return 0;
  if (event->type == BLE_GAP_EVENT_CONNECT) {
    if (event->connect.status) {
      report(PET_ACCESSORY_FAILED);
      op.seq = 0;
    } else {
      op.handle = event->connect.conn_handle;
      op.close_attempts = 0;
      op.retry_after = ms();
      ble_uuid16_t service = BLE_UUID16_INIT(op.kind == PET_ACCESSORY_BATTERY ? 0x180f : 0x181a);
      if (!active() || ble_gattc_disc_svc_by_uuid(op.handle, &service.u, svc_cb, arg))
        finish(PET_ACCESSORY_FAILED);
    }
  } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
    report(PET_ACCESSORY_FAILED);
    op.seq = 0;
  }
  return 0;
}

// NimBLE's optional auto-pair flag only covers outgoing GATT. A peripheral can
// independently send SMP Security Request. Filter at the common inbound ACL
// boundary on both pinned SDKs, before SMP or the local GATT server sees it.
int __real_ble_hs_hci_evt_acl_process(struct os_mbuf *packet);
int __wrap_ble_hs_hci_evt_acl_process(struct os_mbuf *packet) {
  if (!op.seq)
    return __real_ble_hs_hci_evt_acl_process(packet);
  uint8_t header[9];
  if (op.handle != BLE_HS_CONN_HANDLE_NONE && active() &&
      !os_mbuf_copydata(packet, 0, sizeof(header), header) &&
      pet_accessory_response(header, OS_MBUF_PKTLEN(packet), op.handle))
    return __real_ble_hs_hci_evt_acl_process(packet);
  os_mbuf_free_chain(packet);
  finish(PET_ACCESSORY_FAILED);
  return 0;
}

bool pet_accessory_poll(uint8_t own_type, bool keyboard_busy, const uint8_t *keyboard_identity) {
  if (op.seq) {
    if (!active() || keyboard_busy)
      finish(PET_ACCESSORY_FAILED);
    return true;
  }
  uint32_t now = ms();
  ble_addr_t address = {0};
  portENTER_CRITICAL(&lock);
  if (ui.view.status == PET_ACCESSORY_WAIT && now - ui.approved_at > 500)
    ui.view.status = PET_ACCESSORY_FAILED;
  if (ui.owner && ui.view.status == PET_ACCESSORY_WAIT) {
    memset(&op, 0, sizeof(op));
    op.seq = ui.sequence;
    op.kind = ui.kind;
    op.handle = BLE_HS_CONN_HANDLE_NONE;
    address.type = ui.view.random ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
    for (unsigned i = 0; i < 6; i++)
      address.val[5 - i] = (ui.view.digits[2 * i] << 4) | ui.view.digits[2 * i + 1];
  }
  portEXIT_CRITICAL(&lock);
  if (!op.seq)
    return false;
  if (keyboard_busy || !pet_accessory_ready() ||
      (keyboard_identity && !memcmp(keyboard_identity, address.val, 6))) {
    report(PET_ACCESSORY_BUSY);
    op.seq = 0;
    return false;
  }
  uint8_t none[6] = {0}, ones[6] = {255, 255, 255, 255, 255, 255};
  if (!memcmp(address.val, none, 6) || !memcmp(address.val, ones, 6) ||
      (address.type == BLE_ADDR_RANDOM &&
       ((address.val[5] & 0xc0) != 0xc0 ||
        (address.val[5] == 0xc0 && !memcmp(address.val, none, 5))))) {
    report(PET_ACCESSORY_ADDRESS);
    op.seq = 0;
    return false;
  }
  op.deadline = ms() + 8000;
  if (ble_gap_connect(own_type, &address, 4000, NULL, accessory_gap, (void *)(uintptr_t)op.seq)) {
    report(PET_ACCESSORY_FAILED);
    op.seq = 0;
    return false;
  }
  return true;
}
void pet_accessory_reset(void) {
  atomic_store(&capable, false);
  portENTER_CRITICAL(&lock);
  ui.down = false;
  if (ui.view.status == PET_ACCESSORY_WAIT)
    ui.view.status = PET_ACCESSORY_FAILED;
  portEXIT_CRITICAL(&lock);
  op.seq = 0;
}
void pet_accessory_sync(void) {
  uint8_t states[8] = {0};
  // C6 Hosted chip id and LE Read Supported States: initiating state (bit 6).
  atomic_store(&capable, pet_p4_hosted_radio_chip_id() == 0x0d &&
                             !ble_hs_hci_cmd_tx(0x201c, NULL, 0, states, sizeof(states)) &&
                             (states[0] & 0x40));
}
#elif PET_P4_SYSTEM_WIFI && CONFIG_BT_NIMBLE_ENABLED
struct os_mbuf;
int __real_ble_hs_hci_evt_acl_process(struct os_mbuf *packet);
int __wrap_ble_hs_hci_evt_acl_process(struct os_mbuf *packet) {
  return __real_ble_hs_hci_evt_acl_process(packet);
}
#endif
