/* Host-side regression test for the uikit rendering kernel and the appui
 * widget layer built on it.
 *
 * Runs natively (not under BuzzOS): both layers are pure integer pixel math
 * over a caller-supplied buffer, so the parts most likely to break silently --
 * corner coverage, clipping, stride handling, glyph metrics, and the
 * scrollbar geometry shared between painting and hit-testing -- can be
 * checked without booting.  Build and run with "make uikit-test".
 */
#include <stdlib.h>
#include <string.h>
#include "uikit.h"
#include "appui.h"

static int failures;
static void ck(int cond, const char *what) {
    if (!cond) { printf("FAIL: %s\n", what); failures++; }
}
static void ck_eq(int got, int want, const char *what) {
    if (got != want) { printf("FAIL: %s (got %d want %d)\n", what, got, want); failures++; }
}

/* Stub for the kernel-backed Unicode font table.
 *
 * Returns one synthetic glyph that fills the entire 28-row cell -- a hollow
 * box touching row 0 and row 27.  That is the shape a CJK ideograph has, and
 * it is exactly the case that differs from the Latin face, which occupies
 * only rows 2..26.  Every other codepoint is absent. */
#define TEST_FULL_CELL_CP 0x4E00u

int font_glyph(uint32_t cp, uint8_t *bits, size_t cap) {
    if (cp != TEST_FULL_CELL_CP || cap < FONT_GLYPH_BYTES)
        return 0;
    memset(bits, 0, cap);
    for (int row = 0; row < KFONT_HEIGHT; row++) {
        int edge = (row == 0 || row == KFONT_HEIGHT - 1);
        for (int col = 0; col < KFONT_WIDTH; col++) {
            if (!edge && col != 0 && col != KFONT_WIDTH - 1)
                continue;
            bits[row * FONT_GLYPH_STRIDE + col / 8] |=
                (uint8_t)(0x80u >> (col & 7));
        }
    }
    return KFONT_WIDTH;
}

#define W 120
#define H 120
static uint32_t buf[W * H];

static uint32_t at(int x, int y) { return buf[y * W + x] & 0x00FFFFFFu; }

