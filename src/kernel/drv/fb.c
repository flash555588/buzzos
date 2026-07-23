#include <stddef.h>
#include "fb.h"
#include "font_builtin.h"
#include "font_unicode.h"
#include "paging.h"
#include "pmm.h"

enum {
    FB_FONT_W = KFONT_WIDTH,
    FB_FONT_H = KFONT_HEIGHT,
    FB_CONSOLE_FONT_W = KFONT_WIDTH,
    FB_CONSOLE_FONT_H = KFONT_HEIGHT,
    FB_CONSOLE_MAX_COLS = 220,
    FB_CONSOLE_MAX_ROWS = 120,
};

static volatile uint8_t *fb_mem = (uint8_t *)KERNEL_FB_VIRT;
static struct gfx_info fb_info;
static int fb_ready;

static uint16_t console_cols;
static uint16_t console_rows;
static uint16_t console_row;
static uint16_t console_col;
static uint8_t console_color = 0x0F;
static uint8_t ansi_state;
static int ansi_params[2];
static int ansi_param_index;
static int ansi_seen_digit;

static char console_chars[FB_CONSOLE_MAX_ROWS][FB_CONSOLE_MAX_COLS];
static uint8_t console_colors[FB_CONSOLE_MAX_ROWS][FB_CONSOLE_MAX_COLS];

/*
 * Reading a write-combining framebuffer is extremely slow on real hardware
 * and under VMware.  Keep a normal-RAM mirror for the boot console so that
 * scrolling never has to read pixels back from video memory.
 */
static uint8_t *console_shadow;
static uint32_t console_shadow_size;
static int console_shadow_capture;
static int console_write_depth;
static int console_scroll_pending;

static uint32_t palette_lut[256];
static int palette_lut_ready;

static uint32_t palette_rgb_compute(uint8_t index) {
    static const uint32_t base[25] = {
        0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
        0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
        0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
        0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
        0x182848, 0x285080, 0x3868B0, 0x5088D8,
        0x70B0F8, 0x207850, 0x48B870, 0x503820,
        0x6C2C28
    };
    if (index < sizeof(base) / sizeof(base[0]))
        return base[index];
    if (index < 40) {
        uint32_t v = (uint32_t)(32 + (index - 25) * 14);
        return (v << 16) | (v << 8) | v;
    }
    {
        uint8_t n = (uint8_t)(index - 40);
        uint32_t r = (uint32_t)(n / 36u);
        uint32_t g = (uint32_t)((n / 6u) % 6u);
        uint32_t b = (uint32_t)(n % 6u);
        r = r * 51u;
        g = g * 51u;
        b = b * 51u;
        return (r << 16) | (g << 8) | b;
    }
}

static void palette_init(void) {
    if (palette_lut_ready)
        return;
    for (uint32_t i = 0; i < 256u; i++)
        palette_lut[i] = palette_rgb_compute((uint8_t)i);
    palette_lut_ready = 1;
}

static uint32_t palette_rgb(uint8_t index) {
    palette_init();
    return palette_lut[index];
}

static void console_shadow_store_rgb(int x, int y, uint32_t rgb) {
    if (!console_shadow || !console_shadow_capture ||
        x < 0 || y < 0 ||
        x >= (int)fb_info.width || y >= (int)fb_info.height)
        return;
    uint8_t *p = console_shadow + (uint32_t)y * fb_info.pitch;
    if (fb_info.bpp == 32) {
        ((uint32_t *)p)[x] = rgb;
    } else if (fb_info.bpp == 24) {
        p += (uint32_t)x * 3u;
        p[0] = (uint8_t)(rgb & 0xFFu);
        p[1] = (uint8_t)((rgb >> 8) & 0xFFu);
        p[2] = (uint8_t)((rgb >> 16) & 0xFFu);
    } else if (fb_info.bpp == 16) {
        uint16_t r = (uint16_t)((rgb >> 19) & 0x1Fu);
        uint16_t g = (uint16_t)((rgb >> 10) & 0x3Fu);
        uint16_t b = (uint16_t)((rgb >> 3) & 0x1Fu);
        ((uint16_t *)p)[x] = (uint16_t)((r << 11) | (g << 5) | b);
    } else {
        p[x] = (uint8_t)(rgb & 0xFFu);
    }
}

