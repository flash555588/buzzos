#ifndef BUZZOS_FB_H
#define BUZZOS_FB_H

#include <stddef.h>
#include <stdint.h>

struct gfx_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
};

void fb_set_framebuffer(uint64_t phys_addr, uint32_t width, uint32_t height,
                        uint32_t pitch, uint32_t bpp);
void fb_init(void);
void fb_get_info(struct gfx_info *out);
int  fb_set_mode(uint32_t width, uint32_t height);
int  fb_restore_boot_mode(void);
uint32_t fb_palette_rgb(uint8_t index);

int  fb_clear(uint8_t color);
int  fb_putpixel(int x, int y, uint8_t color);
int  fb_fill_rect(int x, int y, int w, int h, uint8_t color);
int  fb_blit8(int x, int y, int w, int h, const uint8_t *pixels);
int  fb_blit8_stride(int x, int y, int w, int h,
                     const uint8_t *pixels, int stride);
int  fb_text(int x, int y, const char *s, uint8_t fg, int bg);
int  fb_present_rgb32(int x, int y, int width, int height,
                      const uint32_t *pixels, int stride);

/* Exactly one user process may own the scanout. PID 0 denotes the console. */
int fb_display_acquire(int pid);
int fb_display_release(int pid);
int fb_display_user_allowed(int pid);
int fb_display_console_active(void);

#endif /* BUZZOS_FB_H */
