#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "pet_p4_ble_accessory.h"

static inline bool pet_accessory_decode(unsigned kind, const uint8_t *b, size_t n, int *value) {
  if (kind == PET_ACCESSORY_BATTERY && n == 1 && b[0] <= 100) {
    *value = b[0];
    return true;
  }
  if (kind != PET_ACCESSORY_TEMPERATURE || n != 2)
    return false;
  int t = (int16_t)((unsigned)b[0] | (unsigned)b[1] << 8);
  if (t < -27315)
    return false; /* Includes the SIG "unknown" sentinel 0x8000. */
  *value = t;
  return true;
}

// A single small, complete ATT response on the approved outbound connection.
// The authenticated HID connection never enters this code path. Keeping SMP,
// CCCD writes and unsolicited ATT requests out also protects the sole HID bond.
static inline bool pet_accessory_response(const uint8_t h[9], size_t total, uint16_t handle) {
  unsigned flags = h[0] | (unsigned)h[1] << 8;
  unsigned acl = h[2] | (unsigned)h[3] << 8, l2 = h[4] | (unsigned)h[5] << 8;
  return (flags & 0xfff) == handle && flags >> 12 == 2 && acl + 4 == total && l2 + 4 == acl &&
         l2 >= 1 && l2 <= 23 && h[6] == 4 && !h[7] &&
         (h[8] == 0x01 || h[8] == 0x07 ||
          h[8] == 0x09); /* Error, Find By Type Value, Read By Type */
}
