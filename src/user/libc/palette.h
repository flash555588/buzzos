#ifndef BUZZOS_PALETTE_H
#define BUZZOS_PALETTE_H

/* Shared color system for the BuzzOS user-space GUI.
 *
 * The desktop and apps compose in 32-bit 0x00RRGGBB (no alpha in the pixel
 * value; coverage uses plt_blend).  Scanout is always truecolor: Bochs VBE /
 * Limine linear FB and virtio-gpu both consume RGB32, matching modern OS
 * compositor working formats.  There is no 8-bit indexed UI path.
 *
 * The token layer is a dark theme in the modern desktop idiom: surfaces are
 * defined as translucent white overlaid on a near-black base rather than as
 * flat colors, which is why the constants below cluster so
 * tightly: the visible difference between a resting control and a hovered one
 * is a few percent of white, not a different hue.  Each token records the
 * overlay it was resolved from, so a value can be rederived if the base tone
 * ever moves.
 *
 * Three layers, in dependency order:
 *   1. UI_*   design tokens.  New code uses these.
 *   2. Geometry and elevation constants (radii, taskbar metrics, shadows).
 *   3. THEME_* legacy aliases, retained so every existing app keeps building.
 *      They are pointed at the design tokens, so apps track the new look
 *      before they are individually migrated.
 */

#include <stdint.h>

/* Gray ramp matching the old palette gray(0..14): 32 + n * 14. */
static __attribute__((unused)) uint32_t plt_gray(int n) {
    if (n < 0) n = 0;
    if (n > 14) n = 14;
    uint32_t v = (uint32_t)(32 + n * 14);
    return (v << 16) | (v << 8) | v;
}

/* 6x6x6 cube sample (r,g,b each 0..5) -> 0xRRGGBB. */
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

/* Layered surfaces are "white at N% over the base".  Percent is in tenths so
 * the published values (5.12%, 8.37%, ...) survive without floating point. */
static __attribute__((unused)) uint32_t plt_overlay_tenths(
    uint32_t over, uint32_t base, int tenths_percent) {
    int alpha = (tenths_percent * 255 + 500) / 1000;
    return plt_blend(over, base, alpha);
}

/* Scale a color toward black (pct < 100) or white (pct > 100). */
static __attribute__((unused)) uint32_t plt_shade(uint32_t color, int pct) {
    if (pct == 100)
        return color & 0x00FFFFFFu;
    if (pct < 100)
        return plt_blend(color, 0x000000u, pct * 255 / 100);
    return plt_blend(0xFFFFFFu, color, (pct - 100) * 255 / 100);
}

/* Perceptual luminance 0..255, for picking readable text over a fill. */
static __attribute__((unused)) int plt_luma(uint32_t color) {
    int r = (int)((color >> 16) & 0xFFu);
    int g = (int)((color >> 8) & 0xFFu);
    int b = (int)(color & 0xFFu);
    return (r * 77 + g * 151 + b * 28) >> 8;
}

/* ------------------------------------------------------------------ */
/* 1. design tokens (dark theme)                                */
/* ------------------------------------------------------------------ */

/* Accent ramp around the default accent blue (#0078D4).  Dark theme fills
 * use the light end with black text, which is what keeps accent buttons
 * reading as bright cyan-blue rather than navy. */
#define UI_ACCENT_DARK2       0x003E92u
#define UI_ACCENT_DARK1       0x005FB8u
#define UI_ACCENT_BASE        0x0078D4u
#define UI_ACCENT_LIGHT1      0x4CC2FFu
#define UI_ACCENT_LIGHT2      0x60CDFFu
#define UI_ACCENT_LIGHT3      0x99EBFFu

/* Dark theme maps accent fills to the light end and text on them to black. */
#define UI_ACCENT_FILL        UI_ACCENT_LIGHT2
#define UI_ACCENT_FILL_HOVER  UI_ACCENT_LIGHT3
#define UI_ACCENT_FILL_PRESS  UI_ACCENT_LIGHT1
#define UI_ACCENT_TEXT        UI_ACCENT_LIGHT2

/* Backdrops.  Mica is the desktop-tinted window backdrop; solid is the opaque
 * fallback used when a window is not composited against the wallpaper. */
