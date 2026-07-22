#ifndef BUZZOS_FB_H
#define BUZZOS_FB_H

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
void fb_set_color(uint8_t fg, uint8_t bg);
void fb_get_info(struct gfx_info *out);

int  fb_clear(uint8_t color);
int  fb_putpixel(int x, int y, uint8_t color);
int  fb_fill_rect(int x, int y, int w, int h, uint8_t color);
int  fb_blit8(int x, int y, int w, int h, const uint8_t *pixels);
int  fb_blit8_stride(int x, int y, int w, int h,
                     const uint8_t *pixels, int stride);
int  fb_text(int x, int y, const char *s, uint8_t fg, int bg);

void fb_console_clear(void);
void fb_console_putc(char c);
void fb_console_puts(const char *s);
void fb_console_backspace(void);

#endif /* BUZZOS_FB_H */
