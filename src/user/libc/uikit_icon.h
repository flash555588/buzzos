#ifndef BUZZOS_UIKIT_ICON_H
#define BUZZOS_UIKIT_ICON_H

/* Monoline chrome icons for the BuzzOS user-space GUI.
 *
 * Modern desktops draw chrome glyphs as thin monoline strokes rather than
 * filled pictograms, which is most of why a shell reads as current: a close
 * button is two hairlines, not a bold X.  These icons are stored as
 * short op lists in a 0..100 box and stroked at draw time, so one definition
 * serves a 10px caption glyph and a 32px Start-menu tile without a second
 * asset and without the blur a scaled bitmap would show.
 *
 * Included by uikit.h; not intended to be included directly.
 */

#include <stdint.h>

enum {
    UI_ICO_LINE,      /* a,b -> c,d                                   */
    UI_ICO_RECT,      /* stroked box a,b .. c,d                       */
    UI_ICO_FILLRECT,  /* filled box a,b .. c,d                        */
    UI_ICO_CIRCLE,    /* stroked, centre a,b radius c                 */
    UI_ICO_DISC,      /* filled, centre a,b radius c                  */
    UI_ICO_ARC,       /* centre a,b radius c, d = start|end in 30 deg */
};

struct ui_icon_op {
    uint8_t op, a, b, c, d;
};

struct ui_icon {
    const struct ui_icon_op *ops;
    uint8_t count;
};

/* Taylor sine, good to ~1e-5 over [-pi,pi] -- enough for icon arcs and it
 * keeps the toolchain free of a libm dependency. */
static inline float ui_sin_rad(float a) {
    float a2 = a * a;
    return a * (1.0f - a2 / 6.0f *
                       (1.0f - a2 / 20.0f * (1.0f - a2 / 42.0f)));
}

static inline float ui_sin_deg(int deg) {
    float rad;
    deg %= 360;
    if (deg < 0) deg += 360;
    if (deg > 180) deg -= 360;
    rad = (float)deg * 3.14159265f / 180.0f;
    return ui_sin_rad(rad);
}

static inline float ui_cos_deg(int deg) { return ui_sin_deg(deg + 90); }

/* ------------------------------------------------------------------ */
/* Icon definitions                                                    */
/* ------------------------------------------------------------------ */

#define UI_OPS(name) static const struct ui_icon_op name[]

UI_OPS(ui_ops_close) = {
    {UI_ICO_LINE, 24, 24, 76, 76},
    {UI_ICO_LINE, 76, 24, 24, 76},
};
UI_OPS(ui_ops_minimize) = {
    {UI_ICO_LINE, 22, 50, 78, 50},
};
UI_OPS(ui_ops_maximize) = {
    {UI_ICO_RECT, 24, 24, 76, 76},
};
UI_OPS(ui_ops_restore) = {
    {UI_ICO_RECT, 18, 36, 64, 82},
    {UI_ICO_LINE, 32, 36, 32, 22},
    {UI_ICO_LINE, 32, 22, 82, 22},
    {UI_ICO_LINE, 82, 22, 82, 68},
    {UI_ICO_LINE, 82, 68, 64, 68},
};
/* Launcher: four squares with a seam. */
UI_OPS(ui_ops_start) = {
    {UI_ICO_FILLRECT, 17, 17, 46, 46},
    {UI_ICO_FILLRECT, 54, 17, 83, 46},
    {UI_ICO_FILLRECT, 17, 54, 46, 83},
    {UI_ICO_FILLRECT, 54, 54, 83, 83},
};
UI_OPS(ui_ops_search) = {
    {UI_ICO_CIRCLE, 44, 44, 24, 0},
    {UI_ICO_LINE, 62, 62, 82, 82},
};
UI_OPS(ui_ops_chevron_down) = {
    {UI_ICO_LINE, 26, 40, 50, 63},
    {UI_ICO_LINE, 50, 63, 74, 40},
};
UI_OPS(ui_ops_chevron_up) = {
    {UI_ICO_LINE, 26, 60, 50, 37},
    {UI_ICO_LINE, 50, 37, 74, 60},
};
UI_OPS(ui_ops_chevron_right) = {
    {UI_ICO_LINE, 40, 26, 63, 50},
    {UI_ICO_LINE, 63, 50, 40, 74},
};
UI_OPS(ui_ops_chevron_left) = {
    {UI_ICO_LINE, 60, 26, 37, 50},
    {UI_ICO_LINE, 37, 50, 60, 74},
};
/* Angles are screen-space: y grows downward, so 270 deg is straight up and
 * the power symbol's gap sits there. */
