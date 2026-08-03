#ifndef BUZZOS_UIKIT_H
#define BUZZOS_UIKIT_H

/* Shape rendering kernel for the BuzzOS user-space GUI.
 *
 * Everything the theme needs that a rectangle blitter cannot express:
 * anti-aliased rounded rectangles at an arbitrary radius, gradients, soft
 * shadows and acrylic blur.  Surfaces are RGB32 0x00RRGGBB, matching the rest
 * of the GUI stack; alpha exists only as per-call coverage, never in a pixel.
 *
 * Anti-aliasing is by 4x4 supersampling inside the corner blocks only, so a
 * rounded rect costs one plain fill for its body plus 4*rad^2 coverage tests.
 * The alternative -- a signed distance field over the whole rect -- would pay
 * a square root per pixel to compute a value that is 0 or 255 everywhere
 * except a two-pixel band.
 *
 * All drawing is clipped to surface.clip, pushed and popped around nested
 * regions rather than threaded through every call.
 *
 * Text lives in uikit_text.h and icons in uikit_icon.h; including this file
 * pulls in both.
 */

#include <stddef.h>
#include <stdint.h>
#include "libc.h"
#include "palette.h"

struct ui_rect {
    int x;
    int y;
    int w;
    int h;
};

struct ui_surface {
    uint32_t *px;
    int w;
    int h;
    int stride;   /* pixels per row; differs from w on a padded scanout */
    int alpha;    /* straight-alpha ARGB target instead of opaque RGBX */
    struct ui_rect clip;
};

/* ------------------------------------------------------------------ */
/* Geometry                                                            */
/* ------------------------------------------------------------------ */

static inline struct ui_rect ui_rect_make(int x, int y, int w, int h) {
    struct ui_rect r;
    r.x = x; r.y = y; r.w = w; r.h = h;
    return r;
}

static inline int ui_min(int a, int b) { return a < b ? a : b; }
static inline int ui_max(int a, int b) { return a > b ? a : b; }

static inline int ui_clamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline struct ui_rect ui_rect_inset(struct ui_rect r, int d) {
    return ui_rect_make(r.x + d, r.y + d, r.w - 2 * d, r.h - 2 * d);
}

static inline struct ui_rect ui_rect_intersect(struct ui_rect a,
                                               struct ui_rect b) {
    int x0 = ui_max(a.x, b.x);
    int y0 = ui_max(a.y, b.y);
    int x1 = ui_min(a.x + a.w, b.x + b.w);
    int y1 = ui_min(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0)
        return ui_rect_make(x0, y0, 0, 0);
    return ui_rect_make(x0, y0, x1 - x0, y1 - y0);
}

