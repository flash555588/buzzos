#include "surface.h"

void surface_init(struct gfx_surface *surface, uint32_t *pixels,
                  uint32_t width, uint32_t height, uint32_t stride) {
    if (!surface)
        return;
    surface->pixels = pixels;
    surface->width = width;
    surface->height = height;
    surface->stride = stride;
}

void surface_putpixel(struct gfx_surface *surface, int x, int y, uint32_t rgb) {
    if (!surface || !surface->pixels || x < 0 || y < 0 ||
        x >= (int)surface->width || y >= (int)surface->height)
        return;
    surface->pixels[(uint32_t)y * surface->stride + (uint32_t)x] = rgb;
}

void surface_fill_rect(struct gfx_surface *surface, int x, int y,
                       int width, int height, uint32_t rgb) {
    if (!surface || !surface->pixels)
        return;
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x >= (int)surface->width || y >= (int)surface->height)
        return;
    if (width > (int)surface->width - x)
        width = (int)surface->width - x;
    if (height > (int)surface->height - y)
        height = (int)surface->height - y;
    if (width <= 0 || height <= 0)
        return;

    for (int row = 0; row < height; row++) {
        uint32_t *dst = surface->pixels +
            (uint32_t)(y + row) * surface->stride + (uint32_t)x;
        int col = 0;
        for (; col + 4 <= width; col += 4) {
            dst[col] = rgb;
            dst[col + 1] = rgb;
            dst[col + 2] = rgb;
            dst[col + 3] = rgb;
        }
        for (; col < width; col++)
            dst[col] = rgb;
    }
}

void surface_clear(struct gfx_surface *surface, uint32_t rgb) {
    if (!surface)
        return;
    surface_fill_rect(surface, 0, 0, (int)surface->width,
                      (int)surface->height, rgb);
}

void surface_scroll_up(struct gfx_surface *surface, uint32_t rows,
                       uint32_t fill_rgb) {
    if (!surface || !surface->pixels || rows == 0)
        return;
    if (rows >= surface->height) {
        surface_clear(surface, fill_rgb);
        return;
    }

    uint32_t copy_rows = surface->height - rows;
    uint32_t copy_pixels = copy_rows * surface->stride;
    uint32_t source = rows * surface->stride;
    for (uint32_t i = 0; i < copy_pixels; i++)
        surface->pixels[i] = surface->pixels[source + i];

    surface_fill_rect(surface, 0, (int)copy_rows, (int)surface->width,
                      (int)rows, fill_rgb);
}
