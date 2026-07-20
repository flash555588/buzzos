#ifndef BUZZOS_APPUI_H
#define BUZZOS_APPUI_H

#include <stdint.h>
#include "../../kernel/drv/font_builtin.h"

struct appui_rect {
    int x;
    int y;
    int w;
    int h;
};

static int appui_rgb6(int r, int g, int b) {
    if (r < 0) r = 0; if (r > 5) r = 5;
    if (g < 0) g = 0; if (g > 5) g = 5;
    if (b < 0) b = 0; if (b > 5) b = 5;
    return 40 + r * 36 + g * 6 + b;
}

static int appui_gray(int n) {
    if (n < 0) n = 0;
    if (n > 14) n = 14;
    return 25 + n;
}

static __attribute__((unused)) int appui_min(int a, int b) { return a < b ? a : b; }
static __attribute__((unused)) int appui_max(int a, int b) { return a > b ? a : b; }

static int appui_inside(int x, int y, struct appui_rect r) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static void appui_pixel(uint8_t *fb, int w, int h, int x, int y, int color) {
    if (x >= 0 && y >= 0 && x < w && y < h)
        fb[y * w + x] = (uint8_t)color;
}

static void appui_fill(uint8_t *fb, int w, int h, struct appui_rect r, int color) {
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > w) r.w = w - r.x;
    if (r.y + r.h > h) r.h = h - r.y;
    if (r.w <= 0 || r.h <= 0)
        return;
    for (int yy = 0; yy < r.h; yy++) {
        uint8_t *row = fb + (r.y + yy) * w + r.x;
        for (int xx = 0; xx < r.w; xx++)
            row[xx] = (uint8_t)color;
    }
}

static void appui_border(uint8_t *fb, int w, int h, struct appui_rect r, int hi, int lo) {
    appui_fill(fb, w, h, (struct appui_rect){r.x, r.y, r.w, 1}, hi);
    appui_fill(fb, w, h, (struct appui_rect){r.x, r.y, 1, r.h}, hi);
    appui_fill(fb, w, h, (struct appui_rect){r.x, r.y + r.h - 1, r.w, 1}, lo);
    appui_fill(fb, w, h, (struct appui_rect){r.x + r.w - 1, r.y, 1, r.h}, lo);
}

static void appui_text(uint8_t *fb, int w, int h, int x, int y,
                       const char *s, int fg, int bg, struct appui_rect clip) {
    while (s && *s) {
        unsigned char ch = (unsigned char)*s++;
        if (ch == '\n') {
            y += KFONT_HEIGHT;
            x = clip.x;
            continue;
        }
        if (ch < KFONT_FIRST || ch >= KFONT_FIRST + KFONT_COUNT)
            ch = '?';
        if (x >= clip.x + clip.w)
            return;
        if (x + KFONT_WIDTH > clip.x && y + KFONT_HEIGHT > clip.y &&
            x < clip.x + clip.w && y < clip.y + clip.h) {
            const uint8_t *alpha = &kfont_alpha[ch - KFONT_FIRST][0][0];
            for (int py = 0; py < KFONT_HEIGHT; py++) {
                for (int px = 0; px < KFONT_WIDTH; px++) {
                    uint8_t a = alpha[py * KFONT_WIDTH + px];
                    int tx = x + px;
                    int ty = y + py;
                    if (!appui_inside(tx, ty, clip))
                        continue;
                    if (a >= 128)
                        appui_pixel(fb, w, h, tx, ty, fg);
                    else if (bg >= 0)
                        appui_pixel(fb, w, h, tx, ty, bg);
                }
            }
        }
        x += KFONT_WIDTH;
    }
}

static void appui_button(uint8_t *fb, int w, int h, struct appui_rect r,
                         const char *label, int active) {
    int bg = active ? appui_rgb6(0, 3, 5) : appui_gray(4);
    appui_fill(fb, w, h, r, bg);
    appui_border(fb, w, h, r, active ? appui_rgb6(2, 5, 5) : appui_gray(8), appui_gray(1));
    appui_text(fb, w, h, r.x + 8, r.y + 6, label, 15, -1,
               (struct appui_rect){r.x + 4, r.y + 2, r.w - 8, r.h - 4});
}

static __attribute__((unused)) void appui_copy_text(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0)
        return;
    while (i + 1 < cap && src && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void appui_append_text(char *dst, const char *src, int cap) {
    int n = 0;
    while (n < cap && dst[n])
        n++;
    int i = 0;
    while (n + 1 < cap && src && src[i])
        dst[n++] = src[i++];
    if (cap > 0)
        dst[n] = 0;
}

static __attribute__((unused)) void appui_append_int(char *dst, int value, int cap) {
    char tmp[16];
    int n = 0;
    unsigned int v;
    if (value < 0) {
        appui_append_text(dst, "-", cap);
        v = (unsigned int)(-value);
    } else {
        v = (unsigned int)value;
    }
    if (v == 0) {
        appui_append_text(dst, "0", cap);
        return;
    }
    while (v && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0) {
        char s[2];
        s[0] = tmp[--n];
        s[1] = 0;
        appui_append_text(dst, s, cap);
    }
}

#endif
