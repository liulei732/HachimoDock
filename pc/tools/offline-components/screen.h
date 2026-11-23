#ifndef OFFLINE_SCREEN_H
#define OFFLINE_SCREEN_H
#include "hachi.h"

static void label(int x, int y, int size, int color, const char *s) {
    unsigned n = 0;
    while (s[n] && n < 127) ++n;
    if (n) hachi_text(x, y, size, color, s, n);
}

static void number(int x, int y, unsigned n) {
    char buf[12];
    unsigned i = sizeof(buf) - 1;
    buf[i] = 0;
    do { buf[--i] = '0' + n % 10; n /= 10; } while (n);
    label(x, y, 20, 0x30665a, buf+i);
}
#endif