static inline int ui_rect_contains(struct ui_rect r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static inline int ui_rect_empty(struct ui_rect r) {
    return r.w <= 0 || r.h <= 0;
}

static inline struct ui_surface ui_surface_stride(uint32_t *px, int w, int h,
                                                 int stride) {
    struct ui_surface s;
    s.px = px;
    s.w = w;
    s.h = h;
    s.stride = stride > 0 ? stride : w;
    s.alpha = 0;
    s.clip = ui_rect_make(0, 0, w, h);
    return s;
}

static inline struct ui_surface ui_surface_alpha_stride(uint32_t *px, int w,
                                                        int h, int stride) {
    struct ui_surface s = ui_surface_stride(px, w, h, stride);
    s.alpha = 1;
    return s;
}

static inline struct ui_surface ui_surface_make(uint32_t *px, int w, int h) {
    return ui_surface_stride(px, w, h, w);
}

/* Narrow the clip to r and return the previous value for ui_clip_pop. */
static inline struct ui_rect ui_clip_push(struct ui_surface *s,
                                          struct ui_rect r) {
    struct ui_rect saved = s->clip;
    s->clip = ui_rect_intersect(saved, r);
    return saved;
}

static inline void ui_clip_pop(struct ui_surface *s, struct ui_rect saved) {
    s->clip = saved;
}

/* Intersection of r with both the clip and the surface bounds. */
static inline struct ui_rect ui_clipped(const struct ui_surface *s,
                                        struct ui_rect r) {
    struct ui_rect bounds = ui_rect_make(0, 0, s->w, s->h);
    return ui_rect_intersect(ui_rect_intersect(r, s->clip), bounds);
}

/* ------------------------------------------------------------------ */
/* Color                                                               */
/* ------------------------------------------------------------------ */

/* Two channels at a time: R and B share one 32-bit lane without overlapping
 * because each 8-bit channel times a 0-255 factor stays inside 16 bits. */
static inline uint32_t ui_blend(uint32_t fg, uint32_t bg, uint32_t a) {
    uint32_t ia, rb, g;
    if (a >= 255u)
        return fg & 0x00FFFFFFu;
    if (a == 0u)
        return bg & 0x00FFFFFFu;
    ia = 255u - a;
    rb = (fg & 0x00FF00FFu) * a + (bg & 0x00FF00FFu) * ia + 0x00800080u;
    rb = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
    g = (fg & 0x0000FF00u) * a + (bg & 0x0000FF00u) * ia + 0x00008000u;
    g = ((g + ((g >> 8) & 0x0000FF00u)) >> 8) & 0x0000FF00u;
    return rb | g;
}

/* Straight-alpha source-over used by the transparent GPU shell overlay.
 * Opaque RGB surfaces retain the much cheaper two-lane blender above. */
static inline uint32_t ui_blend_argb(uint32_t fg, uint32_t bg, uint32_t a) {
    uint32_t da, ia, oa, r, g, b;
    if (a >= 255u)
        return 0xFF000000u | (fg & 0x00FFFFFFu);
    if (a == 0u)
        return bg;
    da = bg >> 24;
    ia = 255u - a;
    oa = a + (da * ia + 127u) / 255u;
    if (!oa)
        return 0;
    r = (((fg >> 16) & 255u) * a +
         ((bg >> 16) & 255u) * da * ia / 255u + oa / 2u) / oa;
    g = (((fg >> 8) & 255u) * a +
         ((bg >> 8) & 255u) * da * ia / 255u + oa / 2u) / oa;
    b = ((fg & 255u) * a + (bg & 255u) * da * ia / 255u + oa / 2u) / oa;
    return (oa << 24) | (r << 16) | (g << 8) | b;
}

static inline uint32_t ui_surface_blend(const struct ui_surface *s,
                                        uint32_t fg, uint32_t bg,
                                        uint32_t alpha) {
    return s->alpha ? ui_blend_argb(fg, bg, alpha)
                    : ui_blend(fg, bg, alpha);
}

static inline uint32_t ui_lerp(uint32_t a, uint32_t b, int t) {
    return ui_blend(b, a, (uint32_t)ui_clamp(t, 0, 255));
}

static inline void ui_pixel(struct ui_surface *s, int x, int y, uint32_t c) {
    if (ui_rect_contains(s->clip, x, y) && x >= 0 && y >= 0 &&
        x < s->w && y < s->h)
        s->px[y * s->stride + x] = (c & 0x00FFFFFFu) |
                                    (s->alpha ? 0xFF000000u : 0u);
}

static inline void ui_pixel_a(struct ui_surface *s, int x, int y, uint32_t c,
                              int a) {
    if (a <= 0)
        return;
    if (ui_rect_contains(s->clip, x, y) && x >= 0 && y >= 0 &&
        x < s->w && y < s->h) {
        uint32_t *p = &s->px[y * s->stride + x];
        *p = ui_surface_blend(s, c, *p, (uint32_t)ui_min(a, 255));
    }
}

static inline int ui_isqrt(int v) {
    int x, y;
    if (v <= 0)
        return 0;
    x = v;
    y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + v / x) / 2;
    }
    return x;
}

/* ------------------------------------------------------------------ */
/* Rectangles                                                          */
/* ------------------------------------------------------------------ */

static inline void ui_fill(struct ui_surface *s, struct ui_rect r,
                           uint32_t color) {
    struct ui_rect c = ui_clipped(s, r);
    if (ui_rect_empty(c))
        return;
    color = (color & 0x00FFFFFFu) | (s->alpha ? 0xFF000000u : 0u);
    for (int y = 0; y < c.h; y++) {
        uint32_t *row = s->px + (size_t)(c.y + y) * s->stride + c.x;
        for (int x = 0; x < c.w; x++)
            row[x] = color;
    }
}

static inline void ui_fill_a(struct ui_surface *s, struct ui_rect r,
                             uint32_t color, int alpha) {
    struct ui_rect c = ui_clipped(s, r);
    if (ui_rect_empty(c) || alpha <= 0)
        return;
    if (alpha >= 255) {
        ui_fill(s, r, color);
        return;
    }
    for (int y = 0; y < c.h; y++) {
        uint32_t *row = s->px + (size_t)(c.y + y) * s->stride + c.x;
        for (int x = 0; x < c.w; x++)
            row[x] = ui_surface_blend(s, color, row[x], (uint32_t)alpha);
    }
}