#define UI_BG_MICA            0x202020u
#define UI_BG_MICA_ALT        0x1C1C1Cu
#define UI_BG_SOLID           0x272727u
#define UI_BG_LAYER           0x2B2B2Bu  /* white 5.12% over mica  */
#define UI_BG_LAYER_ALT       0x2D2D2Du
#define UI_BG_ACRYLIC         0x2C2C2Cu  /* flyout / Start acrylic tint */
#define UI_BG_ACRYLIC_THIN    0x1F1F1Fu  /* taskbar acrylic tint        */
#define UI_BG_SMOKE           0x000000u  /* modal scrim, used with alpha */

/* Controls.  All are white-over-mica overlays; see plt_overlay_tenths. */
#define UI_CTRL_REST          0x2D2D2Du  /* white  6.05% */
#define UI_CTRL_HOVER         0x333333u  /* white  8.37% */
#define UI_CTRL_PRESSED       0x272727u  /* white  3.26% */
#define UI_CTRL_DISABLED      0x292929u  /* white  4.19% */

/* Subtle fills sit on transparent backgrounds (taskbar buttons, menu rows). */
#define UI_SUBTLE_HOVER       0x353535u
#define UI_SUBTLE_PRESSED     0x2A2A2Au

/* Strokes.  Surface stroke is the window outline that separates a window from
 * the wallpaper; control strokes are the hairlines inside chrome. */
#define UI_STROKE_SURFACE     0x424242u
#define UI_STROKE_CONTROL     0x3F3F3Fu
#define UI_STROKE_SECONDARY   0x353535u
#define UI_STROKE_DIVIDER     0x333333u
#define UI_STROKE_FOCUS       0xFFFFFFu

/* Text. */
#define UI_TEXT_PRIMARY       0xFFFFFFu
#define UI_TEXT_SECONDARY     0xCFCFCFu  /* white 78.60% */
#define UI_TEXT_TERTIARY      0x999999u  /* white 54.42% */
#define UI_TEXT_DISABLED      0x717171u  /* white 36.28% */
#define UI_TEXT_ON_ACCENT     0x000000u
#define UI_TEXT_ON_LIGHT      0x000000u

/* System status colors (dark-theme status palette). */
#define UI_SYS_CRITICAL       0xFF99A4u
#define UI_SYS_SUCCESS        0x6CCB5Fu
#define UI_SYS_CAUTION        0xFCE100u
#define UI_SYS_ATTENTION      0x60CDFFu
#define UI_SYS_NEUTRAL        0x999999u

/* Caption button hover states.  Close goes red; the others take a subtle fill,
 * exactly as modern desktops do. */
#define UI_CAPTION_CLOSE      0xC42B1Cu
#define UI_CAPTION_CLOSE_PRESS 0xB4271Bu

/* Desktop wallpaper: a blue bloom over near-black, a calm desktop backdrop.  The shell renders a vertical gradient between these with a radial
 * highlight; a solid fill of UI_WALL_BASE is the degenerate fallback. */
#define UI_WALL_BASE          0x0A1020u
#define UI_WALL_MID           0x123058u
#define UI_WALL_GLOW          0x1E5C9Eu

/* ------------------------------------------------------------------ */
/* 2. Geometry and elevation                                           */
/* ------------------------------------------------------------------ */

/* 8px on overlays and windows, 4px on controls. */
#define UI_RADIUS_CONTROL     4
#define UI_RADIUS_OVERLAY     8
#define UI_RADIUS_WINDOW      8

/* Taskbar metrics at 100% scale. */
#define UI_TASKBAR_H          48
#define UI_TASKBAR_ICON       24
#define UI_TASKBAR_BTN_W      44
#define UI_TASKBAR_BTN_H      40

/* Caption buttons are 46x32 and deliberately not square. */
#define UI_CAPTION_BTN_W      46
#define UI_CAPTION_BTN_H      32
#define UI_TITLEBAR_H         32

/* Elevation: shadow radius and peak opacity (0-255) per level.  Level 0 is
 * flat, 1 is a resting card, 2 a flyout, 3 a dialog. */
#define UI_ELEV_CARD_R        6
#define UI_ELEV_CARD_A        56
#define UI_ELEV_FLYOUT_R      12
#define UI_ELEV_FLYOUT_A      92
#define UI_ELEV_DIALOG_R      20
#define UI_ELEV_DIALOG_A      120

/* Standard spacing step; Layouts are built on multiples of 4. */
#define UI_SPACE_XS           4
#define UI_SPACE_S            8
#define UI_SPACE_M            12
#define UI_SPACE_L            16
#define UI_SPACE_XL           24

