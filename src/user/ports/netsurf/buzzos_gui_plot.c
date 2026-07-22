#include "buzzos_gui_plot.h"
#include "appui.h"
#include "utils/errors.h"
#include "netsurf/layout.h"
#include "netsurf/plotters.h"

struct bitmap {
    void *ptr;
    size_t rowstride;
    int width;
    int height;
    bool opaque;
};

static uint8_t palette_colour(colour value) {
    unsigned red = value & 255u;
    unsigned green = (value >> 8) & 255u;
    unsigned blue = (value >> 16) & 255u;
    unsigned r = (red * 5u + 127u) / 255u;
    unsigned g = (green * 5u + 127u) / 255u;
    unsigned b = (blue * 5u + 127u) / 255u;
    return (uint8_t)(40u + r * 36u + g * 6u + b);
}

static int font_scale(const plot_font_style_t *style) {
    int pixels = (style->size + PLOT_STYLE_SCALE / 2) / PLOT_STYLE_SCALE;
    return pixels >= 24 ? 2 : 1;
}

static uint32_t bounded_codepoint(const char **cursor, const char *end) {
    const uint8_t *s = (const uint8_t *)*cursor;
    if ((const char *)s >= end) return 0;
    int bytes = 1;
    if (s[0] >= 0xc2 && s[0] <= 0xdf) bytes = 2;
    else if (s[0] >= 0xe0 && s[0] <= 0xef) bytes = 3;
    else if (s[0] >= 0xf0 && s[0] <= 0xf4) bytes = 4;
    if ((const char *)s + bytes > end) bytes = 1;
    for (int i = 1; i < bytes; i++)
        if ((s[i] & 0xc0u) != 0x80u) { bytes = 1; break; }
    uint32_t cp = bytes == 1 ? s[0] : (s[0] & (0x7fu >> bytes));
    for (int i = 1; i < bytes; i++) cp = (cp << 6) | (s[i] & 0x3fu);
    *cursor += bytes;
    return cp;
}

static int bounded_text_width(const plot_font_style_t *style,
                              const char *text, size_t length) {
    const char *cursor = text;
    const char *end = text + length;
    int width = 0;
    int scale = font_scale(style);
    while (cursor < end)
        width += appui_codepoint_width(bounded_codepoint(&cursor, end)) * scale;
    return width;
}

static nserror buzzos_font_width(const plot_font_style_t *style,
                                 const char *text, size_t length, int *width) {
    *width = bounded_text_width(style, text, length);
    return NSERROR_OK;
}

static nserror buzzos_font_position(const plot_font_style_t *style,
                                    const char *text, size_t length, int x,
                                    size_t *char_offset, int *actual_x) {
    const char *cursor = text;
    const char *end = text + length;
    int width = 0;
    int scale = font_scale(style);
    while (cursor < end) {
        const char *start = cursor;
        int advance = appui_codepoint_width(bounded_codepoint(&cursor, end)) * scale;
        if (width + advance / 2 >= x) { cursor = start; break; }
        width += advance;
    }
    *char_offset = (size_t)(cursor - text);
    *actual_x = width;
    return NSERROR_OK;
}

static nserror buzzos_font_split(const plot_font_style_t *style,
                                 const char *text, size_t length, int x,
                                 size_t *char_offset, int *actual_x) {
    const char *cursor = text;
    const char *end = text + length;
    size_t best_offset = 0;
    int best_width = 0;
    int width = 0;
    int scale = font_scale(style);
    while (cursor < end) {
        const char *start = cursor;
        uint32_t cp = bounded_codepoint(&cursor, end);
        int advance = appui_codepoint_width(cp) * scale;
        if (width + advance > x) break;
        width += advance;
        if (cp == ' ') { best_offset = (size_t)(cursor - text); best_width = width; }
        if (cursor == end) { best_offset = length; best_width = width; }
        (void)start;
    }
    if (best_offset == 0) {
        best_offset = (size_t)(cursor - text);
        best_width = width;
    }
    *char_offset = best_offset;
    *actual_x = best_width;
    return NSERROR_OK;
}

static struct gui_layout_table buzzos_layout = {
    .width = buzzos_font_width,
    .position = buzzos_font_position,
    .split = buzzos_font_split,
};