/* Vertical gradient, top to bottom. */
static inline void ui_gradient_v(struct ui_surface *s, struct ui_rect r,
                                 uint32_t top, uint32_t bottom) {
    struct ui_rect c = ui_clipped(s, r);
    if (ui_rect_empty(c) || r.h <= 0)
        return;
    for (int y = 0; y < c.h; y++) {
        int t = r.h > 1 ? (c.y + y - r.y) * 255 / (r.h - 1) : 0;
        uint32_t color = ui_lerp(top, bottom, t) |
                         (s->alpha ? 0xFF000000u : 0u);
        uint32_t *row = s->px + (size_t)(c.y + y) * s->stride + c.x;
        for (int x = 0; x < c.w; x++)
            row[x] = color;
    }
}

static inline void ui_gradient_h(struct ui_surface *s, struct ui_rect r,
                                 uint32_t left, uint32_t right) {
    struct ui_rect c = ui_clipped(s, r);
    if (ui_rect_empty(c) || r.w <= 0)
        return;
    for (int x = 0; x < c.w; x++) {
        int t = r.w > 1 ? (c.x + x - r.x) * 255 / (r.w - 1) : 0;
        uint32_t color = ui_lerp(left, right, t) |
                         (s->alpha ? 0xFF000000u : 0u);
        for (int y = 0; y < c.h; y++)
            s->px[(size_t)(c.y + y) * s->stride + c.x + x] = color;
    }
}

/* ------------------------------------------------------------------ */
/* Rounded rectangles                                                  */
/* ------------------------------------------------------------------ */

enum { UI_SS = 4 }; /* subsamples per axis */

/* Coverage 0..255 of the pixel at (px,py) against a circle of radius rad
 * centred on the continuous point (cx,cy).  Coordinates are scaled by 8 so a
 * subsample centre -- (i + 0.5) / 4 of a pixel -- lands on an integer. */
static inline int ui_arc_coverage(int px, int py, int cx, int cy, int rad) {
    int hits = 0;
    int r8 = rad * 8;
    int rr = r8 * r8;
    int cx8 = cx * 8;
    int cy8 = cy * 8;
    for (int j = 0; j < UI_SS; j++) {
        int dy = (py * 8 + j * 2 + 1) - cy8;
        int dy2 = dy * dy;
        if (dy2 > rr)
            continue;
        for (int i = 0; i < UI_SS; i++) {
            int dx = (px * 8 + i * 2 + 1) - cx8;
            if (dx * dx + dy2 <= rr)
                hits++;
        }
    }
    return hits * 255 / (UI_SS * UI_SS);
}

/* Ring coverage: inside the outer radius but outside the inner one. */
static inline int ui_ring_coverage(int px, int py, int cx, int cy, int outer,
                                   int inner) {
    int hits = 0;
    int ro = outer * 8, ri = inner * 8;
    int rro = ro * ro, rri = ri * ri;
    int cx8 = cx * 8, cy8 = cy * 8;
    for (int j = 0; j < UI_SS; j++) {
        int dy = (py * 8 + j * 2 + 1) - cy8;
        int dy2 = dy * dy;
        for (int i = 0; i < UI_SS; i++) {
            int dx = (px * 8 + i * 2 + 1) - cx8;
            int d2 = dx * dx + dy2;
            if (d2 <= rro && d2 > rri)
                hits++;
        }
    }
    return hits * 255 / (UI_SS * UI_SS);
}

/* Draw one rad x rad corner block. (cx,cy) is the arc centre; (bx,by) the
 * block's top-left pixel. */
static inline void ui_corner_block(struct ui_surface *s, int bx, int by,
                                   int cx, int cy, int rad, uint32_t color,
                                   int alpha) {
    for (int y = 0; y < rad; y++) {
        for (int x = 0; x < rad; x++) {
            int cov = ui_arc_coverage(bx + x, by + y, cx, cy, rad);
            if (cov > 0)
                ui_pixel_a(s, bx + x, by + y, color, cov * alpha / 255);
        }
    }
}

static inline int ui_radius_fit(struct ui_rect r, int rad) {
    int limit = ui_min(r.w, r.h) / 2;
    return ui_clamp(rad, 0, limit);
}

/* Coverage of a rounded rect at one pixel: 255 inside, 0 outside, partial in
 * the corner arcs.  Only the corner blocks pay for the supersampled test. */
