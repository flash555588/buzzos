#ifndef BUZZOS_SURFACE_H
#define BUZZOS_SURFACE_H

#include <stdint.h>

struct gfx_surface {
    uint32_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
};

void surface_init(struct gfx_surface *surface, uint32_t *pixels,
                  uint32_t width, uint32_t height, uint32_t stride);
void surface_clear(struct gfx_surface *surface, uint32_t rgb);
void surface_putpixel(struct gfx_surface *surface, int x, int y, uint32_t rgb);
void surface_fill_rect(struct gfx_surface *surface, int x, int y,
                       int width, int height, uint32_t rgb);
void surface_scroll_up(struct gfx_surface *surface, uint32_t rows,
                       uint32_t fill_rgb);

#endif /* BUZZOS_SURFACE_H */
