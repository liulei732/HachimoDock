/* One cache slot per visible card. Storage and built-in streams use the
 * same 16-entry little-endian RGB565 palette and high-nibble-first indices. */
enum { CENTER_THUMB_W = 64, CENTER_THUMB_H = 48, CENTER_THUMB_MAX_ATTEMPTS = 3,
       CENTER_THUMB_BYTES = 32 + CENTER_THUMB_W * CENTER_THUMB_H / 2 };
_Static_assert(CENTER_THUMB_BYTES == PET_CENTER_THUMB_BYTES, "launcher thumbnail format");
typedef struct {
  char id[sizeof(((pet_p4_miniapp_catalog_entry_t *)0)->widget_id)];
  uint32_t checksum;
  uint8_t slot, generation, attempts;
  bool ready, tried_builtin;
  int64_t retry_after_us;
  uint16_t pixels[CENTER_THUMB_W * CENTER_THUMB_H];
} center_thumb_card_t;
static struct {
  center_thumb_card_t cards[PET_CENTER_PAGE_SIZE];
  uint8_t packed[CENTER_THUMB_BYTES];
} *center_thumb_page;
static tinfl_decompressor *center_thumb_inflater;

static void center_thumbnails_prepare(void) {
  center_thumb_page = heap_caps_calloc(1, sizeof(*center_thumb_page), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void center_thumbnails_release(void) {
  if (center_thumb_page) heap_caps_free(center_thumb_page);
  if (center_thumb_inflater) heap_caps_free(center_thumb_inflater);
  center_thumb_page = NULL;
  center_thumb_inflater = NULL;
}

static bool center_read_builtin_thumbnail(const char *id, uint8_t *packed) {
  const size_t blob_size = center_thumbs_z_end - center_thumbs_z;
  for (size_t i = 0; i < CENTER_ART_COUNT; ++i) {
    if (!center_art_catalog[i].art || strcmp(id, center_art_catalog[i].id)) continue;
    size_t offset = center_art_catalog[i].offset, length = center_art_catalog[i].length;
    if (offset > blob_size || !length || length > blob_size - offset) return false;
    if (!center_thumb_inflater) center_thumb_inflater = heap_caps_malloc(
        sizeof(*center_thumb_inflater), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!center_thumb_inflater) return false;
    tinfl_init(center_thumb_inflater);
    size_t consumed = length, decoded = CENTER_THUMB_BYTES;
    tinfl_status status = tinfl_decompress(center_thumb_inflater, center_thumbs_z + offset,
        &consumed, packed, packed, &decoded,
        TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    return status == TINFL_STATUS_DONE && decoded == CENTER_THUMB_BYTES && consumed == length;
  }
  return false;
}

static void center_unpack_thumbnail(const uint8_t *packed, uint16_t *pixels) {
  uint16_t palette[16];
  for (size_t i = 0; i < 16; ++i) palette[i] = packed[2*i] | ((uint16_t)packed[2*i+1] << 8);
  for (size_t i = 0; i < CENTER_THUMB_W * CENTER_THUMB_H / 2; ++i) {
    const uint8_t pair = packed[32+i];
    pixels[2*i] = palette[pair >> 4];
    pixels[2*i+1] = palette[pair & 15];
  }
}

typedef enum { CENTER_THUMB_MISSING, CENTER_THUMB_PENDING, CENTER_THUMB_DRAWN } center_thumb_state_t;

static center_thumb_state_t center_draw_thumbnail(const pet_p4_miniapp_catalog_entry_t *item, unsigned visible_slot,
    int x, int y, bool *allow_storage_read) {
  if (!center_thumb_page) return CENTER_THUMB_MISSING;
  center_thumb_card_t *card = &center_thumb_page->cards[visible_slot];
  if (strcmp(card->id, item->widget_id) || card->checksum != item->center_thumb_checksum
      || card->generation != item->center_thumb_generation || card->slot != item->center_title_slot) {
    memcpy(card->id, item->widget_id, sizeof(card->id));
    card->checksum = item->center_thumb_checksum;
    card->generation = item->center_thumb_generation;
    card->slot = item->center_title_slot;
    card->attempts = 0;
    card->retry_after_us = 0;
    card->ready = false;
    card->tried_builtin = false;
  }
  uint8_t *packed = center_thumb_page->packed;
  if (!card->ready && card->checksum && card->attempts < CENTER_THUMB_MAX_ATTEMPTS) {
    if (!*allow_storage_read || esp_timer_get_time() < card->retry_after_us || center_thumbnail_storage_busy())
      return CENTER_THUMB_PENDING;
    *allow_storage_read = false;
    if (pet_p4_miniapp_center_read_thumb(item, packed, CENTER_THUMB_BYTES)) {
      center_unpack_thumbnail(packed, card->pixels);
      card->ready = true;
    } else if (++card->attempts < CENTER_THUMB_MAX_ATTEMPTS) {
      card->retry_after_us = esp_timer_get_time() + 1000000;
      return CENTER_THUMB_PENDING;
    }
  }
  /* Firmware art is memory mapped. Decode all visible built-ins before the
   * frame is submitted; only file reads share the one-per-frame budget. */
  if (!card->ready && !card->tried_builtin) {
    card->tried_builtin = true;
    if (center_read_builtin_thumbnail(card->id, packed)) {
      center_unpack_thumbnail(packed, card->pixels);
      card->ready = true;
    }
  }
  if (!card->ready) return CENTER_THUMB_MISSING;
  /* Exact row spans of the original radius-seven clip. */
  static const uint8_t insets[] = {0, 1, 1, 1, 2, 3, 4, 7};
  for (int row = 0; row < CENTER_THUMB_H; ++row) {
    int inset = insets[row < 7 ? 7-row : row > 40 ? row-40 : 0];
    memcpy(g_framebuffer + (y+row)*PET_P4_UI_WIDTH + x+inset,
        card->pixels + row*CENTER_THUMB_W + inset, (CENTER_THUMB_W-2*inset)*sizeof(uint16_t));
  }
  return CENTER_THUMB_DRAWN;
}
