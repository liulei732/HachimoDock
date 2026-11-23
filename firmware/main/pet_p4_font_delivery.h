#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

/* Both calls enter and return with the state lock. LAN also lends its lock
 * across storage I/O, reacquiring it after state to recheck the same pairing. */
typedef struct {
  void *owner;
  uint32_t generation;
  void (*unlock)(void);
  bool (*resume)(void *owner, uint32_t generation);
} pet_font_route_t;

bool pet_p4_font_write(const char *id, pet_font_route_t route, size_t offset,
                       const uint8_t *bytes, size_t size);
cJSON *pet_p4_font_delivery(const cJSON *payload, pet_font_route_t route,
                            const char *board, bool admitted);
