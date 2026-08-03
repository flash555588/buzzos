#ifndef BUZZOS_UIKIT_TEXT_H
#define BUZZOS_UIKIT_TEXT_H

/* Scaled text rendering for the BuzzOS user-space GUI.
 *
 * The built-in font is a single 15x28 anti-aliased face.  At native size it
 * reads as roughly 20pt, which is why the pre-theme UI felt oversized: every
 * label, menu row and button was sized around a display-weight glyph.  This
 * layer resamples that one face into a small set of UI sizes, cached per size,
 * so the shell can use 10px captions and 12px body text at modern UI density.
 *
 * The native cell carries blank rows above the cap height -- the reason older
 * code offsets every draw by PLT_FONT_Y_SHIFT.  Here the cell is cropped to
 * the font's true ink bounds once at cache-build time, so a glyph's y is the
 * top of the glyph and no caller-side fudge is needed.
 *
 * Included by uikit.h; not intended to be included directly.
 */

#include <stddef.h>
#include <stdint.h>
#include "libc.h"
#include "palette.h"
#include "../../kernel/drv/font_builtin.h"

enum {
    UI_FONT_CAPTION,   /* 58% -- tray clock, tooltips, secondary labels */
    UI_FONT_BODY,      /* 68% -- the default UI size                    */
    UI_FONT_BODY_LG,   /* 79% -- list rows, buttons wanting emphasis    */
    UI_FONT_SUBTITLE,  /* 100% native                                   */
    UI_FONT_TITLE,     /* 125% -- headings                              */
    UI_FONT_COUNT,
};

enum {
    UI_ALIGN_LEFT = 0,
    UI_ALIGN_CENTER = 1,
    UI_ALIGN_RIGHT = 2,
};

static const uint8_t ui_font_scale_pct[UI_FONT_COUNT] = {58, 68, 79, 100, 125};

struct ui_font {
    int built;
    int gw;            /* scaled cell width (the face is monospace) */
    int gh;            /* scaled cell height, ink-cropped           */
    uint8_t *cov;      /* KFONT_COUNT * gw * gh coverage bytes      */
};

/* Ink bounds of the whole face, so every size crops identically. */
struct ui_font_ink {
    int built;
    int top;
    int bottom;        /* inclusive */
};

static inline struct ui_font_ink *ui_font_ink(void) {
    static struct ui_font_ink ink;
    if (!ink.built) {
        int top = KFONT_HEIGHT, bottom = -1;
        for (int g = 0; g < KFONT_COUNT; g++) {
            for (int y = 0; y < KFONT_HEIGHT; y++) {
                for (int x = 0; x < KFONT_WIDTH; x++) {
                    if (!kfont_alpha[g][y][x])
                        continue;
                    if (y < top) top = y;
                    if (y > bottom) bottom = y;
                    break;
                }
            }
        }
        if (bottom < top) { top = 0; bottom = KFONT_HEIGHT - 1; }
        ink.top = top;
        ink.bottom = bottom;
        ink.built = 1;
    }
    return &ink;
}

/* Box-average the source footprint of one destination pixel.  Correct for
 * minification, which is the case every UI size below native hits. */
static inline int ui_sample_box(const uint8_t *src, int sw, int sh,
                                int x0, int x1, int y0, int y1) {
    int sum = 0, n = 0;
    if (x1 <= x0) x1 = x0 + 1;
    if (y1 <= y0) y1 = y0 + 1;
    for (int y = y0; y < y1 && y < sh; y++) {
        for (int x = x0; x < x1 && x < sw; x++) {
            sum += src[y * sw + x];
            n++;
        }
    }
    return n ? sum / n : 0;
}

/* Bilinear sample at 1/256-pixel precision, for magnification. */
static inline int ui_sample_bilinear(const uint8_t *src, int sw, int sh,
                                     int fx, int fy) {
    int x0 = fx >> 8, y0 = fy >> 8;
    int tx = fx & 255, ty = fy & 255;
    int x1, y1, a, b, c, d, top, bot;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x0 >= sw) x0 = sw - 1;
    if (y0 >= sh) y0 = sh - 1;
    x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
    y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
    a = src[y0 * sw + x0]; b = src[y0 * sw + x1];
    c = src[y1 * sw + x0]; d = src[y1 * sw + x1];
    top = a + (b - a) * tx / 256;
    bot = c + (d - c) * tx / 256;
    return top + (bot - top) * ty / 256;
}

