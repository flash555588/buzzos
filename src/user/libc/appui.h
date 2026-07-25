#ifndef BUZZOS_APPUI_H
#define BUZZOS_APPUI_H

#include <stddef.h>
#include <stdint.h>
#include "libc.h"
#include "palette.h"
#include "../../kernel/drv/font_builtin.h"

struct appui_rect {
    int x;
    int y;
    int w;
    int h;
};

/* Growable RGB32 pixel buffer sized to the *current* window (modern-style),
 * not a static GUIAPP_MAX_W×MAX_H reservation.  Grows with ~25% slack so live
 * resize does not realloc every mouse sample; never exceeds max_w×max_h. */
static __attribute__((unused)) int appui_pixels_ensure(
    uint32_t **pixels, size_t *capacity_px,
    int want_w, int want_h, int max_w, int max_h) {
    if (!pixels || !capacity_px || want_w <= 0 || want_h <= 0 ||
        max_w <= 0 || max_h <= 0)
        return -1;
    if (want_w > max_w) want_w = max_w;
    if (want_h > max_h) want_h = max_h;
    size_t need = (size_t)want_w * (size_t)want_h;
    if (*pixels && need <= *capacity_px)
        return 0;
    size_t hard = (size_t)max_w * (size_t)max_h;
    size_t cap = need + need / 4u;
    if (cap < need || cap > hard)
        cap = hard;
    if (cap < need)
        return -1;
    uint32_t *grown = (uint32_t *)realloc(*pixels, cap * sizeof(uint32_t));
    if (!grown)
        return -1;
    *pixels = grown;
    *capacity_px = cap;
    return 0;
}

enum appui_button_variant {
    APPUI_BTN_DEFAULT,
    APPUI_BTN_PRIMARY,
    APPUI_BTN_DANGER,
    APPUI_BTN_GHOST,
};

enum appui_button_state {
    APPUI_STATE_HOVERED = 1 << 0,
    APPUI_STATE_PRESSED = 1 << 1,
    APPUI_STATE_SELECTED = 1 << 2,
    APPUI_STATE_DISABLED = 1 << 3,
};

/* Convenience aliases — return 0x00RRGGBB. */
static uint32_t appui_rgb6(int r, int g, int b) {
    return plt_cube(r, g, b);
}

static uint32_t appui_gray(int n) {
    return plt_gray(n);
}

static __attribute__((unused)) int appui_min(int a, int b) { return a < b ? a : b; }
static __attribute__((unused)) int appui_max(int a, int b) { return a > b ? a : b; }

static int appui_inside(int x, int y, struct appui_rect r) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static __attribute__((unused)) int appui_pointer_state(
    struct appui_rect r, int x, int y, int buttons) {
    if (!appui_inside(x, y, r))
        return 0;
    return APPUI_STATE_HOVERED |
           ((buttons & 1) ? APPUI_STATE_PRESSED : 0);
}

static void appui_pixel(uint32_t *fb, int w, int h, int x, int y, uint32_t color) {
    if (x >= 0 && y >= 0 && x < w && y < h)
        fb[y * w + x] = color & 0x00FFFFFFu;
}

static void appui_fill(uint32_t *fb, int w, int h, struct appui_rect r,
                       uint32_t color) {
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > w) r.w = w - r.x;
    if (r.y + r.h > h) r.h = h - r.y;
    if (r.w <= 0 || r.h <= 0)
        return;
    color &= 0x00FFFFFFu;
    for (int yy = 0; yy < r.h; yy++) {
        uint32_t *row = fb + (r.y + yy) * w + r.x;
        for (int xx = 0; xx < r.w; xx++)
            row[xx] = color;
    }
}

static __attribute__((unused)) void appui_fill_blend(
    uint32_t *fb, int w, int h, struct appui_rect r, uint32_t color, int alpha) {
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > w) r.w = w - r.x;
    if (r.y + r.h > h) r.h = h - r.y;
    if (r.w <= 0 || r.h <= 0)
        return;
    for (int yy = 0; yy < r.h; yy++) {
        uint32_t *row = fb + (r.y + yy) * w + r.x;
        for (int xx = 0; xx < r.w; xx++)
            row[xx] = plt_blend(color, row[xx], alpha);
    }
}