static inline int ui_round_coverage(struct ui_rect r, int rad, int x, int y) {
    int lx, ly;
    if (!ui_rect_contains(r, x, y))
        return 0;
    rad = ui_radius_fit(r, rad);
    if (rad <= 0)
        return 255;
    lx = x - r.x;
    ly = y - r.y;
    if (lx < rad && ly < rad)
        return ui_arc_coverage(x, y, r.x + rad, r.y + rad, rad);
    if (lx >= r.w - rad && ly < rad)
        return ui_arc_coverage(x, y, r.x + r.w - rad, r.y + rad, rad);
    if (lx < rad && ly >= r.h - rad)
        return ui_arc_coverage(x, y, r.x + rad, r.y + r.h - rad, rad);
    if (lx >= r.w - rad && ly >= r.h - rad)
        return ui_arc_coverage(x, y, r.x + r.w - rad, r.y + r.h - rad, rad);
    return 255;
}

enum {
    UI_CORNER_TL = 1 << 0,
    UI_CORNER_TR = 1 << 1,
    UI_CORNER_BL = 1 << 2,
    UI_CORNER_BR = 1 << 3,
    UI_CORNER_TOP = UI_CORNER_TL | UI_CORNER_TR,
    UI_CORNER_BOTTOM = UI_CORNER_BL | UI_CORNER_BR,
    UI_CORNER_ALL = UI_CORNER_TOP | UI_CORNER_BOTTOM,
};

/* Rounded fill with per-corner control.
 *
 * Needed because Layered surfaces are frequently rounded on only one side: a
 * title bar rounds its top corners into the window frame, and a caption
 * button's hover fill has to round its outer corner to sit flush inside that
 * frame without overpainting it. */
static inline void ui_fill_round_mask(struct ui_surface *s, struct ui_rect r,
                                      int rad, int corners, uint32_t color,
                                      int alpha) {
    int square;
    if (ui_rect_empty(r) || alpha <= 0)
        return;
    rad = ui_radius_fit(r, rad);
    if (rad <= 0 || !(corners & UI_CORNER_ALL)) {
        ui_fill_a(s, r, color, alpha);
        return;
    }
    /* Middle band spans the full width; the end bands are inset by the
     * radius and the corner blocks fill what is left. */
    ui_fill_a(s, ui_rect_make(r.x, r.y + rad, r.w, r.h - 2 * rad), color,
              alpha);
    ui_fill_a(s, ui_rect_make(r.x + rad, r.y, r.w - 2 * rad, rad), color,
              alpha);
    ui_fill_a(s, ui_rect_make(r.x + rad, r.y + r.h - rad, r.w - 2 * rad, rad),
              color, alpha);

    square = rad;
    if (corners & UI_CORNER_TL)
        ui_corner_block(s, r.x, r.y, r.x + rad, r.y + rad, rad, color, alpha);
    else
        ui_fill_a(s, ui_rect_make(r.x, r.y, square, square), color, alpha);
    if (corners & UI_CORNER_TR)
        ui_corner_block(s, r.x + r.w - rad, r.y, r.x + r.w - rad, r.y + rad,
                        rad, color, alpha);
    else
        ui_fill_a(s, ui_rect_make(r.x + r.w - square, r.y, square, square),
                  color, alpha);
    if (corners & UI_CORNER_BL)
        ui_corner_block(s, r.x, r.y + r.h - rad, r.x + rad, r.y + r.h - rad,
                        rad, color, alpha);
    else
        ui_fill_a(s, ui_rect_make(r.x, r.y + r.h - square, square, square),
                  color, alpha);
    if (corners & UI_CORNER_BR)
        ui_corner_block(s, r.x + r.w - rad, r.y + r.h - rad, r.x + r.w - rad,
                        r.y + r.h - rad, rad, color, alpha);
    else
        ui_fill_a(s, ui_rect_make(r.x + r.w - square, r.y + r.h - square,
                                  square, square),
                  color, alpha);
}

static inline void ui_fill_round_a(struct ui_surface *s, struct ui_rect r,
                                   int rad, uint32_t color, int alpha) {
    ui_fill_round_mask(s, r, rad, UI_CORNER_ALL, color, alpha);
}

static inline void ui_fill_round(struct ui_surface *s, struct ui_rect r,
                                 int rad, uint32_t color) {
    ui_fill_round_a(s, r, rad, color, 255);
}