static inline struct ui_font *ui_font_get(int size) {
    static struct ui_font fonts[UI_FONT_COUNT];
    struct ui_font_ink *ink = ui_font_ink();
    struct ui_font *f;
    int pct, src_h;

    if (size < 0 || size >= UI_FONT_COUNT)
        size = UI_FONT_BODY;
    f = &fonts[size];
    if (f->built)
        return f;

    pct = ui_font_scale_pct[size];
    src_h = ink->bottom - ink->top + 1;
    f->gw = (KFONT_WIDTH * pct + 50) / 100;
    f->gh = (src_h * pct + 50) / 100;
    if (f->gw < 1) f->gw = 1;
    if (f->gh < 1) f->gh = 1;

    f->cov = (uint8_t *)malloc((size_t)KFONT_COUNT * f->gw * f->gh);
    if (!f->cov) {
        /* Fall back to the native cell rather than failing to draw. */
        f->gw = KFONT_WIDTH;
        f->gh = src_h;
        f->built = -1;
        return f;
    }

    for (int g = 0; g < KFONT_COUNT; g++) {
        const uint8_t *src = &kfont_alpha[g][ink->top][0];
        uint8_t *dst = f->cov + (size_t)g * f->gw * f->gh;
        for (int y = 0; y < f->gh; y++) {
            for (int x = 0; x < f->gw; x++) {
                int v;
                if (pct < 100) {
                    v = ui_sample_box(src, KFONT_WIDTH, src_h,
                                      x * KFONT_WIDTH / f->gw,
                                      (x + 1) * KFONT_WIDTH / f->gw,
                                      y * src_h / f->gh,
                                      (y + 1) * src_h / f->gh);
                } else {
                    v = ui_sample_bilinear(src, KFONT_WIDTH, src_h,
                                           x * (KFONT_WIDTH - 1) * 256 /
                                               (f->gw > 1 ? f->gw - 1 : 1),
                                           y * (src_h - 1) * 256 /
                                               (f->gh > 1 ? f->gh - 1 : 1));
                }
                dst[y * f->gw + x] = (uint8_t)ui_clamp(v, 0, 255);
            }
        }
    }
    f->built = 1;
    return f;
}

static inline int ui_font_height(int size) { return ui_font_get(size)->gh; }
static inline int ui_font_advance(int size) { return ui_font_get(size)->gw; }

/* Line box used for vertical centring; a little taller than the ink so
 * stacked rows do not touch. */
static inline int ui_line_height(int size) {
    int h = ui_font_get(size)->gh;
    return h + ui_max(2, h / 4);
}

/* ------------------------------------------------------------------ */
/* Measurement                                                         */
/* ------------------------------------------------------------------ */

/* Decode one UTF-8 scalar and always make progress; invalid bytes yield
 * U+FFFD so arbitrary file or network text cannot wedge a GUI application. */