UI_OPS(ui_ops_power) = {
    {UI_ICO_ARC, 50, 56, 28, (10 << 4) | 8},  /* 300 deg round to 240 deg */
    {UI_ICO_LINE, 50, 16, 50, 48},
};
UI_OPS(ui_ops_settings) = {
    {UI_ICO_CIRCLE, 50, 50, 17, 0},
    {UI_ICO_LINE, 50, 12, 50, 26},
    {UI_ICO_LINE, 50, 74, 50, 88},
    {UI_ICO_LINE, 12, 50, 26, 50},
    {UI_ICO_LINE, 74, 50, 88, 50},
    {UI_ICO_LINE, 23, 23, 33, 33},
    {UI_ICO_LINE, 67, 67, 77, 77},
    {UI_ICO_LINE, 77, 23, 67, 33},
    {UI_ICO_LINE, 33, 67, 23, 77},
};
UI_OPS(ui_ops_folder) = {
    {UI_ICO_LINE, 14, 78, 14, 26},
    {UI_ICO_LINE, 14, 26, 40, 26},
    {UI_ICO_LINE, 40, 26, 48, 36},
    {UI_ICO_LINE, 48, 36, 86, 36},
    {UI_ICO_LINE, 86, 36, 86, 78},
    {UI_ICO_LINE, 86, 78, 14, 78},
};
UI_OPS(ui_ops_document) = {
    {UI_ICO_RECT, 26, 14, 74, 86},
    {UI_ICO_LINE, 36, 36, 64, 36},
    {UI_ICO_LINE, 36, 50, 64, 50},
    {UI_ICO_LINE, 36, 64, 55, 64},
};
UI_OPS(ui_ops_terminal) = {
    {UI_ICO_RECT, 12, 20, 88, 80},
    {UI_ICO_LINE, 26, 40, 40, 52},
    {UI_ICO_LINE, 40, 52, 26, 64},
    {UI_ICO_LINE, 48, 66, 70, 66},
};
UI_OPS(ui_ops_globe) = {
    {UI_ICO_CIRCLE, 50, 50, 36, 0},
    {UI_ICO_LINE, 14, 50, 86, 50},
    {UI_ICO_LINE, 50, 14, 50, 86},
    {UI_ICO_LINE, 24, 30, 76, 30},
    {UI_ICO_LINE, 24, 70, 76, 70},
};
UI_OPS(ui_ops_chart) = {
    {UI_ICO_FILLRECT, 18, 58, 31, 84},
    {UI_ICO_FILLRECT, 37, 38, 50, 84},
    {UI_ICO_FILLRECT, 56, 48, 69, 84},
    {UI_ICO_FILLRECT, 75, 22, 88, 84},
};
UI_OPS(ui_ops_music) = {
    {UI_ICO_DISC, 29, 74, 11, 0},
    {UI_ICO_DISC, 70, 66, 11, 0},
    {UI_ICO_LINE, 40, 74, 40, 26},
    {UI_ICO_LINE, 81, 66, 81, 16},
    {UI_ICO_LINE, 40, 26, 81, 16},
};
UI_OPS(ui_ops_image) = {
    {UI_ICO_RECT, 13, 24, 87, 76},
    {UI_ICO_DISC, 34, 40, 6, 0},
    {UI_ICO_LINE, 18, 70, 40, 48},
    {UI_ICO_LINE, 40, 48, 55, 61},
    {UI_ICO_LINE, 55, 61, 68, 46},
    {UI_ICO_LINE, 68, 46, 84, 66},
};
UI_OPS(ui_ops_calculator) = {
    {UI_ICO_RECT, 24, 12, 76, 88},
    {UI_ICO_FILLRECT, 32, 21, 68, 36},
    {UI_ICO_DISC, 36, 50, 4, 0},
    {UI_ICO_DISC, 50, 50, 4, 0},
    {UI_ICO_DISC, 64, 50, 4, 0},
    {UI_ICO_DISC, 36, 66, 4, 0},
    {UI_ICO_DISC, 50, 66, 4, 0},
    {UI_ICO_DISC, 64, 66, 4, 0},
};
UI_OPS(ui_ops_gamepad) = {
    {UI_ICO_DISC, 28, 56, 17, 0},
    {UI_ICO_DISC, 72, 56, 17, 0},
    {UI_ICO_FILLRECT, 28, 40, 72, 66},
    {UI_ICO_LINE, 22, 56, 34, 56},
    {UI_ICO_LINE, 28, 50, 28, 62},
    {UI_ICO_DISC, 72, 50, 4, 0},
    {UI_ICO_DISC, 78, 60, 4, 0},
};
UI_OPS(ui_ops_code) = {
    {UI_ICO_LINE, 34, 30, 16, 50},
    {UI_ICO_LINE, 16, 50, 34, 70},
    {UI_ICO_LINE, 66, 30, 84, 50},
    {UI_ICO_LINE, 84, 50, 66, 70},
    {UI_ICO_LINE, 58, 20, 42, 80},
};
UI_OPS(ui_ops_check) = {
    {UI_ICO_LINE, 22, 52, 41, 72},
    {UI_ICO_LINE, 41, 72, 80, 28},
};
UI_OPS(ui_ops_plus) = {
    {UI_ICO_LINE, 50, 22, 50, 78},
    {UI_ICO_LINE, 22, 50, 78, 50},
};
UI_OPS(ui_ops_minus) = {
    {UI_ICO_LINE, 22, 50, 78, 50},
};
UI_OPS(ui_ops_more) = {
    {UI_ICO_DISC, 22, 50, 6, 0},
    {UI_ICO_DISC, 50, 50, 6, 0},
    {UI_ICO_DISC, 78, 50, 6, 0},
};
UI_OPS(ui_ops_grid) = {
    {UI_ICO_FILLRECT, 16, 16, 34, 34},
    {UI_ICO_FILLRECT, 41, 16, 59, 34},
    {UI_ICO_FILLRECT, 66, 16, 84, 34},
    {UI_ICO_FILLRECT, 16, 41, 34, 59},
    {UI_ICO_FILLRECT, 41, 41, 59, 59},
    {UI_ICO_FILLRECT, 66, 41, 84, 59},
    {UI_ICO_FILLRECT, 16, 66, 34, 84},
    {UI_ICO_FILLRECT, 41, 66, 59, 84},
    {UI_ICO_FILLRECT, 66, 66, 84, 84},
};
UI_OPS(ui_ops_volume) = {
    {UI_ICO_LINE, 18, 40, 32, 40},
    {UI_ICO_LINE, 32, 40, 50, 22},
    {UI_ICO_LINE, 50, 22, 50, 78},
    {UI_ICO_LINE, 50, 78, 32, 60},
    {UI_ICO_LINE, 32, 60, 18, 60},
    {UI_ICO_LINE, 18, 40, 18, 60},
    {UI_ICO_ARC, 50, 50, 22, (10 << 4) | 2},
    {UI_ICO_ARC, 50, 50, 34, (10 << 4) | 2},
};
UI_OPS(ui_ops_keyboard) = {
    {UI_ICO_RECT, 10, 32, 90, 68},
    {UI_ICO_LINE, 22, 43, 30, 43},
    {UI_ICO_LINE, 38, 43, 46, 43},
    {UI_ICO_LINE, 54, 43, 62, 43},
    {UI_ICO_LINE, 70, 43, 78, 43},
    {UI_ICO_LINE, 32, 57, 68, 57},
};
UI_OPS(ui_ops_network) = {
    {UI_ICO_ARC, 50, 74, 40, (7 << 4) | 11},
    {UI_ICO_ARC, 50, 74, 27, (7 << 4) | 11},
    {UI_ICO_ARC, 50, 74, 14, (7 << 4) | 11},
    {UI_ICO_DISC, 50, 74, 4, 0},
};
UI_OPS(ui_ops_pin) = {
    {UI_ICO_LINE, 50, 56, 50, 86},
    {UI_ICO_LINE, 34, 18, 66, 18},
    {UI_ICO_LINE, 34, 18, 40, 56},
    {UI_ICO_LINE, 66, 18, 60, 56},
    {UI_ICO_LINE, 40, 56, 60, 56},
};

