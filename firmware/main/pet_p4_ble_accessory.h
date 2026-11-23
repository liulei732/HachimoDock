#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#if PET_P4_SYSTEM_WIFI && CONFIG_BT_NIMBLE_ROLE_CENTRAL && CONFIG_BT_NIMBLE_GATT_CLIENT
#define PET_BLE_ACCESSORY 1
#else
#define PET_BLE_ACCESSORY 0
#endif

enum { PET_ACCESSORY_BATTERY = 1, PET_ACCESSORY_TEMPERATURE };
enum {
  PET_ACCESSORY_EDIT,
  PET_ACCESSORY_WAIT,
  PET_ACCESSORY_VALUE,
  PET_ACCESSORY_BUSY,
  PET_ACCESSORY_FAILED,
  PET_ACCESSORY_ADDRESS
};

#if PET_BLE_ACCESSORY
typedef struct {
  uint8_t digits[12], cursor, status;
  bool random;
  int value;
} pet_accessory_view_t;

bool pet_accessory_ready(void);
void pet_accessory_select(uint32_t owner, unsigned kind);
void pet_accessory_key(uint32_t owner, bool down, bool click, uint64_t ms);
bool pet_accessory_edit(int direction, bool move, uint64_t ms);
bool pet_accessory_view(uint32_t owner, pet_accessory_view_t *out);
void pet_accessory_shown(uint32_t owner, uint64_t ms);
// All three radio callbacks run on the keyboard's existing NimBLE host queue.
bool pet_accessory_poll(uint8_t own_address_type, bool keyboard_busy,
                        const uint8_t *keyboard_identity);
void pet_accessory_sync(void);
void pet_accessory_reset(void);
#endif
