#include <stddef.h>
#include "fb.h"
#include "font_builtin.h"
#include "font_unicode.h"
#include "io.h"
#include "irq.h"
#include "paging.h"

enum {
    FB_FONT_W = KFONT_WIDTH,
    FB_FONT_H = KFONT_HEIGHT,
};

static volatile uint8_t *fb_mem = (uint8_t *)KERNEL_FB_VIRT;
static struct gfx_info fb_info;
static struct gfx_info fb_boot_info;
static uint32_t fb_page_offset;
static int fb_boot_info_valid;
static int fb_ready;
static int display_owner_pid;

enum {
    VBE_DISPI_INDEX_PORT = 0x01CE,
    VBE_DISPI_DATA_PORT = 0x01CF,
    VBE_DISPI_INDEX_ID = 0,
    VBE_DISPI_INDEX_XRES = 1,
    VBE_DISPI_INDEX_YRES = 2,
    VBE_DISPI_INDEX_BPP = 3,
    VBE_DISPI_INDEX_ENABLE = 4,
    VBE_DISPI_INDEX_VIRT_WIDTH = 6,
    VBE_DISPI_INDEX_X_OFFSET = 8,
    VBE_DISPI_INDEX_Y_OFFSET = 9,
    VBE_DISPI_ID0 = 0xB0C0,
    VBE_DISPI_ID5 = 0xB0C5,
    VBE_DISPI_ENABLED = 0x01,
    VBE_DISPI_LFB_ENABLED = 0x40,
};

static uint16_t bochs_vbe_read(uint16_t index) {
    outw(VBE_DISPI_INDEX_PORT, index);
    return inw(VBE_DISPI_DATA_PORT);
}

static void bochs_vbe_write(uint16_t index, uint16_t value) {
    outw(VBE_DISPI_INDEX_PORT, index);
    outw(VBE_DISPI_DATA_PORT, value);
}

static int bochs_vbe_available(void) {
    uint16_t id = bochs_vbe_read(VBE_DISPI_INDEX_ID);
    return id >= VBE_DISPI_ID0 && id <= VBE_DISPI_ID5;
}

static int bochs_vbe_program(uint32_t width, uint32_t height) {
    if (width > 0xFFFFu || height > 0xFFFFu)
        return -1;
    bochs_vbe_write(VBE_DISPI_INDEX_ENABLE, 0);
    bochs_vbe_write(VBE_DISPI_INDEX_XRES, (uint16_t)width);
    bochs_vbe_write(VBE_DISPI_INDEX_YRES, (uint16_t)height);
    bochs_vbe_write(VBE_DISPI_INDEX_BPP, 32);
    bochs_vbe_write(VBE_DISPI_INDEX_VIRT_WIDTH, (uint16_t)width);
    bochs_vbe_write(VBE_DISPI_INDEX_X_OFFSET, 0);
    bochs_vbe_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
    bochs_vbe_write(VBE_DISPI_INDEX_ENABLE,
                    VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);
    if (bochs_vbe_read(VBE_DISPI_INDEX_XRES) != width ||
        bochs_vbe_read(VBE_DISPI_INDEX_YRES) != height ||
        bochs_vbe_read(VBE_DISPI_INDEX_BPP) != 32)
        return -1;
    return 0;
}

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