#undef UI_OPS

enum {
    UI_ICON_CLOSE, UI_ICON_MINIMIZE, UI_ICON_MAXIMIZE, UI_ICON_RESTORE,
    UI_ICON_START, UI_ICON_SEARCH, UI_ICON_CHEVRON_DOWN, UI_ICON_CHEVRON_UP,
    UI_ICON_CHEVRON_RIGHT, UI_ICON_CHEVRON_LEFT, UI_ICON_POWER,
    UI_ICON_SETTINGS, UI_ICON_FOLDER, UI_ICON_DOCUMENT, UI_ICON_TERMINAL,
    UI_ICON_GLOBE, UI_ICON_CHART, UI_ICON_MUSIC, UI_ICON_IMAGE,
    UI_ICON_CALCULATOR, UI_ICON_GAMEPAD, UI_ICON_CODE, UI_ICON_CHECK,
    UI_ICON_PLUS, UI_ICON_MINUS, UI_ICON_MORE, UI_ICON_GRID, UI_ICON_VOLUME,
    UI_ICON_KEYBOARD, UI_ICON_NETWORK, UI_ICON_PIN,
    UI_ICON_COUNT,
};

#define UI_ICON_ENTRY(sym) {sym, (uint8_t)(sizeof(sym) / sizeof(sym[0]))}

