#ifndef BUZZOS_PALETTE_H
#define BUZZOS_PALETTE_H

/* Shared 8-bit palette helpers for the BuzzOS user-space GUI.
 *
 * The kernel framebuffer owns a fixed 256-entry palette (see
 * src/kernel/drv/fb.c): 16 VGA colors, 9 custom accents, a 15-step gray
 * ramp and a 6x6x6 RGB cube.  Everything drawn in user space is a
 * palette index, so alpha effects (anti-aliased text, soft shadows) are
 * emulated by blending in RGB space and quantizing back to the nearest
 * palette entry through a small 4:4:4 lookup table.
 */

#include <stdint.h>

static int plt_cube(int r, int g, int b) {
    if (r < 0) r = 0; if (r > 5) r = 5;
    if (g < 0) g = 0; if (g > 5) g = 5;
    if (b < 0) b = 0; if (b > 5) b = 5;
    return 40 + r * 36 + g * 6 + b;
}

static int plt_gray(int n) {
    if (n < 0) n = 0;
    if (n > 14) n = 14;
    return 25 + n;
}

/* Palette index -> 0xRRGGBB.  Mirrors palette_rgb_compute() in the
 * kernel framebuffer driver; keep the two in sync. */
static uint32_t plt_index_to_rgb(int index) {
    static const uint32_t base[25] = {
        0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
        0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
        0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
        0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
        0x182848, 0x285080, 0x3868B0, 0x5088D8,
        0x70B0F8, 0x207850, 0x48B870, 0x503820,
        0x6C2C28
    };
    index &= 0xFF;
    if (index < (int)(sizeof(base) / sizeof(base[0])))
        return base[index];
    if (index < 40) {
        uint32_t v = (uint32_t)(32 + (index - 25) * 14);
        return (v << 16) | (v << 8) | v;
    }
    {
        uint8_t n = (uint8_t)(index - 40);
        uint32_t r = (uint32_t)(n / 36u) * 51u;
        uint32_t g = (uint32_t)((n / 6u) % 6u) * 51u;
        uint32_t b = (uint32_t)(n % 6u) * 51u;
        return (r << 16) | (g << 8) | b;
    }
}

static uint8_t plt_lut[4096];
static int plt_lut_ready;

static void plt_lut_build(void) {
    if (plt_lut_ready)
        return;
    for (int r = 0; r < 16; r++) {
        for (int g = 0; g < 16; g++) {
            for (int b = 0; b < 16; b++) {
                int rv = r * 255 / 15;
                int gv = g * 255 / 15;
                int bv = b * 255 / 15;
                int best = 0;
                long best_dist = 0x7FFFFFFFl;
                for (int i = 0; i < 256; i++) {
                    uint32_t rgb = plt_index_to_rgb(i);
                    int dr = (int)((rgb >> 16) & 0xFFu) - rv;
                    int dg = (int)((rgb >> 8) & 0xFFu) - gv;
                    int db = (int)(rgb & 0xFFu) - bv;
                    long dist = (long)dr * dr + (long)dg * dg +
                                (long)db * db;
                    if (dist < best_dist) {
                        best_dist = dist;
                        best = i;
                    }
                }
                plt_lut[(r << 8) | (g << 4) | b] = (uint8_t)best;
            }
        }
    }
    plt_lut_ready = 1;
}

/* 0xRRGGBB -> nearest palette index. */
static int plt_rgb_to_index(uint32_t rgb) {
    plt_lut_build();
    {
        int r = (int)((rgb >> 16) & 0xFFu) >> 4;
        int g = (int)((rgb >> 8) & 0xFFu) >> 4;
        int b = (int)(rgb & 0xFFu) >> 4;
        return plt_lut[(r << 8) | (g << 4) | b];
    }
}

static int plt_rgb(int r, int g, int b) {
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return plt_rgb_to_index(((uint32_t)r << 16) | ((uint32_t)g << 8) |
                            (uint32_t)b);
}

/* Blend palette color fg over palette color bg with 0-255 coverage and
 * return the nearest resulting palette index. */
static int plt_blend(int fg, int bg, int alpha) {
    uint32_t f, b;
    uint32_t r, g, bl;
    uint32_t inv;
    if (alpha <= 0)
        return bg & 0xFF;
    if (alpha >= 255)
        return fg & 0xFF;
    f = plt_index_to_rgb(fg);
    b = plt_index_to_rgb(bg);
    inv = (uint32_t)(255 - alpha);
    r = (((f >> 16) & 0xFFu) * (uint32_t)alpha +
         ((b >> 16) & 0xFFu) * inv + 127u) / 255u;
    g = (((f >> 8) & 0xFFu) * (uint32_t)alpha +
         ((b >> 8) & 0xFFu) * inv + 127u) / 255u;
    bl = ((f & 0xFFu) * (uint32_t)alpha +
          (b & 0xFFu) * inv + 127u) / 255u;
    return plt_rgb_to_index((r << 16) | (g << 8) | bl);
}

/* Modern dark theme palette (VS Code / macOS dark inspired). */
#define THEME_ACCENT          plt_cube(0, 3, 5)   /* (0,153,255)   */
#define THEME_ACCENT_DIM      plt_cube(0, 2, 4)   /* (0,102,204)   */
#define THEME_ACCENT_SOFT     plt_cube(0, 1, 2)   /* (0,51,102)    */
#define THEME_WIN_BODY        plt_gray(1)         /* 46,46,46      */
#define THEME_WIN_PANEL       plt_gray(2)         /* 60,60,60      */
#define THEME_WIN_CONTROL     plt_gray(3)         /* 74,74,74      */
#define THEME_WIN_BORDER_ACT  THEME_ACCENT_DIM
#define THEME_WIN_BORDER_INACT plt_gray(0)
#define THEME_TITLE_ACT       plt_gray(2)
#define THEME_TITLE_INACT     plt_gray(1)
#define THEME_TEXT            15                  /* white         */
#define THEME_TEXT_DIM        plt_gray(9)         /* 158,158,158   */
#define THEME_TEXT_FAINT      plt_gray(7)         /* 130,130,130   */
#define THEME_CLOSE_RED       plt_cube(5, 2, 2)   /* (255,102,102) */
#define THEME_MIN_YELLOW      plt_cube(5, 4, 1)   /* (255,204,51)  */
#define THEME_MAX_GREEN       plt_cube(2, 4, 2)   /* (102,204,102) */

/* The built-in 12x22 font carries ~6px of empty space above cap height,
 * so glyphs visually sit low in their line box and descenders (y/g/p)
 * get clipped by tight clip rects.  User-space renderers draw glyphs
 * this many pixels higher to compensate (the vacated rows were empty). */
#define PLT_FONT_Y_SHIFT 4

#endif
