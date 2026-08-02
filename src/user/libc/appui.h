#ifndef BUZZOS_APPUI_H
#define BUZZOS_APPUI_H

/* Widget layer for BuzzOS GUI applications.
 *
 * Built on the uikit rendering kernel: appui owns widget behaviour and
 * layout, uikit owns pixels.  Applications keep passing a raw (pixels, w, h)
 * triple rather than a surface object, so the whole existing call surface is
 * unchanged.
 *
 * Two deliberate asymmetries in the theme migration:
 *
 *  - Shapes were upgraded in place.  appui_fill_round and appui_button_ex
 *    keep their signatures and geometry but now render anti-aliased corners,
 *    themed fills and hairline strokes, so every application picked up the
 *    new look without a source change.
 *
 *  - Text was not.  Applications lay out with KFONT_HEIGHT and KFONT_WIDTH --
 *    terminal, textedit, luaide and browser use them as a character grid --
 *    so silently rescaling appui_text would have shifted every caret, cursor
 *    and hit box in those apps.  appui_text therefore still draws at native
 *    size, and the scaled UI sizes are reached through appui_label,
 *    which applications adopt per-widget as they are migrated.
 */

#include <stddef.h>
#include <stdint.h>
#include "libc.h"
#include "palette.h"
#include "uikit.h"
#include "../../kernel/drv/font_builtin.h"

struct appui_rect {
    int x;
    int y;
    int w;
    int h;
};

/* appui_rect and ui_rect have the same shape; convert by value rather than
 * casting so the two stay independent types. */
static inline struct ui_rect appui_to_ui(struct appui_rect r) {
    return ui_rect_make(r.x, r.y, r.w, r.h);
}

static inline struct appui_rect appui_from_ui(struct ui_rect r) {
    struct appui_rect a;
    a.x = r.x; a.y = r.y; a.w = r.w; a.h = r.h;
    return a;
}

static inline struct appui_rect appui_rect_make(int x, int y, int w, int h) {
    struct appui_rect r;
    r.x = x; r.y = y; r.w = w; r.h = h;
    return r;
}

static inline struct ui_surface appui_surface(uint32_t *fb, int w, int h) {
    return ui_surface_make(fb, w, h);
}

/* Surface clipped to `clip`, for the calls that carry an explicit clip. */
static inline struct ui_surface appui_surface_clipped(uint32_t *fb, int w,
                                                      int h,
                                                      struct appui_rect clip) {
    struct ui_surface s = ui_surface_make(fb, w, h);
    s.clip = ui_rect_intersect(s.clip, appui_to_ui(clip));
    return s;
}

/* Growable RGB32 pixel buffer sized to the *current* window (modern-style),
 * not a static GUIAPP_MAX_W×MAX_H reservation.  Grows with ~25% slack so live
 * resize does not realloc every mouse sample; never exceeds max_w×max_h. */