/* Corner inset (px) per row for a 4px rounded corner. */
static const uint8_t appui_corner[4] = {3, 2, 1, 1};

static void appui_fill_round(uint32_t *fb, int w, int h, struct appui_rect r,
                             uint32_t color) {
    appui_fill(fb, w, h, (struct appui_rect){r.x + 3, r.y, r.w - 6, r.h},
               color);
    for (int i = 0; i < 4; i++) {
        int inset = appui_corner[i];
        int rw = r.w - 2 * inset;
        appui_fill(fb, w, h,
                   (struct appui_rect){r.x + inset, r.y + i, rw, 1}, color);
        appui_fill(fb, w, h,
                   (struct appui_rect){r.x + inset, r.y + r.h - 1 - i, rw, 1},
                   color);
    }
    appui_fill(fb, w, h, (struct appui_rect){r.x, r.y + 4, r.w, r.h - 8},
               color);
}

static void appui_border(uint32_t *fb, int w, int h, struct appui_rect r,
                         uint32_t hi, uint32_t lo) {
    appui_fill(fb, w, h, (struct appui_rect){r.x, r.y, r.w, 1}, hi);
    appui_fill(fb, w, h, (struct appui_rect){r.x, r.y, 1, r.h}, hi);
    appui_fill(fb, w, h, (struct appui_rect){r.x, r.y + r.h - 1, r.w, 1}, lo);
    appui_fill(fb, w, h, (struct appui_rect){r.x + r.w - 1, r.y, 1, r.h}, lo);
}

/* Decode one UTF-8 scalar and always make progress. Invalid input is rendered
 * as U+FFFD, so arbitrary file/network bytes cannot wedge a GUI application. */
