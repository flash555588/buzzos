#include <stddef.h>
#include "fb.h"
#include "font_builtin.h"
#include "font_unicode.h"
#include "io.h"
#include "irq.h"
#include "paging.h"
#include "pmm.h"
#include "task.h"
#include "user_bounds.h"
#include "virtio_gpu.h"

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
static int scanout_mapped_pid;
static uint32_t scanout_mapped_bytes;

static int fb_gpu_active(void) {
    return fb_ready && fb_info.backend == GFX_BACKEND_VIRTIO_GPU_2D &&
           virtio_gpu_ready();
}

static volatile uint32_t *fb_row32(int y) {
    if (fb_gpu_active())
        return (volatile uint32_t *)virtio_gpu_pixels() +
               (uint32_t)y * virtio_gpu_stride();
    return (volatile uint32_t *)(fb_mem + (uint32_t)y * fb_info.pitch);
}

/* Coalesce GPU uploads: software drawing writes the guest backing store, then
 * a single TRANSFER+FLUSH covers the union of dirty rectangles.  Flushing on
 * every putpixel was pathologically slow and held the virtqueue too often. */
static struct {
    int valid;
    int x0;
    int y0;
    int x1;
    int y1;
} fb_gpu_damage;

static void fb_damage_add(int x, int y, int width, int height) {
    if (!fb_gpu_active() || width <= 0 || height <= 0)
        return;
    if (x < 0) {
        width += x;
        x = 0;
    }
    if (y < 0) {
        height += y;
        y = 0;
    }
    if (x >= (int)fb_info.width || y >= (int)fb_info.height ||
        width <= 0 || height <= 0)
        return;
    if (width > (int)fb_info.width - x)
        width = (int)fb_info.width - x;
    if (height > (int)fb_info.height - y)
        height = (int)fb_info.height - y;
    int x1 = x + width;
    int y1 = y + height;
    if (!fb_gpu_damage.valid) {
        fb_gpu_damage.x0 = x;
        fb_gpu_damage.y0 = y;
        fb_gpu_damage.x1 = x1;
        fb_gpu_damage.y1 = y1;
        fb_gpu_damage.valid = 1;
        return;
    }
    if (x < fb_gpu_damage.x0)
        fb_gpu_damage.x0 = x;
    if (y < fb_gpu_damage.y0)
        fb_gpu_damage.y0 = y;
    if (x1 > fb_gpu_damage.x1)
        fb_gpu_damage.x1 = x1;
    if (y1 > fb_gpu_damage.y1)
        fb_gpu_damage.y1 = y1;
}

static int fb_flush_damage(void) {
    if (!fb_gpu_active() || !fb_gpu_damage.valid)
        return 0;
    int x = fb_gpu_damage.x0;
    int y = fb_gpu_damage.y0;
    int width = fb_gpu_damage.x1 - fb_gpu_damage.x0;
    int height = fb_gpu_damage.y1 - fb_gpu_damage.y0;
    fb_gpu_damage.valid = 0;
    return virtio_gpu_flush(x, y, width, height);
}

static int fb_flush_rect(int x, int y, int width, int height) {
    if (!fb_gpu_active())
        return 0;
    fb_damage_add(x, y, width, height);
    return fb_flush_damage();
}

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