static inline int appui_pixels_ensure(
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
static inline uint32_t appui_rgb6(int r, int g, int b) {
    return plt_cube(r, g, b);
}

static inline uint32_t appui_gray(int n) {
    return plt_gray(n);
}

static inline int appui_min(int a, int b) { return a < b ? a : b; }
static inline int appui_max(int a, int b) { return a > b ? a : b; }

static inline int appui_inside(int x, int y, struct appui_rect r) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static inline int appui_pointer_state(
    struct appui_rect r, int x, int y, int buttons) {
    if (!appui_inside(x, y, r))
        return 0;
    return APPUI_STATE_HOVERED |
           ((buttons & 1) ? APPUI_STATE_PRESSED : 0);
}

static inline void appui_pixel(uint32_t *fb, int w, int h, int x, int y,
                               uint32_t color) {
    if (x >= 0 && y >= 0 && x < w && y < h)
        fb[y * w + x] = color & 0x00FFFFFFu;
}

static inline void appui_fill(uint32_t *fb, int w, int h,
                              struct appui_rect r, uint32_t color) {
    struct ui_surface s = appui_surface(fb, w, h);
    ui_fill(&s, appui_to_ui(r), color);
}

static inline void appui_fill_blend(
    uint32_t *fb, int w, int h, struct appui_rect r, uint32_t color,
    int alpha) {
    struct ui_surface s = appui_surface(fb, w, h);
    ui_fill_a(&s, appui_to_ui(r), color, alpha);
}

/* Rounded fill at an explicit radius. */
static inline void appui_fill_round_r(uint32_t *fb, int w, int h,
                                      struct appui_rect r, int radius,
                                      uint32_t color) {
    struct ui_surface s = appui_surface(fb, w, h);
    ui_fill_round(&s, appui_to_ui(r), radius, color);
}

/* Control-radius rounded fill.  Same geometry as the original 4px corner
 * table it replaces, but anti-aliased. */
static inline void appui_fill_round(uint32_t *fb, int w, int h,
                                    struct appui_rect r, uint32_t color) {
    appui_fill_round_r(fb, w, h, r, UI_RADIUS_CONTROL, color);
}

static inline void appui_stroke_round(uint32_t *fb, int w, int h,
                                      struct appui_rect r, int radius,
                                      uint32_t color) {
    struct ui_surface s = appui_surface(fb, w, h);
    ui_stroke_round(&s, appui_to_ui(r), radius, 1, color, 255);
}

/* Two-tone bevel: highlight on the top and left, shadow on the bottom and
 * right.  Retained for the apps that draw inset frames with it. */
static inline void appui_border(uint32_t *fb, int w, int h,
                                struct appui_rect r, uint32_t hi,
                                uint32_t lo) {
    appui_fill(fb, w, h, appui_rect_make(r.x, r.y, r.w, 1), hi);
    appui_fill(fb, w, h, appui_rect_make(r.x, r.y, 1, r.h), hi);
    appui_fill(fb, w, h, appui_rect_make(r.x, r.y + r.h - 1, r.w, 1), lo);
    appui_fill(fb, w, h, appui_rect_make(r.x + r.w - 1, r.y, 1, r.h), lo);
}

static inline void appui_icon(uint32_t *fb, int w, int h, int id,
                              struct appui_rect r, int size, uint32_t color) {
    struct ui_surface s = appui_surface(fb, w, h);
    ui_icon_in(&s, id, appui_to_ui(r), size, color, 255);
}

/* ------------------------------------------------------------------ */
/* Text: native-size grid                                              */
/* ------------------------------------------------------------------ */

/* Decode one UTF-8 scalar and always make progress. Invalid input is rendered
 * as U+FFFD, so arbitrary file/network bytes cannot wedge a GUI application. */
static inline uint32_t appui_utf8_next(const char **text) {
    return ui_utf8_next(text);
}

static inline int appui_utf8_prev(const char *text, int pos) {
    if (!text || pos <= 0) return 0;
    pos--;
    while (pos > 0 && ((uint8_t)text[pos] & 0xC0u) == 0x80u)
        pos--;
    return pos;
}

/* Horizontal tab is a layout control, not a glyph. */
enum { APPUI_TAB_COLUMNS = 4 };

static inline int appui_tab_width(void) {
    return KFONT_WIDTH * APPUI_TAB_COLUMNS;
}

static inline int appui_tab_advance(int x_from_line_start) {
    int tab = appui_tab_width();
    if (tab <= 0)
        return KFONT_WIDTH;
    if (x_from_line_start < 0)
        x_from_line_start = 0;
    int advance = tab - (x_from_line_start % tab);
    return advance <= 0 ? tab : advance;
}

static inline int appui_codepoint_width(uint32_t cp) {
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

static inline int appui_codepoint_advance(uint32_t cp,
                                          int x_from_line_start) {
    if (cp == '\t')
        return appui_tab_advance(x_from_line_start);
    return appui_codepoint_width(cp);
}

static inline int appui_draw_codepoint(
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
                                ui_blend(fg, under, (uint32_t)coverage));
                }
            }
        }
    }
    return glyph_w;
}

static inline int appui_draw_codepoint_at(
    uint32_t *fb, int w, int h, int x, int y, uint32_t cp,
    uint32_t fg, int bg, struct appui_rect clip, int line_origin_x) {
    if (cp == '\t')
        return appui_tab_advance(x - line_origin_x);
    return appui_draw_codepoint(fb, w, h, x, y, cp, fg, bg, clip);
}

static inline int appui_text_width(const char *s) {
    int width = 0;
    while (s && *s) {
        uint32_t cp = appui_utf8_next(&s);
        if (cp == '\n')
            break;
        width += appui_codepoint_advance(cp, width);
    }
    return width;
}