static void fb_store_rgb(int x, int y, uint32_t rgb) {
    if (!fb_ready || x < 0 || y < 0 ||
        x >= (int)fb_info.width || y >= (int)fb_info.height)
        return;
    volatile uint8_t *p = fb_mem + (uint32_t)y * fb_info.pitch;
    if (fb_info.bpp == 32) {
        ((volatile uint32_t *)p)[x] = rgb;
    } else if (fb_info.bpp == 24) {
        p += (uint32_t)x * 3u;
        p[0] = (uint8_t)(rgb & 0xFFu);
        p[1] = (uint8_t)((rgb >> 8) & 0xFFu);
        p[2] = (uint8_t)((rgb >> 16) & 0xFFu);
    } else if (fb_info.bpp == 16) {
        uint16_t r = (uint16_t)((rgb >> 19) & 0x1Fu);
        uint16_t g = (uint16_t)((rgb >> 10) & 0x3Fu);
        uint16_t b = (uint16_t)((rgb >> 3) & 0x1Fu);
        ((volatile uint16_t *)p)[x] = (uint16_t)((r << 11) | (g << 5) | b);
    } else {
        p[x] = (uint8_t)(rgb & 0xFFu);
    }
    console_shadow_store_rgb(x, y, rgb);
}

static uint32_t blend_rgb(uint32_t fg, uint32_t bg, uint32_t alpha) {
    uint32_t inv = 255u - alpha;
    uint32_t rb = (((fg & 0x00FF00FFu) * alpha +
                    (bg & 0x00FF00FFu) * inv) >> 8) & 0x00FF00FFu;
    uint32_t g = (((fg & 0x0000FF00u) * alpha +
                   (bg & 0x0000FF00u) * inv) >> 8) & 0x0000FF00u;
    return rb | g;
}

static const uint8_t *font_alpha_for(char c) {
    unsigned char ch = (unsigned char)c;
    if (ch < KFONT_FIRST || ch >= KFONT_FIRST + KFONT_COUNT)
        ch = '?';
    return &kfont_alpha[ch - KFONT_FIRST][0][0];
}

static void draw_glyph(int x, int y, char c, uint8_t fg, int bg) {
    const uint8_t *alpha = font_alpha_for(c);
    uint32_t fg_rgb = palette_rgb(fg);
    uint32_t bg_rgb = bg >= 0 ? palette_rgb((uint8_t)bg) : 0;
    for (int py = 0; py < KFONT_HEIGHT; py++) {
        for (int px = 0; px < KFONT_WIDTH; px++) {
            uint8_t a = alpha[py * KFONT_WIDTH + px];
            if (a == 0) {
                if (bg >= 0)
                    fb_store_rgb(x + px, y + py, bg_rgb);
                continue;
            }
            uint32_t rgb = (bg >= 0 && a < 255) ?
                           blend_rgb(fg_rgb, bg_rgb, a) : fg_rgb;
            fb_store_rgb(x + px, y + py, rgb);
        }
    }
}

static int draw_unicode_glyph(int x, int y, uint32_t cp, uint8_t fg, int bg) {
    uint8_t bits[UFONT_BYTES];
    int width = font_unicode_lookup(cp, bits);
    if (width <= 0) {
        draw_glyph(x, y, '?', fg, bg);
        return FB_FONT_W;
    }
    uint32_t fg_rgb = palette_rgb(fg);
    uint32_t bg_rgb = bg >= 0 ? palette_rgb((uint8_t)bg) : 0;
    for (int py = 0; py < UFONT_HEIGHT; py++) {
        for (int px = 0; px < width; px++) {
            int on = (bits[py * UFONT_STRIDE + px / 8] &
                      (uint8_t)(0x80u >> (px & 7))) != 0;
            if (on)
                fb_store_rgb(x + px, y + py, fg_rgb);
            else if (bg >= 0)
                fb_store_rgb(x + px, y + py, bg_rgb);
        }
    }
    return width;
}