static inline uint32_t ui_utf8_next(const char **text) {
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

/* Codepoints outside the built-in ASCII face come from the on-demand Unicode
 * table, which is 1-bit and natively KFONT_WIDTH-based; scale its advance the
 * same way so mixed text stays on the grid. */
static inline int ui_cp_advance(uint32_t cp, int size) {
    struct ui_font *f = ui_font_get(size);
    if (cp >= KFONT_FIRST && cp < KFONT_FIRST + KFONT_COUNT)
        return f->gw;
    if (cp >= 0x80u) {
        uint8_t bits[FONT_GLYPH_BYTES];
        int native = font_glyph(cp, bits, sizeof(bits));
        if (native > 0)
            return ui_max(1, native * ui_font_scale_pct[size] / 100);
    }
    return f->gw;
}

static inline int ui_text_width(const char *s, int size) {
    int w = 0;
    while (s && *s) {
        uint32_t cp = ui_utf8_next(&s);
        if (cp == '\n')
            break;
        w += ui_cp_advance(cp, size);
    }
    return w;
}

/* ------------------------------------------------------------------ */
/* Drawing                                                             */
/* ------------------------------------------------------------------ */

/* Blit one cached glyph.  Synthetic bold takes the max of a column and its
 * left neighbour, which thickens strokes without a second cache. */
static inline void ui_blit_glyph(struct ui_surface *s, int x, int y,
                                 const uint8_t *cov, int gw, int gh,
                                 uint32_t color, int bold, int alpha) {
    for (int py = 0; py < gh; py++) {
        int ty = y + py;
        if (ty < s->clip.y || ty >= s->clip.y + s->clip.h || ty < 0 ||
            ty >= s->h)
            continue;
        const uint8_t *row = cov + (size_t)py * gw;
        uint32_t *dst = s->px + (size_t)ty * s->stride;
        for (int px = 0; px < gw; px++) {
            int tx = x + px;
            int a = row[px];
            if (bold && px > 0 && row[px - 1] > a)
                a = row[px - 1];
            if (a <= 0)
                continue;
            if (tx < s->clip.x || tx >= s->clip.x + s->clip.w || tx < 0 ||
                tx >= s->w)
                continue;
            if (alpha < 255)
                a = a * alpha / 255;
            dst[tx] = ui_surface_blend(s, color, dst[tx], (uint32_t)a);
        }
    }
}

/* Scale and blit a Unicode glyph straight from the 1-bit table.  Uncached:
 * CJK is rare in chrome, and caching an open-ended range is not worth it.
 *
 * The source rows are the glyph's own ink bounds, not the ASCII face's.  CJK
 * forms fill the whole 28-row cell while the Latin face occupies rows 2..26,
 * so sampling through the ASCII bounds crops a CJK glyph top and bottom --
 * the visible symptom is Chinese IME candidates with their upper and lower
 * strokes shaved off. */
static inline void ui_blit_unicode(struct ui_surface *s, int x, int y,
                                   uint32_t cp, int size, uint32_t color,
                                   int bold, int alpha) {
    uint8_t bits[FONT_GLYPH_BYTES];
    struct ui_font_ink *ink = ui_font_ink();
    int native_w = font_glyph(cp, bits, sizeof(bits));
    int pct = ui_font_scale_pct[size];
    int top, bottom, src_h, gw, gh, baseline;
    if (native_w <= 0)
        return;

    /* Ink bounds of this glyph. */
    top = KFONT_HEIGHT;
    bottom = -1;
    for (int row = 0; row < KFONT_HEIGHT; row++) {
        for (int col = 0; col < native_w; col++) {
            if (!(bits[row * FONT_GLYPH_STRIDE + col / 8] &
                  (uint8_t)(0x80u >> (col & 7))))
                continue;
            if (row < top) top = row;
            if (row > bottom) bottom = row;
            break;
        }
    }
    if (bottom < top)
        return; /* blank glyph */
    src_h = bottom - top + 1;

    gw = ui_max(1, native_w * pct / 100);
    gh = ui_max(1, src_h * pct / 100);

    /* Sit the glyph on the same baseline as the ASCII cache so a mixed run
     * does not stagger: both are measured down from the top of the line box.
     * A CJK form that starts above the Latin ink top clamps to 0 rather than
     * drawing outside the caller's box. */
    baseline = (top - ink->top) * pct / 100;
    if (baseline < 0)
        baseline = 0;

    for (int py = 0; py < gh; py++) {
        for (int px = 0; px < gw; px++) {
            /* Sample the centre of each destination pixel's source span.
             * Truncating `py * src_h / gh` never reaches the final source
             * row when downscaling, which drops the bottom stroke of a glyph
             * that fills its cell -- and CJK forms always do. */
            int sx = (px * 2 + 1) * native_w / (gw * 2);
            int sy = top + (py * 2 + 1) * src_h / (gh * 2);
            int on;
            if (sx >= native_w) sx = native_w - 1;
            if (sy > bottom) sy = bottom;
            on = (bits[sy * FONT_GLYPH_STRIDE + sx / 8] &
                  (uint8_t)(0x80u >> (sx & 7))) != 0;
            if (!on && bold && sx > 0)
                on = (bits[sy * FONT_GLYPH_STRIDE + (sx - 1) / 8] &
                      (uint8_t)(0x80u >> ((sx - 1) & 7))) != 0;
            if (on)
                ui_pixel_a(s, x + px, y + baseline + py, color, alpha);
        }
    }
}

/* Draw a string with its top-left at (x,y).  Returns the advance. */
static inline int ui_text_at(struct ui_surface *s, int x, int y,
                             const char *str, int size, uint32_t color,
                             int bold, int alpha) {
    struct ui_font *f = ui_font_get(size);
    int start = x;
    while (str && *str) {
        uint32_t cp = ui_utf8_next(&str);
        if (cp == '\n' || cp == '\r')
            break;
        if (x >= s->clip.x + s->clip.w)
            break;
        if (cp >= KFONT_FIRST && cp < KFONT_FIRST + KFONT_COUNT &&
            f->built > 0) {
            ui_blit_glyph(s, x, y,
                          f->cov + (size_t)(cp - KFONT_FIRST) * f->gw * f->gh,
                          f->gw, f->gh, color, bold, alpha);
            x += f->gw;
        } else if (cp >= 0x80u) {
            ui_blit_unicode(s, x, y, cp, size, color, bold, alpha);
            x += ui_cp_advance(cp, size);
        } else {
            x += f->gw;
        }
    }
    return x - start;
}

static inline void ui_text(struct ui_surface *s, int x, int y, const char *str,
                           int size, uint32_t color) {
    ui_text_at(s, x, y, str, size, color, 0, 255);
}

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

struct ui_text_style {
    int size;
    uint32_t color;
    int bold;
    int align;
    int alpha;
    int ellipsis;   /* truncate overlong text with a trailing ellipsis */
};

static inline struct ui_text_style ui_style(int size, uint32_t color) {
    struct ui_text_style st;
    st.size = size;
    st.color = color;
    st.bold = 0;
    st.align = UI_ALIGN_LEFT;
    st.alpha = 255;
    st.ellipsis = 1;
    return st;
}

/* Copy the longest prefix of src that fits in max_w, appending "..." when
 * anything was dropped.  Truncation is on codepoint boundaries. */
static inline void ui_text_ellipsize(char *dst, size_t cap, const char *src,
                                     int max_w, int size) {
    const char *cursor = src;
    const char *last_fit = src;
    int dots, width = 0;
    size_t n;

    if (!dst || cap == 0)
        return;
    dst[0] = 0;
    if (!src)
        return;
    if (ui_text_width(src, size) <= max_w) {
        n = 0;
        while (n + 1 < cap && src[n]) { dst[n] = src[n]; n++; }
        dst[n] = 0;
        return;
    }
    dots = ui_text_width("...", size);
    while (*cursor) {
        const char *next = cursor;
        uint32_t cp = ui_utf8_next(&next);
        if (!cp)
            break;
        width += ui_cp_advance(cp, size);
        if (width + dots > max_w)
            break;
        cursor = next;
        last_fit = next;
    }
    n = (size_t)(last_fit - src);
    /* Reserve room for the ellipsis and terminator. */
    if (n + 4 > cap)
        n = cap > 4 ? cap - 4 : 0;
    for (size_t i = 0; i < n; i++)
        dst[i] = src[i];
    if (n + 4 <= cap) {
        dst[n] = '.'; dst[n + 1] = '.'; dst[n + 2] = '.'; dst[n + 3] = 0;
    } else {
        dst[n] = 0;
    }
}

/* Draw text inside r, aligned horizontally and centred vertically. */
static inline void ui_text_in(struct ui_surface *s, struct ui_rect r,
                              const char *str, struct ui_text_style st) {
    char buf[256];
    const char *draw = str;
    struct ui_rect saved;
    int tw, x, y;

    if (!str || !*str || ui_rect_empty(r))
        return;
    tw = ui_text_width(str, st.size);
    if (st.ellipsis && tw > r.w) {
        ui_text_ellipsize(buf, sizeof(buf), str, r.w, st.size);
        draw = buf;
        tw = ui_text_width(buf, st.size);
    }
    if (st.align == UI_ALIGN_CENTER)
        x = r.x + (r.w - tw) / 2;
    else if (st.align == UI_ALIGN_RIGHT)
        x = r.x + r.w - tw;
    else
        x = r.x;
    y = r.y + (r.h - ui_font_height(st.size)) / 2;

    saved = ui_clip_push(s, r);
    ui_text_at(s, x, y, draw, st.size, st.color, st.bold, st.alpha);
    ui_clip_pop(s, saved);
}

#endif
