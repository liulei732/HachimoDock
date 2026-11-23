#include "art.h"

enum { KEY_X = 16, KEY_Y = 100, KEY_W = 36, KEY_BOTTOM = 216 };
static const int notes[] = {262, 294, 330, 349, 392, 440, 494, 523};
static int active = -1;
static int touch_started, touch_note = -1;
static int selection;
static int octave;
static int flash_ms;
static unsigned clock_ms;

static void play(int note) {
    selection = active = note;
    flash_ms = 240;
    hachi_tone(notes[note] << octave, 170, 48);
}

static void change_octave(void) {
    octave = !octave;
    hachi_save(0, octave);
    active = -1;
    play(selection);
}

void hachi_init(void) {
    octave = hachi_load(0, 0) == 1;
}

void hachi_event(int type, int a, int b) {
    if (type == HACHI_TOUCH_UP || type == HACHI_TOUCH_CANCEL) {
        touch_started = 0;
        touch_note = -1;
        return;
    }
    if (type == HACHI_TOUCH_DOWN || type == HACHI_TOUCH_MOVE) {
        int on_keys = a >= KEY_X && a < KEY_X + 8 * KEY_W && b >= KEY_Y && b < KEY_BOTTOM;
        if (type == HACHI_TOUCH_DOWN) {
            touch_started = on_keys;
            touch_note = -1;
            if (a >= 222 && a < 302 && b >= 14 && b < 73) change_octave();
        }
        int note = on_keys ? (a - KEY_X) / KEY_W : -1;
        if (touch_started && note >= 0 && note != touch_note) play(note);
        touch_note = note;
    } else if (type == HACHI_KEY_DOWN) {
        if (a == HACHI_LEFT) selection = (selection + 7) % 8;
        if (a == HACHI_RIGHT) selection = (selection + 1) % 8;
        if (a == HACHI_A) play(selection);
        if (a == HACHI_B) change_octave();
    }
}

void hachi_frame(int elapsed_ms) {
    clock_ms += (unsigned)elapsed_ms;
    flash_ms = flash_ms > elapsed_ms ? flash_ms - elapsed_ms : 0;
    if (!flash_ms) active = -1;

    static const art_t keys[][2] = {
        {ART_DATA(key_0_0), ART_DATA(key_0_1)}, {ART_DATA(key_1_0), ART_DATA(key_1_1)},
        {ART_DATA(key_2_0), ART_DATA(key_2_1)}, {ART_DATA(key_3_0), ART_DATA(key_3_1)},
        {ART_DATA(key_4_0), ART_DATA(key_4_1)}, {ART_DATA(key_5_0), ART_DATA(key_5_1)},
        {ART_DATA(key_6_0), ART_DATA(key_6_1)}, {ART_DATA(key_7_0), ART_DATA(key_7_1)},
    };
    ART(backdrop, 0, 0);
    if (octave) { ART(high, 226, 38); }
    else { ART(low, 226, 38); }
    for (int i = 0; i < 8; ++i) {
        int h = active >= 0 ? 5 + (clock_ms / 18 + i * 7) % 22 : 3 + i % 3;
        hachi_rect(152 + i * 7, 66 - h, 4, h, i == active ? 0xf4c188 : 0x9e7462);
        art_draw(keys[i][i == active], KEY_X + i * KEY_W, KEY_Y, key_0_0_w, key_0_0_h);
    }
    for (int i = 1; i < 8; ++i)
        if (i != 3 && i != 7) { ART(divider, KEY_X + i * KEY_W - 7, KEY_Y); }
    hachi_rect(KEY_X + selection * KEY_W + 9, 219, 15, 2, 0xdfab78);
}