static void fb_store_rgb(int x, int y, uint32_t rgb) {
    if (!fb_ready || x < 0 || y < 0 ||
        x >= (int)fb_info.width || y >= (int)fb_info.height)
        return;
    rgb &= 0x00FFFFFFu;
    volatile uint8_t *p = fb_mem + (uint32_t)y * fb_info.pitch;
    if (fb_info.bpp == 32) {
        if (fb_gpu_active())
            virtio_gpu_pixels()[(uint32_t)y * virtio_gpu_stride() +
                                (uint32_t)x] = rgb;
        else
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
    }
    /* 8 bpp and other formats are not supported. */
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

static void draw_glyph(int x, int y, char c, uint32_t fg_rgb, int bg_rgb) {
    const uint8_t *alpha = font_alpha_for(c);
    uint32_t bg = bg_rgb >= 0 ? (uint32_t)bg_rgb : 0;
    for (int py = 0; py < KFONT_HEIGHT; py++) {
        for (int px = 0; px < KFONT_WIDTH; px++) {
            uint8_t a = alpha[py * KFONT_WIDTH + px];
            if (a == 0) {
                if (bg_rgb >= 0)
                    fb_store_rgb(x + px, y + py, bg);
                continue;
            }
            uint32_t rgb = (bg_rgb >= 0 && a < 255) ?
                           blend_rgb(fg_rgb, bg, a) : fg_rgb;
            fb_store_rgb(x + px, y + py, rgb);
        }
    }
}

static int draw_unicode_glyph(int x, int y, uint32_t cp, uint32_t fg_rgb,
                              int bg_rgb) {
    uint8_t bits[UFONT_BYTES];
    int width = font_unicode_lookup(cp, bits);
    if (width <= 0) {
        draw_glyph(x, y, '?', fg_rgb, bg_rgb);
        return FB_FONT_W;
    }
    uint32_t bg = bg_rgb >= 0 ? (uint32_t)bg_rgb : 0;
    for (int py = 0; py < UFONT_HEIGHT; py++) {
        for (int px = 0; px < width; px++) {
            int on = (bits[py * UFONT_STRIDE + px / 8] &
                      (uint8_t)(0x80u >> (px & 7))) != 0;
            if (on)
                fb_store_rgb(x + px, y + py, fg_rgb);
            else if (bg_rgb >= 0)
                fb_store_rgb(x + px, y + py, bg);
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
    /* Truecolor only — no 8 bpp indexed framebuffer path. */
    if (!(bpp == 16 || bpp == 24 || bpp == 32))
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
    fb_info.backend = GFX_BACKEND_FRAMEBUFFER;
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
        fb_info.pitch = 4;
        fb_info.bpp = 32;
        fb_info.backend = GFX_BACKEND_FRAMEBUFFER;
    }
    fb_gpu_damage.valid = 0;
    if (fb_ready && virtio_gpu_init(fb_info.width, fb_info.height) == 0) {
        fb_info.pitch = fb_info.width * 4u;
        fb_info.bpp = 32;
        fb_info.backend = GFX_BACKEND_VIRTIO_GPU_2D;
    }
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
        if (!fb_gpu_active())
            return -1;
    if (fb_info.width == width && fb_info.height == height &&
        fb_info.bpp == 32)
        return 0;

    if (fb_gpu_active()) {
        if (virtio_gpu_set_mode(width, height) < 0)
            return -1;
        fb_gpu_damage.valid = 0;
        fb_info.width = width;
        fb_info.height = height;
        fb_info.pitch = width * 4u;
        fb_info.bpp = 32;
        fb_info.backend = GFX_BACKEND_VIRTIO_GPU_2D;
        /* Caller (compositor) must re-map the scanout after mode change. */
        if (scanout_mapped_pid)
            (void)fb_unmap_scanout_user(scanout_mapped_pid);
        return 0;
    }

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
    fb_info.backend = GFX_BACKEND_FRAMEBUFFER;
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

int fb_clear(uint32_t rgb) {
    if (!fb_ready)
        return -1;
    return fb_fill_rect(0, 0, (int)fb_info.width, (int)fb_info.height, rgb);
}

int fb_putpixel(int x, int y, uint32_t rgb) {
    if (!fb_ready || x < 0 || y < 0 ||
        x >= (int)fb_info.width || y >= (int)fb_info.height)
        return -1;
    fb_store_rgb(x, y, rgb & 0x00FFFFFFu);
    /* Defer GPU upload: callers that batch pixels flush via fill/blit/present. */
    fb_damage_add(x, y, 1, 1);
    return 0;
}

int fb_fill_rect(int x, int y, int w, int h, uint32_t rgb) {
    if (!fb_ready)
        return -1;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb_info.width) w = (int)fb_info.width - x;
    if (y + h > (int)fb_info.height) h = (int)fb_info.height - y;
    if (w <= 0 || h <= 0)
        return 0;
    rgb &= 0x00FFFFFFu;
    if (fb_info.bpp == 32) {
        for (int yy = 0; yy < h; yy++) {
            volatile uint32_t *dst = fb_row32(y + yy) + x;
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
        return fb_flush_rect(x, y, w, h);
    }
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++)
            fb_store_rgb(x + xx, y + yy, rgb);
    return fb_flush_rect(x, y, w, h);
}

int fb_blit32_stride(int x, int y, int w, int h,
                     const uint32_t *pixels, int stride) {
    if (!fb_ready || !pixels || x < 0 || y < 0 || w <= 0 || h <= 0 ||
        stride < w)
        return -1;
    if (x + w > (int)fb_info.width || y + h > (int)fb_info.height)
        return -1;
    if (fb_info.bpp == 32) {
        for (int yy = 0; yy < h; yy++) {
            const uint32_t *src = pixels + yy * stride;
            volatile uint32_t *dst = fb_row32(y + yy) + x;
            int xx = 0;
            for (; xx + 4 <= w; xx += 4) {
                dst[xx] = src[xx] & 0x00FFFFFFu;
                dst[xx + 1] = src[xx + 1] & 0x00FFFFFFu;
                dst[xx + 2] = src[xx + 2] & 0x00FFFFFFu;
                dst[xx + 3] = src[xx + 3] & 0x00FFFFFFu;
            }
            for (; xx < w; xx++)
                dst[xx] = src[xx] & 0x00FFFFFFu;
        }
        return fb_flush_rect(x, y, w, h);
    }
    for (int yy = 0; yy < h; yy++) {
        const uint32_t *src = pixels + yy * stride;
        for (int xx = 0; xx < w; xx++)
            fb_store_rgb(x + xx, y + yy, src[xx] & 0x00FFFFFFu);
    }
    return fb_flush_rect(x, y, w, h);
}

int fb_blit32(int x, int y, int w, int h, const uint32_t *pixels) {
    return fb_blit32_stride(x, y, w, h, pixels, w);
}

int fb_text(int x, int y, const char *s, uint32_t fg_rgb, int bg_rgb) {
    if (!fb_ready || !s)
        return -1;
    int start_x = x;
    int min_x = x;
    int max_x = x;
    int min_y = y;
    int max_y = y + FB_FONT_H;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        if (cp == '\n') {
            x = start_x;
            y += FB_FONT_H;
            if (y + FB_FONT_H > max_y)
                max_y = y + FB_FONT_H;
            continue;
        }
        if (cp < 0x80u) {
            draw_glyph(x, y, (char)cp, fg_rgb, bg_rgb);
            x += FB_FONT_W;
        } else {
            x += draw_unicode_glyph(x, y, cp, fg_rgb, bg_rgb);
        }
        if (x > max_x) max_x = x;
        if (x < min_x) min_x = x;
        if (y < min_y) min_y = y;
        if (y + FB_FONT_H > max_y) max_y = y + FB_FONT_H;
    }
    return fb_flush_rect(min_x, min_y, max_x - min_x, max_y - min_y);
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
            volatile uint32_t *destination = fb_row32(y + row) + x;
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
        __sync_synchronize();
        return fb_flush_rect(x, y, width, height);
    }

    for (int row = 0; row < height; row++) {
        const uint32_t *source = pixels + (uint32_t)row * (uint32_t)stride;
        for (int col = 0; col < width; col++)
            fb_store_rgb(x + col, y + row, source[col]);
    }
    __sync_synchronize();
    return fb_flush_rect(x, y, width, height);
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

int fb_unmap_scanout_user(int pid) {
    if (pid <= 0 || scanout_mapped_pid != pid)
        return 0;
    uint32_t cr3 = paging_current_cr3();
    if (scanout_mapped_bytes)
        (void)paging_unmap_user_range(cr3, USER_DISPLAY_START, scanout_mapped_bytes);
    scanout_mapped_pid = 0;
    scanout_mapped_bytes = 0;
    return 0;
}

int fb_map_scanout_user(int pid, struct fb_scanout_map *out) {
    if (!out || !fb_ready || pid <= 0 ||
        pid != display_owner_pid || pid != task_get_pid())
        return -1;
    if (fb_info.bpp != 32)
        return -1;

    uintptr_t phys = 0;
    uint32_t bytes = 0;
    uint32_t stride_px = fb_info.width;
    uint32_t pixel_offset = 0;
    if (fb_gpu_active()) {
        phys = virtio_gpu_backing_phys();
        bytes = virtio_gpu_backing_bytes();
        stride_px = virtio_gpu_stride();
    } else {
        phys = paging_framebuffer_phys();
        pixel_offset = fb_page_offset;
        bytes = fb_info.pitch * fb_info.height + pixel_offset;
        if (fb_info.pitch >= 4u)
            stride_px = fb_info.pitch / 4u;
    }
    if (!phys || !bytes)
        return -1;
    bytes = (bytes + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    if (bytes > USER_DISPLAY_SIZE)
        return -1;

    /* Drop a previous map before installing the new one (mode change). */
    if (scanout_mapped_pid == pid)
        (void)fb_unmap_scanout_user(pid);

    uint32_t cr3 = paging_current_cr3();
    uint32_t pte = PAGE_PRESENT | PAGE_RW | PAGE_USER | PAGE_WT | PAGE_CD;
    if (paging_map_user_phys(cr3, USER_DISPLAY_START, phys, bytes, pte) < 0)
        return -1;

    scanout_mapped_pid = pid;
    scanout_mapped_bytes = bytes;
    out->user_va = USER_DISPLAY_START + pixel_offset;
    out->width = fb_info.width;
    out->height = fb_info.height;
    out->stride_pixels = stride_px;
    out->bytes = bytes;
    out->backend = fb_info.backend;
    return 0;
}

int fb_present_rect(int pid, int x, int y, int w, int h) {
    if (pid <= 0 || pid != display_owner_pid || !fb_ready)
        return -1;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= (int)fb_info.width || y >= (int)fb_info.height || w <= 0 || h <= 0)
        return 0;
    if (w > (int)fb_info.width - x)
        w = (int)fb_info.width - x;
    if (h > (int)fb_info.height - y)
        h = (int)fb_info.height - y;
    if (fb_gpu_active())
        return virtio_gpu_flush(x, y, w, h);
    /* Linear framebuffer: compositor wrote scanout memory directly. */
    return 0;
}

int fb_display_release(int pid) {
    if (pid <= 0)
        return -1;
    if (scanout_mapped_pid == pid)
        (void)fb_unmap_scanout_user(pid);
    uint32_t flags = irq_save();
    int result = -1;
    if (display_owner_pid == pid) {
        display_owner_pid = 0;
        result = 0;
    }
    irq_restore(flags);
    /* GPU resources are owned by whoever owns the display, and their user
     * mappings live in that process's address space.  Drop them here so a
     * compositor that exits (or crashes) cannot leak GPU memory or leave the
     * scanout pointing at a render target nobody is drawing into. */
    if (result == 0)
        virtio_gpu_3d_release();
    return result;
}

int fb_display_user_allowed(int pid) {
    return pid > 0 && display_owner_pid == pid;
}

int fb_display_console_active(void) {
    return display_owner_pid == 0;
}