int main(void) {
    struct ui_surface s = ui_surface_make(buf, W, H);

    /* --- blend --- */
    ck_eq((int)ui_blend(0xAABBCC, 0x112233, 255), 0xAABBCC, "blend full");
    ck_eq((int)ui_blend(0xAABBCC, 0x112233, 0), 0x112233, "blend none");
    ck_eq((int)ui_blend(0xFFFFFF, 0x000000, 128), 0x808080, "blend half");
    ck_eq((int)ui_blend(0xFF0000, 0x0000FF, 128), 0x80007F, "blend channels");

    /* --- isqrt --- */
    ck_eq(ui_isqrt(0), 0, "isqrt 0");
    ck_eq(ui_isqrt(144), 12, "isqrt 144");
    ck_eq(ui_isqrt(145), 12, "isqrt 145");
    ck_eq(ui_isqrt(1), 1, "isqrt 1");

    /* --- arc coverage --- */
    ck_eq(ui_arc_coverage(50, 50, 40, 40, 30), 255, "arc inside");
    ck_eq(ui_arc_coverage(90, 90, 40, 40, 10), 0, "arc outside");
    {
        /* 45 deg point of a r=20 circle at (40,40) is (54.1,54.1). */
        int edge = ui_arc_coverage(54, 54, 40, 40, 20);
        ck(edge > 0 && edge < 255, "arc edge partial");
    }

    /* --- rounded rect --- */
    memset(buf, 0, sizeof(buf));
    ui_fill_round(&s, ui_rect_make(10, 10, 60, 40), 8, 0x3366FF);
    ck_eq((int)at(40, 30), 0x3366FF, "round centre filled");
    ck_eq((int)at(10, 10), 0, "round corner cut");
    ck_eq((int)at(69, 49), 0, "round far corner cut");
    ck_eq((int)at(40, 10), 0x3366FF, "round top edge filled");
    ck_eq((int)at(10, 30), 0x3366FF, "round left edge filled");
    ck_eq((int)at(9, 30), 0, "round outside left");
    ck_eq((int)at(70, 30), 0, "round outside right");
    {
        int partial = 0;
        for (int y = 10; y < 18; y++)
            for (int x = 10; x < 18; x++)
                if (at(x, y) && at(x, y) != 0x3366FF) partial++;
        ck(partial > 0, "round corner antialiased");
    }

    /* radius clamped to half the smaller side, never inverted */
    memset(buf, 0, sizeof(buf));
    ui_fill_round(&s, ui_rect_make(10, 10, 20, 20), 100, 0xFFFFFF);
    ck_eq((int)at(20, 20), 0xFFFFFF, "huge radius still fills centre");
    ck_eq((int)at(10, 10), 0, "huge radius rounds to circle");

    /* --- clip --- */
    memset(buf, 0, sizeof(buf));
    {
        struct ui_rect saved = ui_clip_push(&s, ui_rect_make(0, 0, 30, 120));
        ui_fill(&s, ui_rect_make(0, 0, 120, 10), 0xFF0000);
        ui_clip_pop(&s, saved);
    }
    ck_eq((int)at(29, 5), 0xFF0000, "clip inside drawn");
    ck_eq((int)at(30, 5), 0, "clip outside skipped");

    /* --- stroke --- */
    memset(buf, 0, sizeof(buf));
    ui_stroke_round(&s, ui_rect_make(10, 10, 60, 40), 8, 1, 0xFFFFFF, 255);
    ck_eq((int)at(40, 10), 0xFFFFFF, "stroke top edge");
    ck_eq((int)at(40, 49), 0xFFFFFF, "stroke bottom edge");
    ck_eq((int)at(40, 30), 0, "stroke hollow centre");

    /* --- gradient --- */
    memset(buf, 0, sizeof(buf));
    ui_gradient_v(&s, ui_rect_make(0, 0, 10, 100), 0x000000, 0xFFFFFF);
    ck_eq((int)at(5, 0), 0x000000, "gradient top");
    ck_eq((int)at(5, 99), 0xFFFFFF, "gradient bottom");
    ck(at(5, 50) > 0x707070 && at(5, 50) < 0x909090, "gradient midpoint");

    /* --- shadow darkens outside, leaves interior alone --- */
    memset(buf, 0xFF, sizeof(buf));
    ui_shadow(&s, ui_rect_make(40, 40, 40, 40), 8, 10, 200, 2);
    ck(at(60, 60) == 0xFFFFFF, "shadow skips interior");
    ck(at(60, 84) < 0xFFFFFF, "shadow darkens below");
    ck(at(60, 20) == 0xFFFFFF, "shadow does not reach far above");

    /* --- acrylic blurs toward the mean and honours the tint --- */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            buf[y * W + x] = (x / 4) & 1 ? 0xFFFFFF : 0x000000;
    ui_acrylic(&s, ui_rect_make(20, 20, 60, 40), 0, 0x000000, 0);
    {
        int lo = 0xFFFFFF, hi = 0;
        for (int x = 30; x < 70; x++) {
            int v = (int)(at(x, 40) & 0xFF);
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        ck(hi - lo < 90, "acrylic flattens the stripe contrast");
    }

    /* --- text --- */
    ck(ui_font_height(UI_FONT_BODY) > 0, "body font has height");
    ck(ui_font_height(UI_FONT_CAPTION) < ui_font_height(UI_FONT_TITLE),
       "caption smaller than title");
    ck(ui_text_width("iii", UI_FONT_BODY) ==
       3 * ui_font_advance(UI_FONT_BODY), "monospace advance");
    ck(ui_text_width("", UI_FONT_BODY) == 0, "empty width");

    memset(buf, 0, sizeof(buf));
    ui_text(&s, 4, 4, "H", UI_FONT_SUBTITLE, 0xFFFFFF);
    {
        int ink = 0;
        for (int y = 0; y < 40; y++)
            for (int x = 0; x < 30; x++)
                if (at(x, y)) ink++;
        ck(ink > 10, "glyph produced ink");
    }
    /* Ink must start at the requested y, not PLT_FONT_Y_SHIFT below it. */
    {
        int top = -1;
        for (int y = 0; y < 60 && top < 0; y++)
            for (int x = 0; x < 30; x++)
                if (at(x, y)) { top = y; break; }
        int slack = ui_font_height(UI_FONT_SUBTITLE) / 4;
        printf("  (ink top %d, cap-height slack %d)\n", top, slack);
        ck(top >= 4 && top <= 4 + slack, "ink starts at the requested y");
    }

    {
        char out[64];
        ui_text_ellipsize(out, sizeof(out), "abcdefghijklmnop", 40,
                          UI_FONT_BODY);
        ck(ui_text_width(out, UI_FONT_BODY) <= 40, "ellipsized fits");
        ck(strstr(out, "...") != NULL, "ellipsis appended");
        ui_text_ellipsize(out, sizeof(out), "ab", 400, UI_FONT_BODY);
        ck(strcmp(out, "ab") == 0, "short text untouched");
        ui_text_ellipsize(out, sizeof(out), "", 400, UI_FONT_BODY);
        ck(out[0] == 0, "empty text safe");
    }

    /* --- icons draw something inside their box and nothing outside --- */
    for (int i = 0; i < UI_ICON_COUNT; i++) {
        int ink = 0, spill = 0;
        memset(buf, 0, sizeof(buf));
        ui_icon(&s, i, 20, 20, 32, 0xFFFFFF, 255);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                if (!at(x, y)) continue;
                if (x >= 18 && x < 74 && y >= 18 && y < 74) ink++;
                else spill++;
            }
        if (ink <= 8) { printf("FAIL: icon %d drew nothing (%d)\n", i, ink); failures++; }
        if (spill) { printf("FAIL: icon %d spilled %d px\n", i, spill); failures++; }
    }

    /* --- appui scrollbar geometry ---
     * Shared by the painter and by drag hit-testing in every app, so an
     * off-by-one here desynchronises the thumb from the pointer. */
    {
        struct appui_rect track = appui_rect_make(100, 10, 12, 200);
        struct appui_rect th;

        /* Content that fits: no thumb math, the track is returned whole. */
        th = appui_scroll_thumb(track, 1, 150, 200, 0);
        ck(th.h == track.h, "scroll: no thumb when content fits");

        /* At offset 0 the thumb is flush with the top... */
        th = appui_scroll_thumb(track, 1, 800, 200, 0);
        ck_eq(th.y, track.y, "scroll: thumb at top for offset 0");
        ck(th.h >= APPUI_SCROLL_MIN_THUMB && th.h < track.h,
           "scroll: thumb proportional and clamped");

        /* ...and at max scroll flush with the bottom, never past it. */
        th = appui_scroll_thumb(track, 1, 800, 200, 600);
        ck_eq(th.y + th.h, track.y + track.h, "scroll: thumb flush at bottom");

        /* Out-of-range offsets clamp rather than escaping the track. */
        th = appui_scroll_thumb(track, 1, 800, 200, 99999);
        ck_eq(th.y + th.h, track.y + track.h, "scroll: overscroll clamps");
        th = appui_scroll_thumb(track, 1, 800, 200, -50);
        ck_eq(th.y, track.y, "scroll: negative offset clamps");

        /* A tiny viewport must still give a grabbable thumb. */
        th = appui_scroll_thumb(track, 1, 100000, 200, 0);
        ck_eq(th.h, APPUI_SCROLL_MIN_THUMB, "scroll: min thumb size honoured");

        /* Round trip: an offset maps to a thumb whose centre maps back. */
        {
            int want = 300;
            struct appui_rect t2 = appui_scroll_thumb(track, 1, 800, 200,
                                                      want);
            int got = appui_scroll_offset_at(track, 1, 800, 200,
                                             t2.y + t2.h / 2);
            ck(got >= want - 4 && got <= want + 4,
               "scroll: offset round-trips through the thumb");
        }

        /* Horizontal uses the same code path on the other axis. */
        {
            struct appui_rect ht = appui_rect_make(10, 100, 200, 12);
            struct appui_rect h1 = appui_scroll_thumb(ht, 0, 800, 200, 0);
            struct appui_rect h2 = appui_scroll_thumb(ht, 0, 800, 200, 600);
            ck_eq(h1.x, ht.x, "scroll: h thumb at left for offset 0");
            ck_eq(h2.x + h2.w, ht.x + ht.w, "scroll: h thumb flush at right");
            ck_eq(h1.h, ht.h, "scroll: h thumb spans track height");
        }
    }

    /* --- appui widgets draw inside their bounds --- */
    {
        struct appui_rect r = appui_rect_make(20, 20, 90, 30);
        const char *names[] = {"button", "field", "checkbox", "progress",
                               "list row", "tab"};
        for (int k = 0; k < 6; k++) {
            int spill = 0;
            memset(buf, 0, sizeof(buf));
            switch (k) {
            case 0: appui_button_ex(buf, W, H, r, "Ok", APPUI_BTN_PRIMARY,
                                    APPUI_STATE_HOVERED); break;
            case 1: appui_field_frame(buf, W, H, r, 1); break;
            case 2: appui_checkbox(buf, W, H, r, 1, 0); break;
            case 3: appui_progress(buf, W, H, r, 3, 10); break;
            case 4: appui_list_row(buf, W, H, r, "Item", UI_ICON_FOLDER,
                                   APPUI_STATE_SELECTED); break;
            case 5: appui_tab(buf, W, H, r, "Tab", 1, 0); break;
            }
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                    if (at(x, y) && !(x >= r.x && x < r.x + r.w &&
                                      y >= r.y && y < r.y + r.h))
                        spill++;
            if (spill) {
                printf("FAIL: %s spilled %d px\n", names[k], spill);
                failures++;
            }
        }
        /* Progress must actually track its value. */
        {
            int lit_lo = 0, lit_hi = 0;
            memset(buf, 0, sizeof(buf));
            appui_progress(buf, W, H, r, 1, 10);
            for (int x = r.x; x < r.x + r.w; x++)
                if ((at(x, r.y + r.h / 2) & 0xFF) > 0x80) lit_lo++;
            memset(buf, 0, sizeof(buf));
            appui_progress(buf, W, H, r, 9, 10);
            for (int x = r.x; x < r.x + r.w; x++)
                if ((at(x, r.y + r.h / 2) & 0xFF) > 0x80) lit_hi++;
            ck(lit_hi > lit_lo * 3, "progress: fill tracks value");
        }
    }

    printf(failures ? "\n%d FAILURES\n" : "\nall uikit checks passed\n", failures);
    return failures ? 1 : 0;
}
