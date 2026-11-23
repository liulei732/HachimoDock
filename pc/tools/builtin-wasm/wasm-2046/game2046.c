#define HACHI_NATIVE_CANVAS 1
#include "hachi.h"
static void draw_board(void);

enum { BOARD_X = 14, BOARD_Y = 56, CELL = 44, ROW = 40, TARGET = 11, MAX_TILE = 15, SLIDE_MS = 115 };

typedef struct { uint8_t from, to, power; } motion_t;
static uint8_t board[16], moving_count;
static motion_t motion[16];
static uint32_t score, best, random_state, moves;
static struct {
    uint8_t tiles[16];
    uint32_t score, moves, random_state;
    int valid;
} previous;
typedef enum { PLAY, RESTART } screen_t;
enum { PROGRESS_SLOT = 7, V12_TUTORIAL_DISMISSED = 1, LEGACY_STEPS = 2, MAX_MOVES = 0x1fffffff };
typedef struct { int x, y, key; } direction_button_t;
static const direction_button_t direction_buttons[] = {
    {202, 133, HACHI_LEFT}, {258, 133, HACHI_RIGHT},
    {202, 159, HACHI_UP}, {258, 159, HACHI_DOWN}
};
static screen_t screen;
static int slide, touch_x, touch_y, tracking, largest, over, blocked;
static int last_direction, legacy_steps;

