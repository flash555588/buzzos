#ifndef BUZZOS_PALETTE_H
#define BUZZOS_PALETTE_H

/* Shared RGB color helpers for the BuzzOS user-space GUI.
 *
 * The desktop and apps compose in 32-bit 0x00RRGGBB (no alpha in the pixel
 * value; coverage uses plt_blend).  Scanout is always truecolor: Bochs VBE /
 * Limine linear FB and virtio-gpu 2D both consume RGB32, matching modern OS
 * compositor working formats.  There is no 8-bit indexed UI path.
 */

#include <stdint.h>

/* Gray ramp matching the old palette gray(0..14): 32 + n * 14. */
static __attribute__((unused)) uint32_t plt_gray(int n) {
    if (n < 0) n = 0;
    if (n > 14) n = 14;
    uint32_t v = (uint32_t)(32 + n * 14);
    return (v << 16) | (v << 8) | v;
}

/* 6x6x6 cube sample (r,g,b each 0..5) → 0xRRGGBB. */
static __attribute__((unused)) uint32_t plt_cube(int r, int g, int b) {
    if (r < 0) r = 0; if (r > 5) r = 5;
    if (g < 0) g = 0; if (g > 5) g = 5;
    if (b < 0) b = 0; if (b > 5) b = 5;
    uint32_t rv = (uint32_t)r * 51u;
    uint32_t gv = (uint32_t)g * 51u;
    uint32_t bv = (uint32_t)b * 51u;
    return (rv << 16) | (gv << 8) | bv;
}

static __attribute__((unused)) uint32_t plt_rgb(int r, int g, int b) {
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Blend fg over bg with 0-255 coverage; returns 0x00RRGGBB (no quantization). */
static uint32_t plt_blend(uint32_t fg, uint32_t bg, int alpha) {
    uint32_t inv;
    uint32_t r, g, bl;
    if (alpha <= 0)
        return bg & 0x00FFFFFFu;
    if (alpha >= 255)
        return fg & 0x00FFFFFFu;
    inv = (uint32_t)(255 - alpha);
    r = (((fg >> 16) & 0xFFu) * (uint32_t)alpha +
         ((bg >> 16) & 0xFFu) * inv + 127u) / 255u;
    g = (((fg >> 8) & 0xFFu) * (uint32_t)alpha +
         ((bg >> 8) & 0xFFu) * inv + 127u) / 255u;
    bl = ((fg & 0xFFu) * (uint32_t)alpha +
          (bg & 0xFFu) * inv + 127u) / 255u;
    return (r << 16) | (g << 8) | bl;
}

/* Cohesive dark theme — same visual targets as the former fixed palette. */
#define THEME_DESKTOP_BASE     0x182848u
#define THEME_DESKTOP_DEEP     0x202020u
#define THEME_TOPBAR           THEME_DESKTOP_DEEP
#define THEME_TOPBAR_BORDER    0x285080u

#define THEME_ACCENT           0x70B0F8u
#define THEME_ACCENT_DIM       0x5088D8u
#define THEME_ACCENT_SOFT      0x285080u
#define THEME_FOCUS            THEME_ACCENT

#define THEME_WIN_BODY         0x2E2E2Eu
#define THEME_WIN_PANEL        0x3C3C3Cu
#define THEME_WIN_CONTROL      0x4A4A4Au
#define THEME_WIN_HOVER        0x585858u
#define THEME_WIN_PRESSED      THEME_WIN_PANEL
#define THEME_WIN_BORDER_ACT   0x3868B0u
#define THEME_WIN_BORDER_INACT THEME_WIN_CONTROL
#define THEME_TITLE_ACT        THEME_WIN_PANEL
#define THEME_TITLE_INACT      THEME_WIN_BODY

#define THEME_TEXT             0xFFFFFFu
#define THEME_TEXT_DIM         0xACACACu
#define THEME_TEXT_FAINT       0x828282u
#define THEME_TEXT_ON_LIGHT    0x000000u

#define THEME_CLOSE_RED        0xFF6666u
#define THEME_DANGER           THEME_CLOSE_RED
#define THEME_DANGER_DIM       0x6C2C28u
#define THEME_MIN_YELLOW       0xFFCC33u
#define THEME_MAX_GREEN        0x48B870u

#define THEME_APP_BG           THEME_WIN_BODY
#define THEME_TOOLBAR_BG       THEME_WIN_PANEL
#define THEME_PANEL_BG         THEME_WIN_BODY
#define THEME_PANEL_RAISED     THEME_WIN_PANEL
#define THEME_DIVIDER          THEME_WIN_CONTROL
#define THEME_FIELD_BG         THEME_DESKTOP_DEEP
#define THEME_FIELD_BORDER     THEME_WIN_HOVER
#define THEME_FIELD_TEXT       THEME_TEXT
#define THEME_DOCUMENT_BG      0xE4E4E4u
#define THEME_DOCUMENT_TEXT    THEME_TEXT_ON_LIGHT
#define THEME_LIST_BG          THEME_WIN_BODY
#define THEME_LIST_ALT         THEME_PANEL_RAISED
#define THEME_LIST_HEADER      THEME_WIN_CONTROL
#define THEME_LIST_TEXT        THEME_TEXT
#define THEME_SELECTION_BG     THEME_ACCENT_DIM
#define THEME_SELECTION_SOFT   THEME_ACCENT_SOFT
#define THEME_SELECTION_TEXT   THEME_TEXT

/* Built-in 15x28 font has empty rows above cap height; user-space draw
 * glyphs this many pixels higher so descenders stay in tight clips. */
#define PLT_FONT_Y_SHIFT 4

#endif
