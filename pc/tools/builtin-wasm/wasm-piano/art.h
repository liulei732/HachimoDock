#ifndef HACHI_EXAMPLE_ART_H
#define HACHI_EXAMPLE_ART_H
#include "hachi.h"
#include "app_art.h"

typedef struct {
    const uint8_t *data;
    unsigned size;
} art_t;

#define ART_DATA(name) {art_##name, sizeof(art_##name)}
#define ART(name, x, y) hachi_sprite(x, y, name##_w, name##_h, art_##name, sizeof(art_##name))

static inline void art_draw(art_t asset, int x, int y, int width, int height) {
    hachi_sprite(x, y, width, height, asset.data, asset.size);
}
#endif