static const struct ui_icon ui_icons[UI_ICON_COUNT] = {
    UI_ICON_ENTRY(ui_ops_close),
    UI_ICON_ENTRY(ui_ops_minimize),
    UI_ICON_ENTRY(ui_ops_maximize),
    UI_ICON_ENTRY(ui_ops_restore),
    UI_ICON_ENTRY(ui_ops_start),
    UI_ICON_ENTRY(ui_ops_search),
    UI_ICON_ENTRY(ui_ops_chevron_down),
    UI_ICON_ENTRY(ui_ops_chevron_up),
    UI_ICON_ENTRY(ui_ops_chevron_right),
    UI_ICON_ENTRY(ui_ops_chevron_left),
    UI_ICON_ENTRY(ui_ops_power),
    UI_ICON_ENTRY(ui_ops_settings),
    UI_ICON_ENTRY(ui_ops_folder),
    UI_ICON_ENTRY(ui_ops_document),
    UI_ICON_ENTRY(ui_ops_terminal),
    UI_ICON_ENTRY(ui_ops_globe),
    UI_ICON_ENTRY(ui_ops_chart),
    UI_ICON_ENTRY(ui_ops_music),
    UI_ICON_ENTRY(ui_ops_image),
    UI_ICON_ENTRY(ui_ops_calculator),
    UI_ICON_ENTRY(ui_ops_gamepad),
    UI_ICON_ENTRY(ui_ops_code),
    UI_ICON_ENTRY(ui_ops_check),
    UI_ICON_ENTRY(ui_ops_plus),
    UI_ICON_ENTRY(ui_ops_minus),
    UI_ICON_ENTRY(ui_ops_more),
    UI_ICON_ENTRY(ui_ops_grid),
    UI_ICON_ENTRY(ui_ops_volume),
    UI_ICON_ENTRY(ui_ops_keyboard),
    UI_ICON_ENTRY(ui_ops_network),
    UI_ICON_ENTRY(ui_ops_pin),
};