/* Rounded vertical gradient: the body is drawn as a gradient, the corners
 * take the gradient colour of their own row. */
static inline void ui_fill_round_grad(struct ui_surface *s, struct ui_rect r,
                                      int rad, uint32_t top, uint32_t bottom) {
    if (ui_rect_empty(r))
        return;
    rad = ui_radius_fit(r, rad);
    if (rad <= 0) {
        ui_gradient_v(s, r, top, bottom);
        return;
    }
    ui_gradient_v(s, ui_rect_make(r.x, r.y + rad, r.w, r.h - 2 * rad), top,
                  bottom);
    for (int y = 0; y < rad; y++) {
        int t_top = r.h > 1 ? y * 255 / (r.h - 1) : 0;
        int t_bot = r.h > 1 ? (r.h - rad + y) * 255 / (r.h - 1) : 255;
        ui_fill(s, ui_rect_make(r.x + rad, r.y + y, r.w - 2 * rad, 1),
                ui_lerp(top, bottom, t_top));
        ui_fill(s, ui_rect_make(r.x + rad, r.y + r.h - rad + y, r.w - 2 * rad,
                                1),
                ui_lerp(top, bottom, t_bot));
    }
    /* Gradients are vertical, so each corner row uses a single colour. */
    for (int y = 0; y < rad; y++) {
        uint32_t c_top = ui_lerp(top, bottom, r.h > 1 ? y * 255 / (r.h - 1) : 0);
        uint32_t c_bot = ui_lerp(top, bottom,
                                 r.h > 1 ? (r.h - rad + y) * 255 / (r.h - 1)
                                         : 255);
        for (int x = 0; x < rad; x++) {
            int cov;
            cov = ui_arc_coverage(r.x + x, r.y + y, r.x + rad, r.y + rad, rad);
            ui_pixel_a(s, r.x + x, r.y + y, c_top, cov);
            cov = ui_arc_coverage(r.x + r.w - rad + x, r.y + y,
                                  r.x + r.w - rad, r.y + rad, rad);
            ui_pixel_a(s, r.x + r.w - rad + x, r.y + y, c_top, cov);
            cov = ui_arc_coverage(r.x + x, r.y + r.h - rad + y, r.x + rad,
                                  r.y + r.h - rad, rad);
            ui_pixel_a(s, r.x + x, r.y + r.h - rad + y, c_bot, cov);
            cov = ui_arc_coverage(r.x + r.w - rad + x, r.y + r.h - rad + y,
                                  r.x + r.w - rad, r.y + r.h - rad, rad);
            ui_pixel_a(s, r.x + r.w - rad + x, r.y + r.h - rad + y, c_bot, cov);
        }
    }
}