uint32_t fb_palette_rgb(uint8_t index) {
    palette_init();
    return palette_lut[index];
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
    uint32_t fg_rgb = fb_palette_rgb(fg);
    uint32_t bg_rgb = bg >= 0 ? fb_palette_rgb((uint8_t)bg) : 0;
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
    uint32_t fg_rgb = fb_palette_rgb(fg);
    uint32_t bg_rgb = bg >= 0 ? fb_palette_rgb((uint8_t)bg) : 0;
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

void fb_set_framebuffer(uint64_t phys_addr, uint32_t width, uint32_t height,
                        uint32_t pitch, uint32_t bpp) {
    if (phys_addr > 0xFFFFFFFFull || !width || !height || !pitch)
        return;
    if (!(bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32))
        return;
    uint32_t page_offset = (uint32_t)phys_addr & 0xFFFu;
    if ((uint64_t)pitch * height + page_offset > KERNEL_FB_SIZE)
        return;
    fb_page_offset = page_offset;
    fb_mem = (uint8_t *)(KERNEL_FB_VIRT + fb_page_offset);
    fb_info.width = width;
    fb_info.height = height;
    fb_info.pitch = pitch;
    fb_info.bpp = bpp;
    if (!fb_boot_info_valid) {
        fb_boot_info = fb_info;
        fb_boot_info_valid = 1;
    }
    fb_ready = 1;
}

void fb_init(void) {
    if (!fb_ready) {
        fb_info.width = 1;
        fb_info.height = 1;
        fb_info.pitch = 1;
        fb_info.bpp = 8;
    }
    palette_init();
    display_owner_pid = 0;
}

void fb_get_info(struct gfx_info *out) {
    if (out)
        *out = fb_info;
}

int fb_set_mode(uint32_t width, uint32_t height) {
    /* Keep modes within the composed desktop surface (GUIAPP_MAX_*) and the
     * 16 MiB VGA aperture.  Aspect-ratio families (16:9 / 16:10 / 4:3 / 5:4)
     * all stay under these ceilings. */
    if (!fb_ready || width < 640u || height < 480u ||
        width > 1920u || height > 1200u)
        return -1;
    uint64_t bytes = (uint64_t)width * height * 4u;
    if (bytes + fb_page_offset > KERNEL_FB_SIZE || !bochs_vbe_available())
        return -1;
    if (fb_info.width == width && fb_info.height == height &&
        fb_info.bpp == 32)
        return 0;

    struct gfx_info previous = fb_info;
    uint32_t flags = irq_save();
    if (bochs_vbe_program(width, height) < 0) {
        (void)bochs_vbe_program(previous.width, previous.height);
        irq_restore(flags);
        return -1;
    }
    uint32_t virtual_width = bochs_vbe_read(VBE_DISPI_INDEX_VIRT_WIDTH);
    if (virtual_width < width)
        virtual_width = width;
    fb_info.width = width;
    fb_info.height = height;
    fb_info.pitch = virtual_width * 4u;
    fb_info.bpp = 32;
    irq_restore(flags);
    return 0;
}

int fb_restore_boot_mode(void) {
    if (!fb_boot_info_valid)
        return -1;
    if (fb_info.width == fb_boot_info.width &&
        fb_info.height == fb_boot_info.height)
        return 0;
    return fb_set_mode(fb_boot_info.width, fb_boot_info.height);
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
    fb_store_rgb(x, y, fb_palette_rgb(color));
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
    uint32_t rgb = fb_palette_rgb(color);
    if (fb_info.bpp == 32) {
        for (int yy = 0; yy < h; yy++) {
            volatile uint32_t *dst = (volatile uint32_t *)(fb_mem +
                (uint32_t)(y + yy) * fb_info.pitch) + x;
            int xx = 0;
            for (; xx + 4 <= w; xx += 4) {
                dst[xx] = rgb;
                dst[xx + 1] = rgb;
                dst[xx + 2] = rgb;
                dst[xx + 3] = rgb;
            }
            for (; xx < w; xx++)
                dst[xx] = rgb;
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
            fb_store_rgb(x + xx, y + yy, fb_palette_rgb(src[xx]));
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

int fb_present_rgb32(int x, int y, int width, int height,
                     const uint32_t *pixels, int stride) {
    if (!fb_ready || !pixels || x < 0 || y < 0 ||
        width <= 0 || height <= 0 || stride < width ||
        width > (int)fb_info.width || height > (int)fb_info.height ||
        x > (int)fb_info.width - width ||
        y > (int)fb_info.height - height)
        return -1;

    if (fb_info.bpp == 32) {
        for (int row = 0; row < height; row++) {
            const uint32_t *source = pixels + (uint32_t)row * (uint32_t)stride;
            volatile uint32_t *destination =
                (volatile uint32_t *)(fb_mem +
                    (uint32_t)(y + row) * fb_info.pitch) + x;
            int col = 0;
            for (; col + 4 <= width; col += 4) {
                destination[col] = source[col];
                destination[col + 1] = source[col + 1];
                destination[col + 2] = source[col + 2];
                destination[col + 3] = source[col + 3];
            }
            for (; col < width; col++)
                destination[col] = source[col];
        }
        __asm__ volatile("sfence" ::: "memory");
        return 0;
    }

    for (int row = 0; row < height; row++) {
        const uint32_t *source = pixels + (uint32_t)row * (uint32_t)stride;
        for (int col = 0; col < width; col++)
            fb_store_rgb(x + col, y + row, source[col]);
    }
    __asm__ volatile("sfence" ::: "memory");
    return 0;
}

int fb_display_acquire(int pid) {
    if (pid <= 0)
        return -1;
    uint32_t flags = irq_save();
    int result = -1;
    if (display_owner_pid == 0 || display_owner_pid == pid) {
        display_owner_pid = pid;
        result = 0;
    }
    irq_restore(flags);
    return result;
}

int fb_display_release(int pid) {
    if (pid <= 0)
        return -1;
    uint32_t flags = irq_save();
    int result = -1;
    if (display_owner_pid == pid) {
        display_owner_pid = 0;
        result = 0;
    }
    irq_restore(flags);
    return result;
}

int fb_display_user_allowed(int pid) {
    return pid > 0 && display_owner_pid == pid;
}

int fb_display_console_active(void) {
    return display_owner_pid == 0;
}