#undef UI_ICON_ENTRY

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

/* Map a 0..100 icon coordinate into the target box. */
static inline int ui_icon_map(int v, int origin, int size) {
    return origin + (v * size + 50) / 100;
}

/* Stroke weight: one hairline for caption-sized glyphs,
 * growing to two only once the icon is large enough to carry it. */
static inline int ui_icon_thickness(int size) {
    int t = (size + 10) / 20;
    return t < 1 ? 1 : t;
}

static inline void ui_icon_arc(struct ui_surface *s, int cx, int cy, int rad,
                               int start_deg, int end_deg, int thick,
                               uint32_t color, int alpha) {
    int steps, px = 0, py = 0;
    if (end_deg <= start_deg)
        end_deg += 360;
    steps = (end_deg - start_deg) / 10;
    if (steps < 2)
        steps = 2;
    for (int i = 0; i <= steps; i++) {
        int deg = start_deg + (end_deg - start_deg) * i / steps;
        int x = cx + (int)(ui_cos_deg(deg) * (float)rad + 0.5f);
        int y = cy + (int)(ui_sin_deg(deg) * (float)rad + 0.5f);
        if (i > 0)
            ui_line(s, px, py, x, y, thick, color, alpha);
        px = x;
        py = y;
    }
}

/* Draw icon `id` inside a size x size box at (x,y). */
static inline void ui_icon(struct ui_surface *s, int id, int x, int y,
                           int size, uint32_t color, int alpha) {
    const struct ui_icon *icon;
    int thick;
    if (id < 0 || id >= UI_ICON_COUNT || size <= 0 || alpha <= 0)
        return;
    icon = &ui_icons[id];
    thick = ui_icon_thickness(size);
    for (int i = 0; i < icon->count; i++) {
        const struct ui_icon_op *o = &icon->ops[i];
        int ax = ui_icon_map(o->a, x, size);
        int ay = ui_icon_map(o->b, y, size);
        int bx = ui_icon_map(o->c, x, size);
        int by = ui_icon_map(o->d, y, size);
        int rad = (o->c * size + 50) / 100;
        switch (o->op) {
        case UI_ICO_LINE:
            ui_line(s, ax, ay, bx, by, thick, color, alpha);
            break;
        case UI_ICO_RECT:
            ui_line(s, ax, ay, bx, ay, thick, color, alpha);
            ui_line(s, bx, ay, bx, by, thick, color, alpha);
            ui_line(s, bx, by, ax, by, thick, color, alpha);
            ui_line(s, ax, by, ax, ay, thick, color, alpha);
            break;
        case UI_ICO_FILLRECT:
            ui_fill_round_a(s, ui_rect_make(ax, ay, bx - ax, by - ay),
                            ui_max(1, size / 20), color, alpha);
            break;
        case UI_ICO_CIRCLE:
            ui_ring(s, ax, ay, rad, thick, color, alpha);
            break;
        case UI_ICO_DISC:
            ui_circle(s, ax, ay, rad, color, alpha);
            break;
        case UI_ICO_ARC:
            ui_icon_arc(s, ax, ay, rad, (o->d >> 4) * 30, (o->d & 15) * 30,
                        thick, color, alpha);
            break;
        default:
            break;
        }
    }
}

/* Centre an icon of the given size inside r. */
static inline void ui_icon_in(struct ui_surface *s, int id, struct ui_rect r,
                              int size, uint32_t color, int alpha) {
    ui_icon(s, id, r.x + (r.w - size) / 2, r.y + (r.h - size) / 2, size, color,
            alpha);
}

#endif