/* Motion, in milliseconds.  "fast" is for hovers, "normal" for
 * flyouts opening, "slow" for window state changes. */
#define UI_MOTION_FAST        90
#define UI_MOTION_NORMAL      180
#define UI_MOTION_SLOW        300

/* ------------------------------------------------------------------ */
/* 3. Legacy THEME_* aliases                                           */
/* ------------------------------------------------------------------ */

/* Kept so the existing shell and apps build unchanged.  Each is pointed at
 * the closest design token, chosen by how the alias is actually used rather
 * than by name: THEME_ACCENT is a text/edge color at every call site, so it
 * maps to the light accent, while THEME_ACCENT_DIM is a fill under white text
 * and maps to the mid accent that keeps that text readable. */
#define THEME_DESKTOP_BASE     UI_WALL_MID
#define THEME_DESKTOP_DEEP     UI_BG_MICA
#define THEME_TOPBAR           UI_BG_ACRYLIC_THIN
#define THEME_TOPBAR_BORDER    UI_STROKE_SURFACE

#define THEME_ACCENT           UI_ACCENT_LIGHT2
#define THEME_ACCENT_DIM       UI_ACCENT_BASE
#define THEME_ACCENT_SOFT      UI_ACCENT_DARK1
#define THEME_FOCUS            UI_ACCENT_LIGHT2

#define THEME_WIN_BODY         UI_BG_SOLID
#define THEME_WIN_PANEL        UI_BG_LAYER
#define THEME_WIN_CONTROL      UI_CTRL_REST
#define THEME_WIN_HOVER        UI_CTRL_HOVER
#define THEME_WIN_PRESSED      UI_CTRL_PRESSED
#define THEME_WIN_BORDER_ACT   UI_STROKE_SURFACE
#define THEME_WIN_BORDER_INACT UI_STROKE_CONTROL
#define THEME_TITLE_ACT        UI_BG_LAYER
#define THEME_TITLE_INACT      UI_BG_SOLID

#define THEME_TEXT             UI_TEXT_PRIMARY
#define THEME_TEXT_DIM         UI_TEXT_SECONDARY
#define THEME_TEXT_FAINT       UI_TEXT_TERTIARY
#define THEME_TEXT_ON_LIGHT    UI_TEXT_ON_LIGHT

#define THEME_CLOSE_RED        UI_SYS_CRITICAL
#define THEME_DANGER           UI_SYS_CRITICAL
#define THEME_DANGER_DIM       0x5C1F1Fu
#define THEME_MIN_YELLOW       UI_SYS_CAUTION
#define THEME_MAX_GREEN        UI_SYS_SUCCESS

#define THEME_APP_BG           UI_BG_SOLID
#define THEME_TOOLBAR_BG       UI_BG_LAYER
#define THEME_PANEL_BG         UI_BG_SOLID
#define THEME_PANEL_RAISED     UI_BG_LAYER
#define THEME_DIVIDER          UI_STROKE_DIVIDER
#define THEME_FIELD_BG         UI_BG_MICA_ALT
#define THEME_FIELD_BORDER     UI_STROKE_CONTROL
#define THEME_FIELD_TEXT       UI_TEXT_PRIMARY

/* Document surfaces stay light: the browser renders real pages, which assume
 * paper.  A dark editor surface is a separate token so text editors can move
 * to the dark theme without dragging page rendering with them. */
#define THEME_DOCUMENT_BG      0xE8E8E8u
#define THEME_DOCUMENT_TEXT    UI_TEXT_ON_LIGHT
#define THEME_EDITOR_BG        UI_BG_MICA_ALT
#define THEME_EDITOR_TEXT      UI_TEXT_PRIMARY

#define THEME_LIST_BG          UI_BG_SOLID
#define THEME_LIST_ALT         UI_BG_LAYER
#define THEME_LIST_HEADER      UI_CTRL_REST
#define THEME_LIST_TEXT        UI_TEXT_PRIMARY
#define THEME_SELECTION_BG     UI_ACCENT_BASE
#define THEME_SELECTION_SOFT   UI_ACCENT_DARK1
#define THEME_SELECTION_TEXT   UI_TEXT_PRIMARY

/* Built-in 15x28 font has empty rows above cap height; user-space draw
 * glyphs this many pixels higher so descenders stay in tight clips. */
#define PLT_FONT_Y_SHIFT 4

#endif
