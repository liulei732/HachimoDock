/* 640×480 presentation for the same board, input rules and save slots. */
static const int ink = 0xffedd0;
static const int muted = 0xdcc4a5;
static const int gold = 0xf4bf79;

static void label(int x, int y, int px, int rgb, const char *value) {
    unsigned length = 0;
    while (value[length]) ++length;
    hachi_text(x, y, px, rgb, value, length);
}

static void decimal(int x, int y, int px, int rgb, uint32_t value, int centered) {
    char digits[11];
    unsigned start = sizeof(digits) - 1;
    digits[start] = 0;
    do {
        digits[--start] = '0' + value % 10;
        value /= 10;
    } while (value);
    if (centered) x -= (int)(sizeof(digits) - 1 - start) * px * 9 / 32;
    label(x, y, px, rgb, digits + start);
}

static void panel(int x, int y, int w, int h, int border, int face) {
    hachi_rect(x, y, w, h, border);
    hachi_rect(x + 1, y + 1, w - 2, h - 2, face);
}

static void draw_tile(int x, int y, unsigned power) {
    static const int fills[] = {
        0xe6cfaa, 0xf2d29f, 0xdba777, 0xcf8a61, 0xbc6d50, 0xa9573d, 0x96493c,
        0x86634b, 0x875b3d, 0x8d6135, 0xb67e44, 0x9b6937, 0x805b3c, 0x73543e, 0x684b35,
    };
    const int left = x * 2, top = y * 2;
    const int px = power < 7 ? 34 : power < 10 ? 28 : 22;
    panel(left, top, 80, 70, 0xa78459, fills[power - 1]);
    decimal(left + 40, top + (70 - px) / 2, px, power <= 4 ? 0x342319 : ink, 1u << power, 1);
}

static void native_header(void) {
    panel(17, 15, 606, 75, 0x694a35, 0x2d2723);
    label(32, 22, 48, ink, "2048");
    label(223, 24, 25, ink, "摇杆往哪推，整盘往哪滑");
    label(224, 59, 19, gold, "相同数字合并，例如 2 + 2 = 4");
}

static void native_restart(void) {
    panel(32, 106, 576, 328, 0x997858, 0x33271f);
    label(205, 145, 33, ink, "开启新的一局？");
    label(184, 209, 24, muted, "当前棋盘和分数会清空");
    label(224, 260, 24, gold, "最高分会保留");
    panel(80, 344, 224, 76, 0xffd398, 0xb98256);
    panel(336, 344, 224, 76, 0x997858, 0x4a382c);
    label(118, 368, 25, 0x27190f, "1  确认重开");
    label(398, 368, 25, ink, "2  取消");
}

void hachi_frame(int elapsed_ms) {
    slide = slide > elapsed_ms ? slide - elapsed_ms : 0;
    hachi_rect(0, 0, HACHI_WIDTH, HACHI_HEIGHT, 0x221d1a);
    native_header();
    if (screen == RESTART) {
        native_restart();
        return;
    }

    panel(17, 104, 371, 320, 0x765238, 0x32271f);
    for (int i = 0; i < 16; ++i) {
        panel((BOARD_X + (i % 4) * CELL) * 2, (BOARD_Y + (i / 4) * ROW) * 2,
              80, 70, 0x624936, 0x3a2e25);
    }
    draw_board();

    panel(402, 104, 221, 151, 0x765238, 0x2d2723);
    label(414, 112, 21, muted, "当前分");
    label(414, 150, 21, muted, "最高分");
    label(414, 188, 21, muted, legacy_steps ? "本段步数" : "移动步数");
    decimal(506, 111, 19, ink, score, 0);
    decimal(506, 149, 19, ink, best, 0);
    decimal(506, 187, 19, ink, moves, 0);
    const char *grade = largest >= TARGET ? "2048 达标" : largest >= 10 ? "1024 熟练" :
        largest >= 9 ? "512 进阶" : largest >= 7 ? "128 入门" : "挑战 2048";
    label(414, 224, 19, gold, grade);

    static const char *const arrows[] = {"左", "右", "上", "下"};
    for (unsigned i = 0; i < 4; ++i) {
        const direction_button_t *button = &direction_buttons[i];
        const int x = button->x * 2, y = button->y * 2;
        const int active = last_direction == button->key;
        panel(x, y, 107, 47, active ? gold : 0x765238, active ? 0x775037 : 0x49362a);
        label(x + 42, y + 11, 22, ink, arrows[i]);
    }
    label(410, 372, 19, gold, over ? "没有可移动方向" : blocked ? "这个方向滑不动" : "滑动或点方向");
    label(410, 400, 18, muted, "四向都卡住就结束");
    panel(404, 423, 107, 47, 0xa27653, 0x6d4833);
    label(421, 434, 22, ink, "新游戏");
    panel(516, 423, 107, 47, 0xa27653, previous.valid ? 0x6d4833 : 0x32271f);
    label(548, 434, 22, previous.valid ? ink : muted, "撤回");
    label(42, 432, 19, muted, "128 入门  /  512 进阶");
    label(42, 455, 19, muted, "1024 熟练  /  2048 达标");
}