static inline void appui_text(uint32_t *fb, int w, int h, int x, int y,
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

/* ------------------------------------------------------------------ */
/* Text: scaled UI sizes                                           */
/* ------------------------------------------------------------------ */

/* Chrome label at a UI size, aligned and vertically centred inside r.
 * Unlike appui_text this does not sit on the KFONT grid, so it is only safe
 * where the caller is not doing character-cell layout. */
static inline void appui_label(uint32_t *fb, int w, int h,
                               struct appui_rect r, const char *text,
                               int size, uint32_t color, int align) {
    struct ui_surface s = appui_surface(fb, w, h);
    struct ui_text_style st = ui_style(size, color);
    st.align = align;
    ui_text_in(&s, appui_to_ui(r), text, st);
}

static inline int appui_label_width(const char *text, int size) {
    return ui_text_width(text, size);
}

static inline int appui_label_height(int size) {
    return ui_font_height(size);
}

/* ------------------------------------------------------------------ */
/* Controls                                                            */
/* ------------------------------------------------------------------ */

static inline void appui_button_ex(uint32_t *fb, int w, int h,
                                   struct appui_rect r, const char *label,
                                   int variant, int state) {
    struct ui_surface s = appui_surface(fb, w, h);
    struct ui_rect box = appui_to_ui(r);
    int disabled = (state & APPUI_STATE_DISABLED) != 0;
    int hovered = (state & APPUI_STATE_HOVERED) != 0 && !disabled;
    int pressed = (state & APPUI_STATE_PRESSED) != 0 && !disabled;
    int selected = (state & APPUI_STATE_SELECTED) != 0;
    uint32_t bg, edge, fg;
    struct ui_text_style ts;

    if (disabled) {
        bg = UI_CTRL_DISABLED;
        edge = UI_STROKE_CONTROL;
        fg = UI_TEXT_DISABLED;
    } else if (variant == APPUI_BTN_PRIMARY || selected) {
        /* Accent fills take the light end of the ramp with black text; that
         * inversion is what keeps an accent button reading as bright rather
         * than as a dark navy slab. */
        bg = pressed ? UI_ACCENT_FILL_PRESS
                     : (hovered ? UI_ACCENT_FILL_HOVER : UI_ACCENT_FILL);
        edge = UI_ACCENT_DARK1;
        fg = UI_TEXT_ON_ACCENT;
    } else if (variant == APPUI_BTN_DANGER) {
        bg = pressed ? plt_shade(UI_SYS_CRITICAL, 70)
                     : (hovered ? UI_SYS_CRITICAL
                                : plt_shade(UI_SYS_CRITICAL, 85));
        edge = UI_SYS_CRITICAL;
        fg = UI_TEXT_ON_ACCENT;
    } else if (variant == APPUI_BTN_GHOST) {
        bg = pressed ? UI_SUBTLE_PRESSED
                     : (hovered ? UI_SUBTLE_HOVER : UI_BG_SOLID);
        edge = hovered ? UI_STROKE_CONTROL : UI_BG_SOLID;
        fg = UI_TEXT_PRIMARY;
    } else {
        bg = pressed ? UI_CTRL_PRESSED
                     : (hovered ? UI_CTRL_HOVER : UI_CTRL_REST);
        edge = UI_STROKE_CONTROL;
        fg = pressed ? UI_TEXT_SECONDARY : UI_TEXT_PRIMARY;
    }

    ui_fill_round(&s, box, UI_RADIUS_CONTROL, bg);
    ui_stroke_round(&s, box, UI_RADIUS_CONTROL, 1, edge, 255);
    ts = ui_style(UI_FONT_BODY, fg);
    ts.align = UI_ALIGN_CENTER;
    ui_text_in(&s, ui_rect_inset(box, 6), label, ts);
}

static inline void appui_button(uint32_t *fb, int w, int h,
                                struct appui_rect r, const char *label,
                                int active) {
    appui_button_ex(fb, w, h, r, label,
                    active ? APPUI_BTN_PRIMARY : APPUI_BTN_DEFAULT, 0);
}

/* Square icon-only button, for toolbars. */
static inline void appui_icon_button(uint32_t *fb, int w, int h,
                                     struct appui_rect r, int icon,
                                     int state) {
    struct ui_surface s = appui_surface(fb, w, h);
    struct ui_rect box = appui_to_ui(r);
    int disabled = (state & APPUI_STATE_DISABLED) != 0;
    int hovered = (state & APPUI_STATE_HOVERED) != 0 && !disabled;
    int pressed = (state & APPUI_STATE_PRESSED) != 0 && !disabled;
    int selected = (state & APPUI_STATE_SELECTED) != 0;
    uint32_t fg = disabled ? UI_TEXT_DISABLED : UI_TEXT_PRIMARY;

    if (selected)
        ui_fill_round(&s, box, UI_RADIUS_CONTROL, UI_ACCENT_FILL);
    else if (pressed)
        ui_fill_round(&s, box, UI_RADIUS_CONTROL, UI_SUBTLE_PRESSED);
    else if (hovered)
        ui_fill_round(&s, box, UI_RADIUS_CONTROL, UI_SUBTLE_HOVER);
    if (selected)
        fg = UI_TEXT_ON_ACCENT;
    ui_icon_in(&s, icon, box, appui_max(12, appui_min(r.w, r.h) - 12), fg,
               255);
}

static inline void appui_field_frame(
    uint32_t *fb, int w, int h, struct appui_rect r, int focused) {
    struct ui_surface s = appui_surface(fb, w, h);
    struct ui_rect box = appui_to_ui(r);
    ui_fill_round(&s, box, UI_RADIUS_CONTROL, UI_BG_MICA_ALT);
    ui_stroke_round(&s, box, UI_RADIUS_CONTROL, 1,
                    focused ? UI_ACCENT_FILL : UI_STROKE_CONTROL, 255);
    /* The theme marks the focused field with a thicker accent underline. */
    if (focused)
        ui_fill_round(&s,
                      ui_rect_make(box.x + UI_RADIUS_CONTROL,
                                   box.y + box.h - 2,
                                   box.w - 2 * UI_RADIUS_CONTROL, 2),
                      1, UI_ACCENT_FILL);
}

static inline void appui_checkbox(uint32_t *fb, int w, int h,
                                  struct appui_rect r, int checked,
                                  int state) {
    struct ui_surface s = appui_surface(fb, w, h);
    int side = appui_min(20, appui_min(r.w, r.h));
    struct ui_rect box = ui_rect_make(r.x, r.y + (r.h - side) / 2, side, side);
    int disabled = (state & APPUI_STATE_DISABLED) != 0;
    int hovered = (state & APPUI_STATE_HOVERED) != 0 && !disabled;

    if (checked) {
        ui_fill_round(&s, box, UI_RADIUS_CONTROL,
                      disabled ? UI_CTRL_DISABLED
                               : (hovered ? UI_ACCENT_FILL_HOVER
                                          : UI_ACCENT_FILL));
        ui_icon_in(&s, UI_ICON_CHECK, box, side - 6,
                   disabled ? UI_TEXT_DISABLED : UI_TEXT_ON_ACCENT, 255);
    } else {
        ui_fill_round(&s, box, UI_RADIUS_CONTROL,
                      hovered ? UI_CTRL_HOVER : UI_CTRL_REST);
        ui_stroke_round(&s, box, UI_RADIUS_CONTROL, 1,
                        disabled ? UI_STROKE_SECONDARY : UI_STROKE_CONTROL,
                        255);
    }
}

/* Determinate progress bar; `value` and `total` may be any positive scale. */
static inline void appui_progress(uint32_t *fb, int w, int h,
                                  struct appui_rect r, int value, int total) {
    struct ui_surface s = appui_surface(fb, w, h);
    struct ui_rect track = appui_to_ui(r);
    int filled;
    if (total <= 0)
        total = 1;
    if (value < 0) value = 0;
    if (value > total) value = total;
    filled = (int)((long)track.w * value / total);
    ui_fill_round(&s, track, track.h / 2, UI_CTRL_REST);
    if (filled > 0)
        ui_fill_round(&s, ui_rect_make(track.x, track.y, filled, track.h),
                      track.h / 2, UI_ACCENT_FILL);
}

/* ------------------------------------------------------------------ */
/* Containers                                                          */
/* ------------------------------------------------------------------ */

/* Elevated content card: a layer fill, hairline stroke and soft shadow. */
static inline void appui_card(uint32_t *fb, int w, int h,
                              struct appui_rect r) {
    struct ui_surface s = appui_surface(fb, w, h);
    struct ui_rect box = appui_to_ui(r);
    ui_shadow(&s, box, UI_RADIUS_OVERLAY, UI_ELEV_CARD_R, UI_ELEV_CARD_A,
              2);
    ui_fill_round(&s, box, UI_RADIUS_OVERLAY, UI_BG_LAYER);
    ui_stroke_round(&s, box, UI_RADIUS_OVERLAY, 1, UI_STROKE_CONTROL, 255);
}

static inline void appui_toolbar(uint32_t *fb, int w, int h,
                                 struct appui_rect r) {
    struct ui_surface s = appui_surface(fb, w, h);
    ui_fill(&s, appui_to_ui(r), UI_BG_LAYER);
    ui_fill_a(&s, ui_rect_make(r.x, r.y + r.h - 1, r.w, 1),
              UI_STROKE_DIVIDER, 255);
}

static inline void appui_separator(uint32_t *fb, int w, int h, int x, int y,
                                   int extent, int vertical) {
    struct ui_surface s = appui_surface(fb, w, h);
    ui_fill_a(&s,
              vertical ? ui_rect_make(x, y, 1, extent)
                       : ui_rect_make(x, y, extent, 1),
              UI_STROKE_DIVIDER, 255);
}

/* Dim everything behind a modal.  Draw before the dialog itself. */
static inline void appui_scrim(uint32_t *fb, int w, int h) {
    struct ui_surface s = appui_surface(fb, w, h);
    ui_fill_a(&s, ui_rect_make(0, 0, w, h), UI_BG_SMOKE, 120);
}

/* One row of a list.  The theme marks the selected row with a leading accent
 * bar rather than by filling the whole row, which keeps long lists readable. */
static inline void appui_list_row(uint32_t *fb, int w, int h,
                                  struct appui_rect r, const char *label,
                                  int icon, int state) {
    struct ui_surface s = appui_surface(fb, w, h);
    struct ui_rect box = appui_to_ui(r);
    int selected = (state & APPUI_STATE_SELECTED) != 0;
    int disabled = (state & APPUI_STATE_DISABLED) != 0;
    int hovered = (state & APPUI_STATE_HOVERED) != 0 && !disabled;
    int pressed = (state & APPUI_STATE_PRESSED) != 0 && !disabled;
    int text_x = r.x + 12;
    uint32_t fg = disabled ? UI_TEXT_DISABLED
                           : (selected || hovered ? UI_TEXT_PRIMARY
                                                  : UI_TEXT_SECONDARY);

    if (selected)
        ui_fill_round(&s, box, UI_RADIUS_CONTROL, UI_SUBTLE_HOVER);
    else if (pressed)
        ui_fill_round(&s, box, UI_RADIUS_CONTROL, UI_SUBTLE_PRESSED);
    else if (hovered)
        ui_fill_round(&s, box, UI_RADIUS_CONTROL, UI_SUBTLE_PRESSED);
    if (selected)
        ui_fill_round(&s,
                      ui_rect_make(box.x + 1, box.y + box.h / 4, 3,
                                   box.h / 2),
                      1, UI_ACCENT_FILL);
    if (icon >= 0) {
        ui_icon(&s, icon, r.x + 14, r.y + (r.h - 18) / 2, 18,
                selected ? UI_ACCENT_FILL : fg, 255);
        text_x = r.x + 42;
    }
    appui_label(fb, w, h, appui_rect_make(text_x, r.y, r.x + r.w - text_x - 8,
                                          r.h),
                label, UI_FONT_BODY, fg, UI_ALIGN_LEFT);
}

/* Horizontal tab strip item; the active tab carries the accent underline. */
static inline void appui_tab(uint32_t *fb, int w, int h,
                             struct appui_rect r, const char *label,
                             int active, int state) {
    struct ui_surface s = appui_surface(fb, w, h);
    struct ui_rect box = appui_to_ui(r);
    int hovered = (state & APPUI_STATE_HOVERED) != 0;
    uint32_t fg = active ? UI_TEXT_PRIMARY : UI_TEXT_SECONDARY;

    if (hovered && !active)
        ui_fill_round_mask(&s, box, UI_RADIUS_CONTROL, UI_CORNER_TOP,
                           UI_SUBTLE_PRESSED, 255);
    else if (active)
        ui_fill_round_mask(&s, box, UI_RADIUS_CONTROL, UI_CORNER_TOP,
                           UI_BG_LAYER, 255);
    appui_label(fb, w, h, r, label, UI_FONT_BODY, fg, UI_ALIGN_CENTER);
    if (active)
        ui_fill_round(&s,
                      ui_rect_make(box.x + box.w / 4, box.y + box.h - 3,
                                   box.w / 2, 3),
                      1, UI_ACCENT_FILL);
}

/* ------------------------------------------------------------------ */
/* Scrollbar                                                           */
/* ------------------------------------------------------------------ */

enum { APPUI_SCROLL_W = 12, APPUI_SCROLL_MIN_THUMB = 24 };

/* Thumb rect for a track, given the content and viewport extents.  Shared by
 * the painter and by drag hit-testing so the two cannot disagree -- six
 * applications previously each derived this twice. */
static inline struct appui_rect appui_scroll_thumb(struct appui_rect track,
                                                   int vertical, int content,
                                                   int viewport, int offset) {
    int extent = vertical ? track.h : track.w;
    int max_scroll = content - viewport;
    int size, pos;
    if (max_scroll <= 0 || content <= 0)
        return track;
    size = viewport * extent / content;
    if (size < APPUI_SCROLL_MIN_THUMB)
        size = APPUI_SCROLL_MIN_THUMB;
    if (size > extent)
        size = extent;
    if (offset < 0) offset = 0;
    if (offset > max_scroll) offset = max_scroll;
    pos = (extent - size) * offset / max_scroll;
    return vertical ? appui_rect_make(track.x, track.y + pos, track.w, size)
                    : appui_rect_make(track.x + pos, track.y, size, track.h);
}

/* Scroll offset for a pointer position on the track, for click-to-page and
 * thumb dragging. */
static inline int appui_scroll_offset_at(struct appui_rect track,
                                         int vertical, int content,
                                         int viewport, int pointer) {
    int extent = vertical ? track.h : track.w;
    int origin = vertical ? track.y : track.x;
    int max_scroll = content - viewport;
    struct appui_rect thumb;
    int size, span, rel;
    if (max_scroll <= 0)
        return 0;
    thumb = appui_scroll_thumb(track, vertical, content, viewport, 0);
    size = vertical ? thumb.h : thumb.w;
    span = extent - size;
    if (span <= 0)
        return 0;
    rel = pointer - origin - size / 2;
    if (rel < 0) rel = 0;
    if (rel > span) rel = span;
    return rel * max_scroll / span;
}

/* Scrollbar: a thin rounded thumb over a track that only fills in when
 * hot, so the bar reads as part of the content rather than as chrome. */
static inline void appui_scrollbar(uint32_t *fb, int w, int h,
                                   struct appui_rect track, int vertical,
                                   int content, int viewport, int offset,
                                   int hot) {
    struct ui_surface s = appui_surface(fb, w, h);
    struct appui_rect thumb;
    struct ui_rect t;
    int inset;

    if (content <= viewport || content <= 0)
        return;
    thumb = appui_scroll_thumb(track, vertical, content, viewport, offset);
    if (hot)
        ui_fill_round(&s, appui_to_ui(track),
                      (vertical ? track.w : track.h) / 2, UI_BG_MICA_ALT);
    inset = hot ? 2 : 3;
    t = vertical
        ? ui_rect_make(thumb.x + inset, thumb.y + 2, thumb.w - 2 * inset,
                       thumb.h - 4)
        : ui_rect_make(thumb.x + 2, thumb.y + inset, thumb.w - 4,
                       thumb.h - 2 * inset);
    ui_fill_round(&s, t, 3, hot ? UI_TEXT_SECONDARY : UI_TEXT_TERTIARY);
}

/* ------------------------------------------------------------------ */
/* String helpers                                                      */
/* ------------------------------------------------------------------ */

static inline void appui_copy_text(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0)
        return;
    while (i + 1 < cap && src && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static inline void appui_append_text(char *dst, const char *src, int cap) {
    int n = 0;
    while (n < cap && dst[n])
        n++;
    int i = 0;
    while (n + 1 < cap && src && src[i])
        dst[n++] = src[i++];
    if (cap > 0)
        dst[n] = 0;
}

static inline void appui_append_int(char *dst, int value, int cap) {
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
