#ifndef BUZZOS_FB_H
#define BUZZOS_FB_H

#include <stddef.h>
#include <stdint.h>

struct gfx_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t backend;
};

enum {
    GFX_BACKEND_FRAMEBUFFER = 0,
    GFX_BACKEND_VIRTIO_GPU_2D = 1,
};

void fb_set_framebuffer(uint64_t phys_addr, uint32_t width, uint32_t height,
                        uint32_t pitch, uint32_t bpp);
void fb_init(void);
void fb_get_info(struct gfx_info *out);
int  fb_set_mode(uint32_t width, uint32_t height);
int  fb_restore_boot_mode(void);
/* Color arguments are 0x00RRGGBB.  Scanout is truecolor (32 bpp preferred;
 * 16/24 bpp linear FB write paths remain for odd boot frames only). */
int  fb_clear(uint32_t rgb);
int  fb_putpixel(int x, int y, uint32_t rgb);
int  fb_fill_rect(int x, int y, int w, int h, uint32_t rgb);
int  fb_blit32(int x, int y, int w, int h, const uint32_t *pixels);
int  fb_blit32_stride(int x, int y, int w, int h,
                      const uint32_t *pixels, int stride);
int  fb_text(int x, int y, const char *s, uint32_t fg_rgb, int bg_rgb);
int  fb_present_rgb32(int x, int y, int width, int height,
                      const uint32_t *pixels, int stride);

/* Zero-copy scanout: map guest RGB surface into the display owner's VA and
 * present dirty rectangles (GPU: TRANSFER+FLUSH; linear FB: direct write). */
struct fb_scanout_map {
    uintptr_t user_va;
    uint32_t width;
    uint32_t height;
    uint32_t stride_pixels; /* pixels per row */
    uint32_t bytes;
    uint32_t backend;
};

int fb_map_scanout_user(int pid, struct fb_scanout_map *out);
int fb_unmap_scanout_user(int pid);
int fb_present_rect(int pid, int x, int y, int w, int h);

/* Exactly one user process may own the scanout. PID 0 denotes the console. */
int fb_display_acquire(int pid);
int fb_display_release(int pid);
int fb_display_user_allowed(int pid);
int fb_display_console_active(void);

#endif /* BUZZOS_FB_H */