static uint32_t utf8_next(const char **text) {
    const uint8_t *s = (const uint8_t *)*text;
    uint32_t cp;
    int extra;
    if (s[0] < 0x80u) {
        *text = (const char *)(s + 1);
        return s[0];
    }
    if (s[0] >= 0xC2u && s[0] <= 0xDFu) {
        cp = s[0] & 0x1Fu; extra = 1;
    } else if (s[0] >= 0xE0u && s[0] <= 0xEFu) {
        cp = s[0] & 0x0Fu; extra = 2;
    } else if (s[0] >= 0xF0u && s[0] <= 0xF4u) {
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

static void draw_cell(uint16_t r, uint16_t c) {
    if (r >= console_rows || c >= console_cols)
        return;
    int x = c * FB_CONSOLE_FONT_W;
    int y = r * FB_CONSOLE_FONT_H;
    console_shadow_capture++;
    fb_fill_rect(x, y, FB_CONSOLE_FONT_W, FB_CONSOLE_FONT_H,
                 console_colors[r][c] >> 4);
    draw_glyph(x, y, console_chars[r][c],
               console_colors[r][c] & 0x0F,
               console_colors[r][c] >> 4);
    console_shadow_capture--;
}

static void console_sanitize(void) {
    if (console_cols == 0 || console_cols > FB_CONSOLE_MAX_COLS)
        console_cols = 1;
    if (console_rows == 0 || console_rows > FB_CONSOLE_MAX_ROWS)
        console_rows = 1;
    if (console_col >= console_cols)
        console_col = console_cols - 1;
    if (console_row >= console_rows)
        console_row = console_rows - 1;
}

static void console_shadow_init(void) {
    if (!fb_ready || console_shadow || !fb_info.pitch || !fb_info.height)
        return;
    uint64_t bytes = (uint64_t)fb_info.pitch * fb_info.height;
    if (bytes == 0 || bytes > KERNEL_FB_SIZE)
        return;
    size_t pages = ((size_t)bytes + PAGE_SIZE - 1u) / PAGE_SIZE;
    uintptr_t address = pmm_alloc_pages(pages);
    if (!address)
        return;
    console_shadow = (uint8_t *)address;
    console_shadow_size = (uint32_t)bytes;
    for (size_t i = 0; i < pages * PAGE_SIZE; i++)
        console_shadow[i] = 0;
}

static void console_shadow_flush(void) {
    if (!fb_ready || !console_shadow || !console_shadow_size)
        return;
    uint32_t words = console_shadow_size / sizeof(uint32_t);
    volatile uint32_t *dst32 = (volatile uint32_t *)fb_mem;
    const uint32_t *src32 = (const uint32_t *)console_shadow;
    uint32_t i = 0;
    for (; i + 4u <= words; i += 4u) {
        dst32[i] = src32[i];
        dst32[i + 1u] = src32[i + 1u];
        dst32[i + 2u] = src32[i + 2u];
        dst32[i + 3u] = src32[i + 3u];
    }
    for (; i < words; i++)
        dst32[i] = src32[i];
    for (uint32_t byte = words * sizeof(uint32_t);
         byte < console_shadow_size; byte++)
        fb_mem[byte] = console_shadow[byte];
    __asm__ volatile("sfence" ::: "memory");
}

static void console_shadow_fill_rows(uint32_t first_y, uint32_t rows,
                                     uint32_t rgb) {
    if (!console_shadow || first_y >= fb_info.height)
        return;
    if (rows > fb_info.height - first_y)
        rows = fb_info.height - first_y;
    int saved_capture = console_shadow_capture;
    console_shadow_capture = 1;
    for (uint32_t y = first_y; y < first_y + rows; y++)
        for (uint32_t x = 0; x < fb_info.width; x++)
            console_shadow_store_rgb((int)x, (int)y, rgb);
    console_shadow_capture = saved_capture;
}

static void fb_scroll_pixels_up(uint32_t pixels) {
    if (!fb_ready || pixels == 0 || pixels >= fb_info.height)
        return;
    uint32_t src_y = pixels;
    uint32_t copy_rows = fb_info.height - pixels;
    uint32_t bytes = fb_info.pitch;
    if (console_shadow) {
        uint32_t src_offset = src_y * bytes;
        uint32_t copy_bytes = copy_rows * bytes;
        uint32_t words = copy_bytes / sizeof(uint32_t);
        uint32_t *dst32 = (uint32_t *)console_shadow;
        const uint32_t *src32 =
            (const uint32_t *)(console_shadow + src_offset);
        uint32_t i = 0;
        for (; i < words; i++)
            dst32[i] = src32[i];
        for (uint32_t byte = words * sizeof(uint32_t);
             byte < copy_bytes; byte++)
            console_shadow[byte] = console_shadow[src_offset + byte];
        console_shadow_fill_rows(copy_rows, pixels,
                                 palette_rgb(console_color >> 4));
        if (console_write_depth > 0) {
            console_scroll_pending = 1;
        } else {
            console_shadow_flush();
        }
        return;
    }

    /* Allocation failure fallback.  This is correct but slow on WC memory. */
    for (uint32_t y = 0; y < copy_rows; y++) {
        volatile uint8_t *dst = fb_mem + y * fb_info.pitch;
        volatile uint8_t *src = fb_mem + (src_y + y) * fb_info.pitch;
        for (uint32_t i = 0; i < bytes; i++)
            dst[i] = src[i];
    }
    fb_fill_rect(0, (int)copy_rows, (int)fb_info.width, (int)pixels,
                 console_color >> 4);
}

static void ansi_reset(void) {
    ansi_state = 0;
    ansi_params[0] = 0;
    ansi_params[1] = 0;
    ansi_param_index = 0;
    ansi_seen_digit = 0;
}

static int ansi_value_or_default(int value, int fallback) {
    return value > 0 ? value : fallback;
}

static void console_set_cursor(int r, int c) {
    console_sanitize();
    if (r < 0) r = 0;
    if (c < 0) c = 0;
    if (r >= console_rows) r = console_rows - 1;
    if (c >= console_cols) c = console_cols - 1;
    console_row = (uint16_t)r;
    console_col = (uint16_t)c;
}

static void console_clear_line_from_cursor(void) {
    console_sanitize();
    for (uint16_t c = console_col; c < console_cols; c++) {
        console_chars[console_row][c] = ' ';
        console_colors[console_row][c] = console_color;
        draw_cell(console_row, c);
    }
}

static int handle_ansi(char c) {
    if (ansi_state == 0) {
        if ((unsigned char)c == 0x1B) {
            ansi_state = 1;
            return 1;
        }
        return 0;
    }
    if (ansi_state == 1) {
        if (c == '[') {
            ansi_state = 2;
            ansi_params[0] = 0;
            ansi_params[1] = 0;
            ansi_param_index = 0;
            ansi_seen_digit = 0;
        } else {
            ansi_reset();
        }
        return 1;
    }
    if (ansi_state == 2) {
        if (c >= '0' && c <= '9') {
            if (ansi_param_index < 2)
                ansi_params[ansi_param_index] =
                    ansi_params[ansi_param_index] * 10 + (c - '0');
            ansi_seen_digit = 1;
            return 1;
        }
        if (c == ';') {
            if (ansi_param_index < 1)
                ansi_param_index++;
            ansi_seen_digit = 0;
            return 1;
        }
        int n = ansi_value_or_default(ansi_params[0], 1);
        switch (c) {
        case 'A': console_set_cursor((int)console_row - n, console_col); break;
        case 'B': console_set_cursor((int)console_row + n, console_col); break;
        case 'C': console_set_cursor(console_row, (int)console_col + n); break;
        case 'D': console_set_cursor(console_row, (int)console_col - n); break;
        case 'H':
        case 'f':
            console_set_cursor(ansi_value_or_default(ansi_params[0], 1) - 1,
                               ansi_value_or_default(ansi_params[1], 1) - 1);
            break;
        case 'J':
            if (ansi_params[0] == 2 || !ansi_seen_digit)
                fb_console_clear();
            break;
        case 'K':
            console_clear_line_from_cursor();
            break;
        default:
            break;
        }
    }
    ansi_reset();
    return 1;
}

static void console_newline(void) {
    console_sanitize();
    console_col = 0;
    if (++console_row < console_rows)
        return;
    if (console_rows == 0) {
        console_row = 0;
        return;
    }
    for (uint16_t r = 1; r < console_rows; r++) {
        for (uint16_t c = 0; c < console_cols; c++) {
            console_chars[r - 1][c] = console_chars[r][c];
            console_colors[r - 1][c] = console_colors[r][c];
        }
    }
    console_row = console_rows - 1;
    for (uint16_t c = 0; c < console_cols; c++) {
        console_chars[console_row][c] = ' ';
        console_colors[console_row][c] = console_color;
    }
    fb_scroll_pixels_up(FB_CONSOLE_FONT_H);
}

void fb_set_framebuffer(uint64_t phys_addr, uint32_t width, uint32_t height,
                        uint32_t pitch, uint32_t bpp) {
    if (phys_addr > 0xFFFFFFFFull || !width || !height || !pitch)
        return;
    if (!(bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32))
        return;
    if (pitch * height > KERNEL_FB_SIZE)
        return;
    fb_mem = (uint8_t *)(KERNEL_FB_VIRT + ((uint32_t)phys_addr & 0xFFFu));
    fb_info.width = width;
    fb_info.height = height;
    fb_info.pitch = pitch;
    fb_info.bpp = bpp;
    fb_ready = 1;
}

void fb_init(void) {
    if (!fb_ready) {
        fb_info.width = 1;
        fb_info.height = 1;
        fb_info.pitch = 1;
        fb_info.bpp = 8;
    }
    console_cols = (uint16_t)(fb_info.width / FB_CONSOLE_FONT_W);
    console_rows = (uint16_t)(fb_info.height / FB_CONSOLE_FONT_H);
    if (console_cols > FB_CONSOLE_MAX_COLS)
        console_cols = FB_CONSOLE_MAX_COLS;
    if (console_rows > FB_CONSOLE_MAX_ROWS)
        console_rows = FB_CONSOLE_MAX_ROWS;
    if (console_cols == 0)
        console_cols = 1;
    if (console_rows == 0)
        console_rows = 1;
    ansi_reset();
    console_shadow_init();
    fb_console_clear();
}

void fb_set_color(uint8_t fg, uint8_t bg) {
    console_color = (uint8_t)((bg << 4) | (fg & 0x0F));
}

void fb_get_info(struct gfx_info *out) {
    if (out)
        *out = fb_info;
}

int fb_clear(uint8_t color) {
    if (!fb_ready)
        return -1;
    return fb_fill_rect(0, 0, (int)fb_info.width, (int)fb_info.height, color);
}

int fb_putpixel(int x, int y, uint8_t color) {
    if (!fb_ready || x < 0 || y < 0 ||
        x >= (int)fb_info.width || y >= (int)fb_info.height)
        return -1;
    fb_store_rgb(x, y, palette_rgb(color));
    return 0;
}

int fb_fill_rect(int x, int y, int w, int h, uint8_t color) {
    if (!fb_ready)
        return -1;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb_info.width) w = (int)fb_info.width - x;
    if (y + h > (int)fb_info.height) h = (int)fb_info.height - y;
    if (w <= 0 || h <= 0)
        return 0;
    uint32_t rgb = palette_rgb(color);
    if (fb_info.bpp == 32) {
        for (int yy = 0; yy < h; yy++) {
            volatile uint32_t *dst = (volatile uint32_t *)(fb_mem +
                (uint32_t)(y + yy) * fb_info.pitch) + x;
            uint32_t *shadow = console_shadow && console_shadow_capture
                ? (uint32_t *)(console_shadow +
                    (uint32_t)(y + yy) * fb_info.pitch) + x
                : NULL;
            int xx = 0;
            for (; xx + 4 <= w; xx += 4) {
                dst[xx] = rgb;
                dst[xx + 1] = rgb;
                dst[xx + 2] = rgb;
                dst[xx + 3] = rgb;
                if (shadow) {
                    shadow[xx] = rgb;
                    shadow[xx + 1] = rgb;
                    shadow[xx + 2] = rgb;
                    shadow[xx + 3] = rgb;
                }
            }
            for (; xx < w; xx++) {
                dst[xx] = rgb;
                if (shadow)
                    shadow[xx] = rgb;
            }
        }
        return 0;
    }
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++)
            fb_store_rgb(x + xx, y + yy, rgb);
    return 0;
}