struct gui_layout_table *buzzos_layout_table = &buzzos_layout;

void buzzos_plot_target_init(struct buzzos_plot_target *target,
                             uint8_t *pixels, int width, int height,
                             int offset_y) {
    target->pixels = pixels;
    target->width = width;
    target->height = height;
    target->offset_y = offset_y;
    target->clip_x0 = 0;
    target->clip_y0 = 0;
    target->clip_x1 = width;
    target->clip_y1 = height - offset_y;
}

static void pixel(struct buzzos_plot_target *target, int x, int y, uint8_t colour_value) {
    if (x < target->clip_x0 || x >= target->clip_x1 ||
        y < target->clip_y0 || y >= target->clip_y1) return;
    y += target->offset_y;
    if (x >= 0 && x < target->width && y >= 0 && y < target->height)
        target->pixels[y * target->width + x] = colour_value;
}

static nserror plot_clip(const struct redraw_context *ctx, const struct rect *clip) {
    struct buzzos_plot_target *target = ctx->priv;
    target->clip_x0 = appui_max(0, clip->x0);
    target->clip_y0 = appui_max(0, clip->y0);
    target->clip_x1 = appui_min(target->width, clip->x1);
    target->clip_y1 = appui_min(target->height - target->offset_y, clip->y1);
    return NSERROR_OK;
}