/* Anti-aliased rounded outline of the given thickness, drawn inside r. */
static inline void ui_stroke_round(struct ui_surface *s, struct ui_rect r,
                                   int rad, int thick, uint32_t color,
                                   int alpha) {
    if (ui_rect_empty(r) || thick <= 0 || alpha <= 0)
        return;
    rad = ui_radius_fit(r, rad);
    if (thick > rad && rad > 0)
        thick = ui_min(thick, ui_min(r.w, r.h) / 2);
    if (rad <= 0) {
        ui_fill_a(s, ui_rect_make(r.x, r.y, r.w, thick), color, alpha);
        ui_fill_a(s, ui_rect_make(r.x, r.y + r.h - thick, r.w, thick), color,
                  alpha);
        ui_fill_a(s, ui_rect_make(r.x, r.y + thick, thick, r.h - 2 * thick),
                  color, alpha);
        ui_fill_a(s, ui_rect_make(r.x + r.w - thick, r.y + thick, thick,
                                  r.h - 2 * thick),
                  color, alpha);
        return;
    }
    /* Straight runs between the corner blocks. */
    ui_fill_a(s, ui_rect_make(r.x + rad, r.y, r.w - 2 * rad, thick), color,
              alpha);
    ui_fill_a(s, ui_rect_make(r.x + rad, r.y + r.h - thick, r.w - 2 * rad,
                              thick),
              color, alpha);
    ui_fill_a(s, ui_rect_make(r.x, r.y + rad, thick, r.h - 2 * rad), color,
              alpha);
    ui_fill_a(s, ui_rect_make(r.x + r.w - thick, r.y + rad, thick,
                              r.h - 2 * rad),
              color, alpha);

    int inner = rad - thick;
    if (inner < 0)
        inner = 0;
    for (int y = 0; y < rad; y++) {
        for (int x = 0; x < rad; x++) {
            int cov;
            cov = ui_ring_coverage(r.x + x, r.y + y, r.x + rad, r.y + rad, rad,
                                   inner);
            ui_pixel_a(s, r.x + x, r.y + y, color, cov * alpha / 255);
            cov = ui_ring_coverage(r.x + r.w - rad + x, r.y + y, r.x + r.w - rad,
                                   r.y + rad, rad, inner);
            ui_pixel_a(s, r.x + r.w - rad + x, r.y + y, color,
                       cov * alpha / 255);
            cov = ui_ring_coverage(r.x + x, r.y + r.h - rad + y, r.x + rad,
                                   r.y + r.h - rad, rad, inner);
            ui_pixel_a(s, r.x + x, r.y + r.h - rad + y, color,
                       cov * alpha / 255);
            cov = ui_ring_coverage(r.x + r.w - rad + x, r.y + r.h - rad + y,
                                   r.x + r.w - rad, r.y + r.h - rad, rad,
                                   inner);
            ui_pixel_a(s, r.x + r.w - rad + x, r.y + r.h - rad + y, color,
                       cov * alpha / 255);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Circles and lines                                                   */
/* ------------------------------------------------------------------ */

static inline void ui_circle(struct ui_surface *s, int cx, int cy, int rad,
                             uint32_t color, int alpha) {
    if (rad <= 0 || alpha <= 0)
        return;
    for (int y = cy - rad; y < cy + rad; y++) {
        for (int x = cx - rad; x < cx + rad; x++) {
            int cov = ui_arc_coverage(x, y, cx, cy, rad);
            if (cov > 0)
                ui_pixel_a(s, x, y, color, cov * alpha / 255);
        }
    }
}

static inline void ui_ring(struct ui_surface *s, int cx, int cy, int outer,
                           int thick, uint32_t color, int alpha) {
    int inner = outer - thick;
    if (outer <= 0 || alpha <= 0)
        return;
    if (inner < 0)
        inner = 0;
    for (int y = cy - outer; y < cy + outer; y++) {
        for (int x = cx - outer; x < cx + outer; x++) {
            int cov = ui_ring_coverage(x, y, cx, cy, outer, inner);
            if (cov > 0)
                ui_pixel_a(s, x, y, color, cov * alpha / 255);
        }
    }
}

/* Anti-aliased line of the given thickness, endpoints in whole pixels.
 * Distance is evaluated per pixel over the segment's bounding box, which is
 * cheap at the sizes icons use and avoids the special cases a Bresenham
 * variant needs for thickness and end caps. */
static inline void ui_line(struct ui_surface *s, int x0, int y0, int x1,
                           int y1, int thick, uint32_t color, int alpha) {
    if (alpha <= 0)
        return;
    if (thick < 1)
        thick = 1;
    int pad = thick + 2;
    int bx0 = ui_min(x0, x1) - pad, bx1 = ui_max(x0, x1) + pad;
    int by0 = ui_min(y0, y1) - pad, by1 = ui_max(y0, y1) + pad;
    /* Work in 1/16 pixel so the half-thickness of an odd stroke stays exact. */
    int dx = (x1 - x0) * 16, dy = (y1 - y0) * 16;
    int len2 = dx * dx + dy * dy;
    int half = thick * 8; /* thick/2 in 1/16 units */
    for (int y = by0; y <= by1; y++) {
        for (int x = bx0; x <= bx1; x++) {
            int px = (x - x0) * 16 + 8;
            int py = (y - y0) * 16 + 8;
            int t = 0, ox, oy, d2, d, cov;
            if (len2 > 0) {
                t = (px * dx + py * dy) / (len2 / 16 ? len2 / 16 : 1);
                t = ui_clamp(t, 0, 16);
            }
            ox = px - dx * t / 16;
            oy = py - dy * t / 16;
            d2 = ox * ox + oy * oy;
            d = ui_isqrt(d2);
            /* One pixel (16 units) of linear falloff at the stroke edge. */
            cov = (half + 8 - d) * 255 / 16;
            cov = ui_clamp(cov, 0, 255);
            if (cov > 0)
                ui_pixel_a(s, x, y, color, cov * alpha / 255);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Soft shadow                                                         */
/* ------------------------------------------------------------------ */

enum { UI_SHADOW_STEPS = 64 };

static inline const uint8_t *ui_shadow_profile(void) {
    static uint8_t table[UI_SHADOW_STEPS + 1];
    static int built;
    if (!built) {
        for (int i = 0; i <= UI_SHADOW_STEPS; i++) {
            float t = (float)i / (float)UI_SHADOW_STEPS;
            /* Smoothstep falloff: full opacity at the edge, easing to zero. */
            float f = 1.0f - t * t * (3.0f - 2.0f * t);
            table[i] = (uint8_t)(f * 255.0f + 0.5f);
        }
        built = 1;
    }
    return table;
}

/* Soft drop shadow for a rounded rect, offset down by dy.
 *
 * `caster` is the rect that will be painted on top; the shadow is attenuated
 * by that rect's coverage so the two meet exactly.  Getting this wrong is
 * visible: attenuating by the *offset* rect instead leaves the dy-pixel strip
 * directly under the window uncovered by either the shadow or the window, so
 * the wallpaper shows through as a bright seam and the shadow below it starts
 * at full strength with a hard edge. */
static inline void ui_shadow(struct ui_surface *s, struct ui_rect caster,
                             int rad, int blur, int alpha, int dy) {
    const uint8_t *profile = ui_shadow_profile();
    struct ui_rect box = ui_rect_make(caster.x, caster.y + dy, caster.w,
                                      caster.h);
    struct ui_rect band = ui_rect_make(box.x - blur,
                                       ui_min(box.y, caster.y) - blur,
                                       box.w + 2 * blur,
                                       box.h + 2 * blur + (dy > 0 ? dy : -dy));
    struct ui_rect c = ui_clipped(s, band);
    if (ui_rect_empty(c) || blur <= 0 || alpha <= 0)
        return;
    for (int y = c.y; y < c.y + c.h; y++) {
        for (int x = c.x; x < c.x + c.w; x++) {
            /* Overshoot beyond the shadow box on each axis, 0 when inside. */
            int ox = ui_max(ui_max(box.x - x, x - (box.x + box.w - 1)), 0);
            int oy = ui_max(ui_max(box.y - y, y - (box.y + box.h - 1)), 0);
            int d, a, hole;
            if (ox || oy) {
                d = (ox && oy) ? ui_isqrt(ox * ox + oy * oy) : ox + oy;
                if (d >= blur)
                    continue;
                a = profile[d * UI_SHADOW_STEPS / blur] * alpha / 255;
            } else {
                a = alpha; /* under the box: full strength before the hole */
            }
            /* Remove what the caster will cover, including the partial
             * coverage of its own antialiased corners. */
            hole = ui_round_coverage(caster, rad, x, y);
            if (hole >= 255)
                continue;
            a = a * (255 - hole) / 255;
            if (a > 0)
                ui_pixel_a(s, x, y, 0x000000u, a);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Acrylic                                                             */
/* ------------------------------------------------------------------ */

enum {
    UI_ACRYLIC_SHIFT = 2,               /* blur at 1/4 resolution */
    UI_ACRYLIC_SCALE = 1 << UI_ACRYLIC_SHIFT,
    UI_ACRYLIC_PASSES = 3,              /* box passes ~= Gaussian */
    UI_ACRYLIC_RADIUS = 3,
};

struct ui_blur_scratch {
    uint32_t *a;
    uint32_t *b;
    size_t capacity;
};

static inline struct ui_blur_scratch *ui_blur_pool(void) {
    static struct ui_blur_scratch pool;
    return &pool;
}

static inline int ui_blur_reserve(struct ui_blur_scratch *pool, size_t need) {
    if (pool->a && need <= pool->capacity)
        return 0;
    uint32_t *na = (uint32_t *)realloc(pool->a, need * sizeof(uint32_t));
    if (!na)
        return -1;
    pool->a = na;
    uint32_t *nb = (uint32_t *)realloc(pool->b, need * sizeof(uint32_t));
    if (!nb)
        return -1;
    pool->b = nb;
    pool->capacity = need;
    return 0;
}

/* One separable box pass over a small RGB32 buffer. */
static inline void ui_box_blur(uint32_t *src, uint32_t *dst, int w, int h,
                               int rad) {
    int span = 2 * rad + 1;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t r = 0, g = 0, b = 0;
            for (int i = -rad; i <= rad; i++) {
                uint32_t p = src[y * w + ui_clamp(x + i, 0, w - 1)];
                r += (p >> 16) & 0xFFu;
                g += (p >> 8) & 0xFFu;
                b += p & 0xFFu;
            }
            dst[y * w + x] = ((r / span) << 16) | ((g / span) << 8) | (b / span);
        }
    }
    for (int x = 0; x < w; x++) {
        for (int y = 0; y < h; y++) {
            uint32_t r = 0, g = 0, b = 0;
            for (int i = -rad; i <= rad; i++) {
                uint32_t p = dst[ui_clamp(y + i, 0, h - 1) * w + x];
                r += (p >> 16) & 0xFFu;
                g += (p >> 8) & 0xFFu;
                b += p & 0xFFu;
            }
            src[y * w + x] = ((r / span) << 16) | ((g / span) << 8) | (b / span);
        }
    }
}

/* Blur what is already on the surface under r, then tint it.
 *
 * The blur runs at 1/4 resolution: a box kernel that small is invisible once
 * upscaled, and it makes a full-width taskbar backdrop cost a few thousand
 * pixels instead of a hundred thousand.  Returns -1 and draws a flat tinted
 * fill if scratch memory is unavailable, so a low-memory system degrades to a
 * solid panel rather than failing to draw. */
static inline int ui_acrylic(struct ui_surface *s, struct ui_rect r, int rad,
                             uint32_t tint, int tint_alpha) {
    struct ui_rect c = ui_clipped(s, r);
    if (ui_rect_empty(c))
        return 0;

    int sw = (c.w + UI_ACRYLIC_SCALE - 1) >> UI_ACRYLIC_SHIFT;
    int sh = (c.h + UI_ACRYLIC_SCALE - 1) >> UI_ACRYLIC_SHIFT;
    struct ui_blur_scratch *pool = ui_blur_pool();
    if (sw < 1 || sh < 1 ||
        ui_blur_reserve(pool, (size_t)sw * (size_t)sh) < 0) {
        ui_fill_round_a(s, r, rad, tint, ui_max(tint_alpha, 235));
        return -1;
    }

    /* Downsample by box-averaging each UI_ACRYLIC_SCALE block. */
    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            uint32_t rr = 0, gg = 0, bb = 0, n = 0;
            for (int j = 0; j < UI_ACRYLIC_SCALE; j++) {
                int sy = ui_min((y << UI_ACRYLIC_SHIFT) + j, c.h - 1);
                for (int i = 0; i < UI_ACRYLIC_SCALE; i++) {
                    int sx = ui_min((x << UI_ACRYLIC_SHIFT) + i, c.w - 1);
                    uint32_t p = s->px[(size_t)(c.y + sy) * s->stride + c.x + sx];
                    rr += (p >> 16) & 0xFFu;
                    gg += (p >> 8) & 0xFFu;
                    bb += p & 0xFFu;
                    n++;
                }
            }
            pool->a[y * sw + x] = ((rr / n) << 16) | ((gg / n) << 8) | (bb / n);
        }
    }

    for (int pass = 0; pass < UI_ACRYLIC_PASSES; pass++)
        ui_box_blur(pool->a, pool->b, sw, sh, UI_ACRYLIC_RADIUS);

    /* Upsample, tint, and add the grain that keeps a large flat blur from
     * banding.  The pattern is a fixed function of position, so a static
     * panel does not shimmer between frames. */
    for (int y = 0; y < c.h; y++) {
        for (int x = 0; x < c.w; x++) {
            int sx = ui_min(x >> UI_ACRYLIC_SHIFT, sw - 1);
            int sy = ui_min(y >> UI_ACRYLIC_SHIFT, sh - 1);
            uint32_t blurred = pool->a[sy * sw + sx];
            uint32_t mixed = ui_blend(tint, blurred, (uint32_t)tint_alpha);
            int grain = (int)(((x * 7u + y * 13u) & 7u)) - 4;
            int rr = ui_clamp((int)((mixed >> 16) & 0xFFu) + grain, 0, 255);
            int gg = ui_clamp((int)((mixed >> 8) & 0xFFu) + grain, 0, 255);
            int bb = ui_clamp((int)(mixed & 0xFFu) + grain, 0, 255);
            uint32_t out = ((uint32_t)rr << 16) | ((uint32_t)gg << 8) |
                           (uint32_t)bb;

            int gx = c.x + x, gy = c.y + y;
            int cov = ui_round_coverage(r, rad, gx, gy);
            if (cov >= 255)
                s->px[(size_t)gy * s->stride + gx] = out;
            else if (cov > 0)
                ui_pixel_a(s, gx, gy, out, cov);
        }
    }
    return 0;
}

#include "uikit_text.h"
#include "uikit_icon.h"

#endif
