#include "screen.h"

typedef struct {
    uint32_t key;
    const char *title;
    unsigned start, lines;
} Article;
#include "shelf.h"

enum { PER_PAGE = 7, LIST_ROWS = 6 };
static int selected, page, reading;

static int last_page(void) { return (articles[selected].lines - 1) / PER_PAGE; }

static void bookmark(void) {
    hachi_save(0, (int)articles[selected].key);
    hachi_save(1, page);
}

void hachi_init(void) {
    hachi_canvas640();
    uint32_t key = (uint32_t)hachi_load(0, 0);
    selected = page = reading = 0;
    for (int i = 0; i < ARTICLE_COUNT; ++i) {
        if (articles[i].key != key) continue;
        selected = i;
        page = hachi_load(1, 0);
        if (page < 0) page = 0;
        if (page > last_page()) page = last_page();
        reading = 1;
        break;
    }
}

static void open_article(void) {
    int saved = hachi_load(1, 0);
    page = articles[selected].key == (uint32_t)hachi_load(0, 0)
        && saved >= 0 && saved <= last_page() ? saved : 0;
    reading = 1;
    bookmark();
}

static void step(int dir) {
    if (!reading) {
        selected = (selected + dir + ARTICLE_COUNT) % ARTICLE_COUNT;
    } else if (page + dir >= 0 && page + dir <= last_page()) {
        page += dir;
        bookmark();
    }
}

void hachi_event(int type, int a, int b) {
    if (type == HACHI_TOUCH_UP) {
        if (b >= 406) {
            if (a < 200) step(-1);
            else if (a > 438) step(1);
            else if (reading) reading = 0;
            else open_article();
        } else if (!reading && b >= 84 && b < 84 + LIST_ROWS * 50) {
            int i = selected / LIST_ROWS * LIST_ROWS + (b-84)/50;
            if (i < ARTICLE_COUNT) { selected = i; open_article(); }
        }
    } else if (type == HACHI_KEY_DOWN) {
        if (a == HACHI_B) reading = 0;
        else if (a == HACHI_A && !reading) open_article();
        else if (a == HACHI_LEFT || a == HACHI_UP) step(-1);
        else if (a == HACHI_RIGHT || a == HACHI_DOWN || a == HACHI_A) step(1);
    }
}

void hachi_frame(int ms) {
    (void)ms;
    hachi_rect(0, 0, 640, 480, 0xf9f4df);
    label(24, 22, 24, 0x224c43, reading ? articles[selected].title : SHELF_TITLE);
    hachi_rect(24, 65, 592, 2, 0x578378);
    if (reading) {
        const Article *article = &articles[selected];
        unsigned first = page * PER_PAGE;
        for (unsigned r = 0; r < PER_PAGE && first+r < article->lines; ++r)
            label(28, 84 + r*42, 21, 0x30342c, lines[article->start+first+r]);
        number(445, 372, page+1);
        label(499, 372, 18, 0x48685e, "/");
        number(525, 372, last_page()+1);
    } else {
        for (int r = 0; r < LIST_ROWS; ++r) {
            int i = selected / LIST_ROWS * LIST_ROWS + r;
            if (i >= ARTICLE_COUNT) break;
            if (i == selected) hachi_rect(16, 84+r*50, 608, 45, 0xdde6cc);
            label(28, 92+r*50, 21, 0x30342c, articles[i].title);
        }
    }
    hachi_rect(0, 406, 640, 74, 0x224c43);
    label(34, 429, 20, 0xffffff, reading ? "上一页" : "上一篇");
    label(284, 429, 20, 0xffffff, reading ? "目录" : "阅读");
    label(512, 429, 20, 0xffffff, reading ? "下一页" : "下一篇");
}