int fb_blit8_stride(int x, int y, int w, int h,
                    const uint8_t *pixels, int stride) {
    if (!fb_ready || !pixels || x < 0 || y < 0 || w <= 0 || h <= 0 ||
        stride < w)
        return -1;
    if (x + w > (int)fb_info.width || y + h > (int)fb_info.height)
        return -1;
    palette_init();
    if (fb_info.bpp == 32) {
        for (int yy = 0; yy < h; yy++) {
            const uint8_t *src = pixels + yy * stride;
            volatile uint32_t *dst = (volatile uint32_t *)(fb_mem +
                (uint32_t)(y + yy) * fb_info.pitch) + x;
            int xx = 0;
            for (; xx + 4 <= w; xx += 4) {
                dst[xx] = palette_lut[src[xx]];
                dst[xx + 1] = palette_lut[src[xx + 1]];
                dst[xx + 2] = palette_lut[src[xx + 2]];
                dst[xx + 3] = palette_lut[src[xx + 3]];
            }
            for (; xx < w; xx++)
                dst[xx] = palette_lut[src[xx]];
        }
        return 0;
    }
    for (int yy = 0; yy < h; yy++) {
        const uint8_t *src = pixels + yy * stride;
        for (int xx = 0; xx < w; xx++)
            fb_store_rgb(x + xx, y + yy, palette_rgb(src[xx]));
    }
    return 0;
}