static __attribute__((unused)) uint32_t appui_utf8_next(const char **text) {
    const uint8_t *s = (const uint8_t *)*text;
    uint32_t cp;
    int extra;
    if (!s || !*s)
        return 0;
    if (s[0] < 0x80) {
        *text = (const char *)(s + 1);
        return s[0];
    }
    if (s[0] >= 0xC2 && s[0] <= 0xDF) {
        cp = s[0] & 0x1Fu; extra = 1;
    } else if (s[0] >= 0xE0 && s[0] <= 0xEF) {
        cp = s[0] & 0x0Fu; extra = 2;
    } else if (s[0] >= 0xF0 && s[0] <= 0xF4) {
        cp = s[0] & 0x07u; extra = 3;
    } else {
        *text = (const char *)(s + 1);
        return 0xFFFDu;
    }
    for (int i = 1; i <= extra; i++) {
        if (!s[i] || (s[i] & 0xC0u) != 0x80u) {
            *text = (const char *)(s + 1);
            return 0xFFFDu;
        }
        cp = (cp << 6) | (s[i] & 0x3Fu);
    }
    if ((extra == 2 && cp < 0x800u) || (extra == 3 && cp < 0x10000u) ||
        (cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu) {
        *text = (const char *)(s + 1);
        return 0xFFFDu;
    }
    *text = (const char *)(s + extra + 1);
    return cp;
}

static __attribute__((unused)) int appui_utf8_prev(const char *text, int pos) {
    if (!text || pos <= 0) return 0;
    pos--;
    while (pos > 0 && ((uint8_t)text[pos] & 0xC0u) == 0x80u)
        pos--;
    return pos;
}

/* Horizontal tab is a layout control, not a glyph. */
enum { APPUI_TAB_COLUMNS = 4 };

static __attribute__((unused)) int appui_tab_width(void) {
    return KFONT_WIDTH * APPUI_TAB_COLUMNS;
}

static __attribute__((unused)) int appui_tab_advance(int x_from_line_start) {
    int tab = appui_tab_width();
    if (tab <= 0)
        return KFONT_WIDTH;
    if (x_from_line_start < 0)
        x_from_line_start = 0;
    int advance = tab - (x_from_line_start % tab);
    return advance <= 0 ? tab : advance;
}

static __attribute__((unused)) int appui_codepoint_width(uint32_t cp) {
    if (cp == '\t')
        return appui_tab_width();
    if (cp == '\r' || cp == '\n')
        return 0;
    if (cp < 0x80u)
        return KFONT_WIDTH;
    uint8_t bits[FONT_GLYPH_BYTES];
    int width = font_glyph(cp, bits, sizeof(bits));
    return width > 0 ? width : KFONT_WIDTH;
}

static __attribute__((unused)) int appui_codepoint_advance(
    uint32_t cp, int x_from_line_start) {
    if (cp == '\t')
        return appui_tab_advance(x_from_line_start);
    return appui_codepoint_width(cp);
}

static __attribute__((unused)) int appui_draw_codepoint(
    uint32_t *fb, int w, int h, int x, int y, uint32_t cp,
    uint32_t fg, int bg, struct appui_rect clip) {
    if (cp == '\t')
        return appui_tab_width();
    if (cp == '\r' || cp == '\n')
        return 0;

    uint8_t bits[FONT_GLYPH_BYTES];
    const uint8_t *alpha = 0;
    int glyph_w = KFONT_WIDTH;
    y -= PLT_FONT_Y_SHIFT;
    if (cp >= KFONT_FIRST && cp < KFONT_FIRST + KFONT_COUNT) {
        alpha = &kfont_alpha[cp - KFONT_FIRST][0][0];
    } else if (cp >= 0x80u) {
        glyph_w = font_glyph(cp, bits, sizeof(bits));
        if (glyph_w <= 0) {
            cp = '?'; glyph_w = KFONT_WIDTH;
            alpha = &kfont_alpha[cp - KFONT_FIRST][0][0];
        }
    } else {
        cp = '?';
        alpha = &kfont_alpha[cp - KFONT_FIRST][0][0];
    }
    if (x + glyph_w > clip.x && y + KFONT_HEIGHT > clip.y &&
        x < clip.x + clip.w && y < clip.y + clip.h) {
        for (int py = 0; py < KFONT_HEIGHT; py++) {
            for (int px = 0; px < glyph_w; px++) {
                int coverage = alpha ? alpha[py * KFONT_WIDTH + px] :
                    (((bits[py * FONT_GLYPH_STRIDE + px / 8] &
                       (uint8_t)(0x80u >> (px & 7))) != 0) ? 255 : 0);
                int tx = x + px;
                int ty = y + py;
                if (!appui_inside(tx, ty, clip) ||
                    tx < 0 || ty < 0 || tx >= w || ty >= h)
                    continue;
                if (coverage >= 255) {
                    appui_pixel(fb, w, h, tx, ty, fg);
                } else if (coverage <= 0) {
                    if (bg >= 0)
                        appui_pixel(fb, w, h, tx, ty, (uint32_t)bg);
                } else {
                    uint32_t under = bg >= 0 ? (uint32_t)bg : fb[ty * w + tx];
                    appui_pixel(fb, w, h, tx, ty,
                                plt_blend(fg, under, coverage));
                }
            }
        }
    }
    return glyph_w;
}

static __attribute__((unused)) int appui_draw_codepoint_at(
    uint32_t *fb, int w, int h, int x, int y, uint32_t cp,
    uint32_t fg, int bg, struct appui_rect clip, int line_origin_x) {
    if (cp == '\t')
        return appui_tab_advance(x - line_origin_x);
    return appui_draw_codepoint(fb, w, h, x, y, cp, fg, bg, clip);
}

static __attribute__((unused)) int appui_text_width(const char *s) {
    int width = 0;
    while (s && *s) {
        uint32_t cp = appui_utf8_next(&s);
        if (cp == '\n')
            break;
        width += appui_codepoint_advance(cp, width);
    }
    return width;
}

static void appui_text(uint32_t *fb, int w, int h, int x, int y,
                       const char *s, uint32_t fg, int bg,
                       struct appui_rect clip) {
    int line_origin = x;
    while (s && *s) {
        uint32_t cp = appui_utf8_next(&s);
        if (cp == '\n') {
            y += KFONT_HEIGHT;
            x = line_origin;
            continue;
        }
        if (x >= clip.x + clip.w)
            return;
        x += appui_draw_codepoint_at(fb, w, h, x, y, cp, fg, bg, clip,
                                     line_origin);
    }
}

static void appui_button_ex(uint32_t *fb, int w, int h, struct appui_rect r,
                            const char *label, int variant, int state) {
    uint32_t bg = THEME_WIN_CONTROL;
    uint32_t edge = THEME_WIN_BORDER_INACT;
    uint32_t fg = THEME_TEXT;
    int disabled = (state & APPUI_STATE_DISABLED) != 0;
    int hovered = (state & APPUI_STATE_HOVERED) != 0 && !disabled;
    int pressed = (state & APPUI_STATE_PRESSED) != 0 && !disabled;

    if (variant == APPUI_BTN_PRIMARY) {
        bg = THEME_ACCENT_DIM;
        edge = THEME_ACCENT;
    } else if (variant == APPUI_BTN_DANGER) {
        bg = THEME_DANGER_DIM;
        edge = THEME_DANGER;
    } else if (variant == APPUI_BTN_GHOST) {
        bg = THEME_PANEL_BG;
        edge = THEME_PANEL_BG;
    }
    if (state & APPUI_STATE_SELECTED) {
        bg = pressed ? THEME_SELECTION_BG : THEME_SELECTION_SOFT;
        edge = THEME_ACCENT_DIM;
    } else if (hovered) {
        if (variant == APPUI_BTN_PRIMARY) {
            bg = THEME_ACCENT;
        } else if (variant == APPUI_BTN_DANGER) {
            bg = plt_blend(THEME_DANGER, THEME_DANGER_DIM, 120);
        } else {
            bg = THEME_WIN_HOVER;
            edge = variant == APPUI_BTN_GHOST ? THEME_WIN_HOVER :
                                                THEME_FIELD_BORDER;
        }
    }
    if (pressed && !(state & APPUI_STATE_SELECTED)) {
        if (variant == APPUI_BTN_PRIMARY)
            bg = THEME_ACCENT_SOFT;
        else if (variant == APPUI_BTN_DANGER)
            bg = THEME_DANGER_DIM;
        else
            bg = THEME_WIN_PRESSED;
    }
    if (hovered && !pressed &&
        (variant == APPUI_BTN_PRIMARY || variant == APPUI_BTN_DANGER))
        fg = THEME_TEXT_ON_LIGHT;
    if (disabled) {
        bg = THEME_PANEL_RAISED;
        edge = THEME_DIVIDER;
        fg = THEME_TEXT_FAINT;
    }

    appui_fill_round(fb, w, h, r, edge);
    appui_fill_round(fb, w, h,
                     (struct appui_rect){r.x + 1, r.y + 1, r.w - 2, r.h - 2},
                     bg);
    if (!(state & APPUI_STATE_DISABLED) && r.w > 8)
        appui_fill(fb, w, h, (struct appui_rect){r.x + 4, r.y + 2, r.w - 8, 1},
                   plt_blend(THEME_TEXT, bg, 28));

    int label_w = appui_text_width(label);
    int tx = r.x + appui_max(6, (r.w - label_w) / 2);
    int ty = r.y + (r.h - KFONT_HEIGHT) / 2 + PLT_FONT_Y_SHIFT +
             (pressed ? 1 : 0);
    appui_text(fb, w, h, tx, ty, label, fg, -1,
               (struct appui_rect){r.x + 4, r.y + 2, r.w - 8, r.h - 4});
}

static void appui_button(uint32_t *fb, int w, int h, struct appui_rect r,
                         const char *label, int active) {
    appui_button_ex(fb, w, h, r, label,
                    active ? APPUI_BTN_PRIMARY : APPUI_BTN_DEFAULT, 0);
}

static __attribute__((unused)) void appui_field_frame(
    uint32_t *fb, int w, int h, struct appui_rect r, int focused) {
    uint32_t edge = focused ? THEME_FOCUS : THEME_FIELD_BORDER;
    appui_fill_round(fb, w, h, r, edge);
    appui_fill_round(fb, w, h,
                     (struct appui_rect){r.x + 1, r.y + 1, r.w - 2, r.h - 2},
                     THEME_FIELD_BG);
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
