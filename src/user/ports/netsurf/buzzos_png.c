#include "libc.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lodepng.h"

#include "utils/errors.h"
#include "netsurf/bitmap.h"
#include "netsurf/content.h"
#include "content/content_factory.h"
#include "content/content_protected.h"
#include "content/llcache.h"
#include "desktop/gui_internal.h"
#include "image/image.h"

#include "buzzos_png.h"

struct buzzos_png_content {
    struct content base;
    struct bitmap *bitmap;
};

void *lodepng_malloc(size_t size) { return malloc(size); }
void *lodepng_realloc(void *ptr, size_t size) { return realloc(ptr, size); }
void lodepng_free(void *ptr) { free(ptr); }

static nserror buzzos_png_create(const struct content_handler *handler,
                                 lwc_string *mime_type,
                                 const struct http_parameter *params,
                                 llcache_handle *llcache,
                                 const char *fallback_charset,
                                 bool quirks,
                                 struct content **result) {
    struct buzzos_png_content *png = calloc(1, sizeof(*png));
    if (png == NULL) return NSERROR_NOMEM;
    nserror error = content__init(&png->base, handler, mime_type, params,
                                  llcache, fallback_charset, quirks);
    if (error != NSERROR_OK) {
        free(png);
        return error;
    }
    *result = &png->base;
    return NSERROR_OK;
}

static bool buzzos_png_convert(struct content *content) {
    struct buzzos_png_content *png = (struct buzzos_png_content *)content;
    size_t source_size = 0;
    const uint8_t *source = content__get_source_data(content, &source_size);
    unsigned char *rgba = NULL;
    unsigned width = 0;
    unsigned height = 0;
    unsigned decode_error = lodepng_decode32(&rgba, &width, &height,
                                              source, source_size);
    if (decode_error != 0 || width == 0 || height == 0 ||
        width > 16384u || height > 16384u) {
        free(rgba);
        content_broadcast_error(content, NSERROR_PNG_ERROR, NULL);
        return false;
    }

    png->bitmap = guit->bitmap->create((int)width, (int)height, BITMAP_NONE);
    if (png->bitmap == NULL) {
        free(rgba);
        content_broadcast_error(content, NSERROR_NOMEM, NULL);
        return false;
    }

    uint8_t *destination = guit->bitmap->get_buffer(png->bitmap);
    size_t stride = guit->bitmap->get_rowstride(png->bitmap);
    if (stride == 0) stride = (size_t)width * 4u;
    bool opaque = true;
    for (unsigned y = 0; y < height; y++) {
        memcpy(destination + (size_t)y * stride,
               rgba + (size_t)y * width * 4u,
               (size_t)width * 4u);
        for (unsigned x = 0; x < width; x++) {
            if (rgba[((size_t)y * width + x) * 4u + 3u] != 255u)
                opaque = false;
        }
    }
    free(rgba);

    guit->bitmap->set_opaque(png->bitmap, opaque);
    guit->bitmap->modified(png->bitmap);
    content->width = (int)width;
    content->height = (int)height;
    content->size += (size_t)width * height * 4u;
    content_set_ready(content);
    content_set_done(content);
    content_set_status(content, "");
    return true;
}

static bool buzzos_png_redraw(struct content *content,
                              struct content_redraw_data *data,
                              const struct rect *clip,
                              const struct redraw_context *ctx) {
    struct buzzos_png_content *png = (struct buzzos_png_content *)content;
    if (png->bitmap == NULL) return false;
    return image_bitmap_plot(png->bitmap, data, clip, ctx);
}

static void buzzos_png_destroy(struct content *content) {
    struct buzzos_png_content *png = (struct buzzos_png_content *)content;
    if (png->bitmap != NULL) guit->bitmap->destroy(png->bitmap);
}

static nserror buzzos_png_clone(const struct content *old_content,
                                struct content **result) {
    struct buzzos_png_content *clone = calloc(1, sizeof(*clone));
    if (clone == NULL) return NSERROR_NOMEM;
    nserror error = content__clone(old_content, &clone->base);
    if (error != NSERROR_OK) {
        free(clone);
        return error;
    }
    if (old_content->status == CONTENT_STATUS_READY ||
        old_content->status == CONTENT_STATUS_DONE) {
        if (!buzzos_png_convert(&clone->base)) {
            content_destroy(&clone->base);
            return NSERROR_CLONE_FAILED;
        }
    }
    *result = &clone->base;
    return NSERROR_OK;
}

static void *buzzos_png_get_internal(const struct content *content,
                                     void *context) {
    (void)context;
    return ((const struct buzzos_png_content *)content)->bitmap;
}

static content_type buzzos_png_type(void) { return CONTENT_IMAGE; }

static bool buzzos_png_is_opaque(struct content *content) {
    struct buzzos_png_content *png = (struct buzzos_png_content *)content;
    return png->bitmap != NULL && guit->bitmap->get_opaque(png->bitmap);
}

static const content_handler buzzos_png_handler = {
    .create = buzzos_png_create,
    .data_complete = buzzos_png_convert,
    .destroy = buzzos_png_destroy,
    .redraw = buzzos_png_redraw,
    .clone = buzzos_png_clone,
    .get_internal = buzzos_png_get_internal,
    .type = buzzos_png_type,
    .is_opaque = buzzos_png_is_opaque,
    .no_share = false,
};

nserror buzzos_png_init(void) {
    return content_factory_register_handler("image/png", &buzzos_png_handler);
}
