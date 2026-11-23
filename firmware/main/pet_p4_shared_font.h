#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Fixed, optional component-storage resource. Published once after boot or verified provisioning. */
#ifdef ESP_PLATFORM
void pet_p4_font_boot_done(void);
void pet_p4_font_delivery_expire(void);
bool pet_shared_font_ready(void);
#else
static inline bool pet_shared_font_ready(void) { return false; }
#endif
bool pet_shared_font_activate_staged(const char *path);
void pet_shared_font_load(void);
typedef struct {
    const uint8_t *bits;
    uint8_t width, height;
    int8_t left, bottom;
} pet_shared_glyph_t;
bool pet_shared_font_lookup(uint32_t cp, pet_shared_glyph_t *glyph);

/* Four source cells span three destination pixels. Edge cells outside the
 * glyph have zero coverage. A2 values are converted to alpha after filtering. */
static inline unsigned pet_shared_level(const pet_shared_glyph_t *g, unsigned x, unsigned y) {
    if (x >= g->width || y >= g->height) return 0;
    unsigned index = y * g->width + x;
    return (g->bits[index / 4] >> (6 - 2 * (index % 4))) & 3;
}
static inline unsigned pet_shared_alpha(const pet_shared_glyph_t *g, unsigned x, unsigned y, bool medium) {
    if (!medium) return pet_shared_level(g, x, y) * 85;
    unsigned sx = x * 4 / 3, sy = y * 4 / 3;
    unsigned wx = 3 - x % 3, wy = 3 - y % 3;
    unsigned top = pet_shared_level(g, sx, sy) * wx + pet_shared_level(g, sx+1, sy) * (4-wx);
    unsigned bottom = pet_shared_level(g, sx, sy+1) * wx + pet_shared_level(g, sx+1, sy+1) * (4-wx);
    return ((top * wy + bottom * (4-wy)) * 85 + 8) / 16;
}