static nserror plot_line(const struct redraw_context *ctx,
                         const plot_style_t *style, const struct rect *line) {
    struct buzzos_plot_target *target = ctx->priv;
    int x0 = line->x0, y0 = line->y0, x1 = line->x1, y1 = line->y1;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    uint8_t c = palette_colour(style->stroke_colour);
    for (;;) {
        pixel(target, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int twice = error * 2;
        if (twice >= dy) { error += dy; x0 += sx; }
        if (twice <= dx) { error += dx; y0 += sy; }
    }
    return NSERROR_OK;
}

static nserror plot_rectangle(const struct redraw_context *ctx,
                              const plot_style_t *style, const struct rect *rect) {
    struct buzzos_plot_target *target = ctx->priv;
    if (style->fill_type != PLOT_OP_TYPE_NONE) {
        uint8_t c = palette_colour(style->fill_colour);
        for (int y = rect->y0; y < rect->y1; y++)
            for (int x = rect->x0; x < rect->x1; x++) pixel(target, x, y, c);
    }
    if (style->stroke_type != PLOT_OP_TYPE_NONE) {
        struct rect edge;
        edge = (struct rect){rect->x0, rect->y0, rect->x1, rect->y0}; plot_line(ctx, style, &edge);
        edge = (struct rect){rect->x1 - 1, rect->y0, rect->x1 - 1, rect->y1}; plot_line(ctx, style, &edge);
        edge = (struct rect){rect->x1, rect->y1 - 1, rect->x0, rect->y1 - 1}; plot_line(ctx, style, &edge);
        edge = (struct rect){rect->x0, rect->y1, rect->x0, rect->y0}; plot_line(ctx, style, &edge);
    }
    return NSERROR_OK;
}

static nserror plot_disc(const struct redraw_context *ctx,
                         const plot_style_t *style, int cx, int cy, int radius) {
    struct buzzos_plot_target *target = ctx->priv;
    uint8_t c = palette_colour(style->fill_type != PLOT_OP_TYPE_NONE ?
                               style->fill_colour : style->stroke_colour);
    for (int y = -radius; y <= radius; y++)
        for (int x = -radius; x <= radius; x++)
            if (x * x + y * y <= radius * radius) pixel(target, cx + x, cy + y, c);
    return NSERROR_OK;
}

static nserror plot_arc(const struct redraw_context *ctx, const plot_style_t *style,
                        int x, int y, int radius, int angle1, int angle2) {
    (void)angle1; (void)angle2;
    return plot_disc(ctx, style, x, y, radius);
}

static nserror plot_polygon(const struct redraw_context *ctx,
                            const plot_style_t *style, const int *points,
                            unsigned int count) {
    if (count < 2) return NSERROR_OK;
    for (unsigned int i = 0; i < count; i++) {
        struct rect edge = { points[i * 2], points[i * 2 + 1],
            points[((i + 1) % count) * 2], points[((i + 1) % count) * 2 + 1] };
        plot_line(ctx, style, &edge);
    }
    return NSERROR_OK;
}

static nserror plot_path(const struct redraw_context *ctx,
                         const plot_style_t *style, const float *path,
                         unsigned int count, const float transform[6]) {
    (void)ctx; (void)style; (void)path; (void)count; (void)transform;
    return NSERROR_OK;
}

static nserror plot_bitmap(const struct redraw_context *ctx, struct bitmap *bitmap,
                           int x, int y, int width, int height, colour background,
                           bitmap_flags_t flags) {
    (void)background; (void)flags;
    struct buzzos_plot_target *target = ctx->priv;
    if (!bitmap || !bitmap->ptr || width <= 0 || height <= 0) return NSERROR_OK;
    const uint8_t *source = bitmap->ptr;
    size_t stride = bitmap->rowstride ? bitmap->rowstride : (size_t)bitmap->width * 4u;
    for (int dy = 0; dy < height; dy++) {
        int sy = dy * bitmap->height / height;
        for (int dx = 0; dx < width; dx++) {
            int sx = dx * bitmap->width / width;
            const uint8_t *rgba = source + (size_t)sy * stride + (size_t)sx * 4u;
            if (rgba[3] < 32) continue;
            unsigned r = (rgba[0] * 5u + 127u) / 255u;
            unsigned g = (rgba[1] * 5u + 127u) / 255u;
            unsigned b = (rgba[2] * 5u + 127u) / 255u;
            pixel(target, x + dx, y + dy, (uint8_t)(40u + r * 36u + g * 6u + b));
        }
    }
    return NSERROR_OK;
}

static nserror plot_text(const struct redraw_context *ctx,
                         const plot_font_style_t *style, int x, int y,
                         const char *text, size_t length) {
    struct buzzos_plot_target *target = ctx->priv;
    const char *cursor = text;
    const char *end = text + length;
    int scale = font_scale(style);
    int top = y - (KFONT_HEIGHT - 2) * scale;
    uint8_t colour_value = palette_colour(style->foreground);
    bool bold = style->weight >= 600;
    while (cursor < end) {
        uint32_t cp = bounded_codepoint(&cursor, end);
        uint8_t bits[FONT_GLYPH_BYTES];
        const uint8_t *alpha = NULL;
        int glyph_width = KFONT_WIDTH;
        if (cp >= KFONT_FIRST && cp < KFONT_FIRST + KFONT_COUNT) {
            alpha = &kfont_alpha[cp - KFONT_FIRST][0][0];
        } else if (cp >= 0x80u) {
            glyph_width = font_glyph(cp, bits, sizeof(bits));
            if (glyph_width <= 0) { cp = '?'; glyph_width = KFONT_WIDTH; }
            if (cp == '?') alpha = &kfont_alpha[cp - KFONT_FIRST][0][0];
        } else {
            cp = '?'; alpha = &kfont_alpha[cp - KFONT_FIRST][0][0];
        }
        for (int gy = 0; gy < KFONT_HEIGHT; gy++) {
            for (int gx = 0; gx < glyph_width; gx++) {
                bool on = alpha ? alpha[gy * KFONT_WIDTH + gx] >= 128 :
                    (bits[gy * FONT_GLYPH_STRIDE + gx / 8] &
                     (uint8_t)(0x80u >> (gx & 7)));
                if (!on) continue;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++) {
                        pixel(target, x + gx * scale + sx, top + gy * scale + sy,
                              colour_value);
                        if (bold)
                            pixel(target, x + gx * scale + sx + 1,
                                  top + gy * scale + sy, colour_value);
                    }
            }
        }
        x += glyph_width * scale;
    }
    return NSERROR_OK;
}

const struct plotter_table buzzos_plotters = {
    .clip = plot_clip,
    .arc = plot_arc,
    .disc = plot_disc,
    .line = plot_line,
    .rectangle = plot_rectangle,
    .polygon = plot_polygon,
    .path = plot_path,
    .bitmap = plot_bitmap,
    .text = plot_text,
    .option_knockout = false,
};
