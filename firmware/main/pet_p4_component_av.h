#pragma once
#include "cJSON.h"
#include "esp_err.h"
#include <stdbool.h>
#define PET_NOTE_PREFIX "lan-note-"
esp_err_t pet_p4_note_start(const char *owner);
void pet_p4_note_discard(const char *owner);
cJSON *pet_p4_note_read(const char *owner, unsigned cursor);
cJSON *pet_p4_media_lan(const char *topic, const cJSON *payload, const char *source,
                        const char *instance, bool cleanup);

bool pet_p4_media_radio_matches(const char *source,const char *instance);
cJSON *pet_p4_media_radio_poll(uint32_t generation,uint32_t association);