static uint32_t random_next(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static void spawn(void) {
    uint8_t free_slots[16];
    unsigned count = 0;
    for (int i = 0; i < 16; ++i) if (!board[i]) free_slots[count++] = i;
    if (count) {
        unsigned slot = random_next() % count;
        board[free_slots[slot]] = random_next() % 10 ? 1 : 2;
    }
}

static void refresh_state(void) {
    over = 1;
    largest = 0;
    for (int i = 0; i < 16; ++i) {
        if (board[i] > largest) largest = board[i];
        if (!board[i] ||
            (board[i] < MAX_TILE && ((i % 4 != 3 && board[i] == board[i + 1]) ||
                                     (i < 12 && board[i] == board[i + 4])))) over = 0;
    }
}

static void save(void) {
    for (int y = 0; y < 4; ++y) {
        unsigned row = 0;
        for (int x = 0; x < 4; ++x) row |= (unsigned)board[y * 4 + x] << (4 * x);
        hachi_save(y, row);
    }
    hachi_save(4, (int)score);
    hachi_save(5, (int)best);
    hachi_save(6, (int)random_state);
    // Keep the v1.2 progress layout, including its tutorial bit for downgrades.
    hachi_save(PROGRESS_SLOT, (int)((moves << 2) | (unsigned)legacy_steps | V12_TUTORIAL_DISMISSED));
}

static void reset(void) {
    for (int i = 0; i < 16; ++i) board[i] = 0;
    score = moves = slide = blocked = last_direction = 0;
    previous.valid = 0;
    legacy_steps = 0;
    screen = PLAY;
    spawn();
    spawn();
    refresh_state();
    save();
}

void hachi_init(void) {
    hachi_canvas640();
    unsigned any = 0;
    for (int y = 0; y < 4; ++y) {
        unsigned row = (unsigned)hachi_load(y, 0);
        any |= row & 0xffff;
        for (int x = 0; x < 4; ++x) board[y * 4 + x] = (row >> (4 * x)) & 15;
    }
    score = (unsigned)hachi_load(4, 0);
    best = (unsigned)hachi_load(5, 0);
    random_state = (unsigned)hachi_load(6, 0x28f340f9);
    int progress = hachi_load(PROGRESS_SLOT, -1);
    if (progress >= 0) {
        legacy_steps = progress & LEGACY_STEPS;
        moves = (unsigned)progress >> 2;
    } else if (any) {
        legacy_steps = LEGACY_STEPS;
    }
    if (!random_state) random_state = 1;
    if (any) refresh_state();
    else reset();
}

static int at(int direction, int line, int offset) {
    switch (direction) {
    case HACHI_LEFT: return 4 * line + offset;
    case HACHI_RIGHT: return 4 * line + 3 - offset;
    case HACHI_UP: return line + 4 * offset;
    default: return line + 4 * (3 - offset);
    }
}

static void move(int direction) {
    if (over) return;
    last_direction = direction;
    uint8_t next[16] = {0};
    motion_t steps[16];
    unsigned step_count = 0;
    unsigned gain = 0;
    for (int line = 0; line < 4; ++line) {
        int src[4], count = 0;
        for (int offset = 0; offset < 4; ++offset) {
            int pos = at(direction, line, offset);
            if (board[pos]) src[count++] = pos;
        }
        for (int from = 0, to = 0; from < count; ++from, ++to) {
            int destination = at(direction, line, to);
            int current = src[from];
            unsigned power = board[current];
            steps[step_count++] = (motion_t){current, destination, power};
            if (from + 1 < count && power < MAX_TILE && board[src[from + 1]] == power) {
                steps[step_count++] = (motion_t){src[++from], destination, power};
                gain += 1u << ++power;
            }
            next[destination] = power;
        }
    }
    int changed = 0;
    for (int i = 0; i < 16; ++i) changed |= next[i] != board[i];
    if (!changed) {
        blocked = 1;
        return;
    }
    blocked = 0;
    for (int i = 0; i < 16; ++i) previous.tiles[i] = board[i];
    previous.score = score;
    previous.moves = moves;
    previous.random_state = random_state;
    previous.valid = 1;
    if (moves < MAX_MOVES) ++moves;
    for (int i = 0; i < 16; ++i) board[i] = next[i];
    for (unsigned i = 0; i < step_count; ++i) motion[i] = steps[i];
    moving_count = step_count;
    slide = SLIDE_MS;
    score = score > 0x7fffffffu - gain ? 0x7fffffffu : score + gain;
    if (score > best) best = score;
    spawn();
    refresh_state();
    hachi_tone(gain ? 590 : 370, gain ? 55 : 30, gain ? 19 : 11);
    save();
}

static void choose(int key) {
    if (screen == RESTART) {
        if (key == HACHI_A) reset();
        if (key == HACHI_B) screen = PLAY;
    } else if (key == HACHI_A) {
        screen = RESTART;
    } else if (key == HACHI_B && previous.valid) {
        for (int i = 0; i < 16; ++i) board[i] = previous.tiles[i];
        score = previous.score;
        moves = previous.moves;
        random_state = previous.random_state;
        previous.valid = slide = blocked = last_direction = 0;
        refresh_state();
        save();
    } else if (key >= HACHI_LEFT && key <= HACHI_DOWN) {
        move(key);
    }
}

static int magnitude(int v) { return v < 0 ? -v : v; }

static int hit(int x, int y, int left, int top, int width, int height) {
    return x >= left && x < left + width && y >= top && y < top + height;
}

static void tap(int x, int y) {
    if (screen == RESTART) {
        if (hit(x, y, 40, 172, 112, 38)) choose(HACHI_A);
        if (hit(x, y, 168, 172, 112, 38)) choose(HACHI_B);
    } else {
        for (unsigned i = 0; i < sizeof(direction_buttons) / sizeof(direction_buttons[0]); ++i) {
            const direction_button_t *button = &direction_buttons[i];
            if (hit(x, y, button->x, button->y, 54, 24)) choose(button->key);
        }
        if (hit(x, y, 202, 211, 54, 24)) choose(HACHI_A);
        if (hit(x, y, 258, 211, 54, 24)) choose(HACHI_B);
    }
}

void hachi_event(int type, int a, int b) {
    if (type >= HACHI_TOUCH_DOWN && type <= HACHI_TOUCH_UP) {
        a /= 2;
        b /= 2;
    }
    if (type == HACHI_KEY_DOWN) {
        tracking = 0;
        choose(a);
    } else if (type == HACHI_TOUCH_CANCEL) {
        tracking = 0;
    } else if (type == HACHI_TOUCH_DOWN) {
        touch_x = a;
        touch_y = b;
        tracking = 1;
    } else if (type == HACHI_TOUCH_UP && tracking) {
        tracking = 0;
        int dx = a - touch_x, dy = b - touch_y;
        if (magnitude(dx) >= 16 || magnitude(dy) >= 16) {
            if (screen == PLAY && hit(touch_x, touch_y, 8, 51, 186, 166)) {
                choose(magnitude(dx) >= magnitude(dy) ? (dx > 0 ? HACHI_RIGHT : HACHI_LEFT)
                                                       : (dy > 0 ? HACHI_DOWN : HACHI_UP));
            }
        } else if (magnitude(dx) <= 8 && magnitude(dy) <= 8) {
            tap(a, b);
        }
    }
}

#include "game2046_native.h"

static void draw_board(void) {
    if (!slide) {
        for (int i = 0; i < 16; ++i)
            if (board[i]) draw_tile(BOARD_X + (i % 4) * CELL, BOARD_Y + (i / 4) * ROW, board[i]);
    } else {
        for (unsigned i = 0; i < moving_count; ++i) {
            motion_t m = motion[i];
            int dx = (int)(m.from % 4) - (int)(m.to % 4);
            int dy = (int)(m.from / 4) - (int)(m.to / 4);
            draw_tile(BOARD_X + (m.to % 4) * CELL + dx * CELL * slide / SLIDE_MS,
                      BOARD_Y + (m.to / 4) * ROW + dy * ROW * slide / SLIDE_MS, m.power);
        }
    }
}