int fb_blit8(int x, int y, int w, int h, const uint8_t *pixels) {
    return fb_blit8_stride(x, y, w, h, pixels, w);
}

int fb_text(int x, int y, const char *s, uint8_t fg, int bg) {
    if (!fb_ready || !s)
        return -1;
    int start_x = x;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        if (cp == '\n') {
            x = start_x;
            y += FB_FONT_H;
            continue;
        }
        if (cp < 0x80u) {
            draw_glyph(x, y, (char)cp, fg, bg);
            x += FB_FONT_W;
        } else {
            x += draw_unicode_glyph(x, y, cp, fg, bg);
        }
    }
    return 0;
}

void fb_console_clear(void) {
    console_sanitize();
    console_shadow_capture++;
    fb_clear(0);
    console_shadow_capture--;
    for (uint16_t r = 0; r < console_rows; r++) {
        for (uint16_t c = 0; c < console_cols; c++) {
            console_chars[r][c] = ' ';
            console_colors[r][c] = console_color;
        }
    }
    console_row = 0;
    console_col = 0;
}

void fb_console_putc(char c) {
    console_sanitize();
    if (handle_ansi(c))
        return;
    console_sanitize();
    if (c == '\n') {
        console_newline();
        return;
    }
    if (c == '\r') {
        console_col = 0;
        return;
    }
    if (c == '\b') {
        fb_console_backspace();
        return;
    }
    if (c < 32)
        return;
    console_sanitize();
    uint16_t row = console_row;
    uint16_t col = console_col;
    console_chars[row][col] = c;
    console_colors[row][col] = console_color;
    int x = (int)col * FB_CONSOLE_FONT_W;
    int y = (int)row * FB_CONSOLE_FONT_H;
    console_shadow_capture++;
    fb_fill_rect(x, y, FB_CONSOLE_FONT_W, FB_CONSOLE_FONT_H,
                 console_color >> 4);
    draw_glyph(x, y, c, console_color & 0x0F, console_color >> 4);
    console_shadow_capture--;
    if (++console_col >= console_cols)
        console_newline();
}

void fb_console_puts(const char *s) {
    size_t count = 0;
    while (s && s[count])
        count++;
    fb_console_write(s, count);
}

void fb_console_write(const char *s, size_t count) {
    if (!s || count == 0)
        return;
    console_write_depth++;
    for (size_t i = 0; i < count; i++)
        fb_console_putc(s[i]);
    console_write_depth--;
    if (console_write_depth == 0 && console_scroll_pending) {
        console_scroll_pending = 0;
        console_shadow_flush();
    }
}

void fb_console_backspace(void) {
    console_sanitize();
    if (console_col > 0) {
        console_col--;
    } else if (console_row > 0) {
        console_row--;
        console_col = console_cols - 1;
    } else {
        return;
    }
    console_chars[console_row][console_col] = ' ';
    console_colors[console_row][console_col] = console_color;
    draw_cell(console_row, console_col);
}
