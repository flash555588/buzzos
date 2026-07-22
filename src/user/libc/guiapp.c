#include "guiapp.h"
#include "libc.h"

static int read_full(int fd, void *buf, int size) {
    uint8_t *p = (uint8_t *)buf;
    int done = 0;
    while (done < size) {
        int n = read(fd, p + done, (size_t)(size - done));
        if (n <= 0)
            return -1;
        done += n;
    }
    return 0;
}

static int write_full(int fd, const void *buf, int size) {
    const uint8_t *p = (const uint8_t *)buf;
    int done = 0;
    while (done < size) {
        int n = write(fd, p + done, (size_t)(size - done));
        if (n <= 0)
            return -1;
        done += n;
    }
    return 0;
}

static void init_frame(struct guiapp_frame *frame, int type) {
    memset(frame, 0, sizeof(*frame));
    frame->magic = GUIAPP_MAGIC;
    frame->type = type;
}

static void copy_field(char *dst, const char *src, int cap) {
    int i = 0;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

int guiapp_parse_args(int argc, char **argv, struct guiapp_ctx *ctx) {
    if (!ctx || argc < 6 || strcmp(argv[1], "--buzz-gui") != 0)
        return -1;
    ctx->event_fd = atoi(argv[2]);
    ctx->frame_fd = atoi(argv[3]);
    if (ctx->event_fd < 0 || ctx->frame_fd < 0)
        return -1;
    ctx->shm_token = (uint32_t)strtoul(argv[5], 0, 10);
    struct shm_mapping mapping;
    if (!ctx->shm_token || shm_map(ctx->shm_token, &mapping) < 0 ||
        mapping.size < GUIAPP_SHARED_HEADER_SIZE)
        return -1;
    ctx->shared = (struct guiapp_shared_surface *)mapping.address;
    ctx->capacity_pixels = ctx->shared->capacity_pixels;
    if (!ctx->capacity_pixels || ctx->capacity_pixels > GUIAPP_SHARED_PIXELS ||
        mapping.size < GUIAPP_SHARED_SIZE_FOR_PIXELS(ctx->capacity_pixels))
        return -1;
    return 0;
}

int guiapp_read_event(struct guiapp_ctx *ctx, struct guiapp_event *ev) {
    if (!ctx || !ev)
        return -1;
    if (read_full(ctx->event_fd, ev, (int)sizeof(*ev)) < 0)
        return -1;
    return ev->magic == GUIAPP_MAGIC ? 0 : -1;
}

static uint8_t *shared_pixels(struct guiapp_ctx *ctx) {
    return (uint8_t *)ctx->shared + GUIAPP_SHARED_HEADER_SIZE;
}

static int surface_fits(struct guiapp_ctx *ctx, int width, int height) {
    if (!ctx || width <= 0 || height <= 0 ||
        width > GUIAPP_MAX_W || height > GUIAPP_MAX_H)
        return 0;
    return (uint32_t)width <= ctx->capacity_pixels / (uint32_t)height;
}

static uint32_t begin_surface_write(struct guiapp_ctx *ctx) {
    uint32_t sequence = ctx->shared->sequence;
    if (sequence & 1u)
        sequence++;
    ctx->shared->sequence = sequence + 1u;
    __sync_synchronize();
    return sequence + 2u;
}

static void end_surface_write(struct guiapp_ctx *ctx, uint32_t sequence,
                              int width, int height) {
    ctx->shared->width = (uint32_t)width;
    ctx->shared->height = (uint32_t)height;
    __sync_synchronize();
    ctx->shared->sequence = sequence;
    __sync_synchronize();
}

static int publish_frame(struct guiapp_ctx *ctx, const char *title,
                         int width, int height, const uint8_t *pixels, int stride) {
    struct guiapp_frame frame;
    if (!ctx || !ctx->shared || !pixels || stride < width ||
        !surface_fits(ctx, width, height))
        return -1;
    uint32_t sequence = begin_surface_write(ctx);
    uint8_t *dst = shared_pixels(ctx);
    for (int y = 0; y < height; y++)
        memcpy(dst + y * width, pixels + y * stride, (size_t)width);
    end_surface_write(ctx, sequence, width, height);

    init_frame(&frame, GUIAPP_FRAME_FULL);
    frame.width = width;
    frame.height = height;
    frame.x = 0;
    frame.y = 0;
    frame.dirty_w = width;
    frame.dirty_h = height;
    frame.sequence = sequence;
    copy_field(frame.title, title, GUIAPP_TITLE_MAX);
    return write_full(ctx->frame_fd, &frame, (int)sizeof(frame));
}

int guiapp_send_frame(struct guiapp_ctx *ctx, const char *title,
                      int width, int height, const uint8_t *pixels) {
    return publish_frame(ctx, title, width, height, pixels, width);
}

int guiapp_send_scaled_frame(struct guiapp_ctx *ctx, const char *title,
                             int width, int height, const uint8_t *pixels,
                             int source_width, int source_height) {
    struct guiapp_frame frame;
    if (!ctx || !ctx->shared || !pixels || width <= 0 || height <= 0 ||
        !surface_fits(ctx, source_width, source_height))
        return -1;
    uint32_t sequence = begin_surface_write(ctx);
    uint8_t *dst = shared_pixels(ctx);
    for (int y = 0; y < source_height; y++)
        memcpy(dst + y * source_width, pixels + y * source_width,
               (size_t)source_width);
    end_surface_write(ctx, sequence, source_width, source_height);
    init_frame(&frame, GUIAPP_FRAME_SCALED);
    frame.width = width > GUIAPP_MAX_W ? GUIAPP_MAX_W : width;
    frame.height = height > GUIAPP_MAX_H ? GUIAPP_MAX_H : height;
    frame.dirty_w = source_width;
    frame.dirty_h = source_height;
    frame.sequence = sequence;
    copy_field(frame.title, title, GUIAPP_TITLE_MAX);
    return write_full(ctx->frame_fd, &frame, (int)sizeof(frame));
}

int guiapp_send_dirty(struct guiapp_ctx *ctx, const char *title,
                      int width, int height, int x, int y, int dirty_w, int dirty_h,
                      const uint8_t *pixels, int stride) {
    if (!ctx || !ctx->shared || !pixels || stride < width ||
        !surface_fits(ctx, width, height))
        return -1;
    if (x < 0) { dirty_w += x; x = 0; }
    if (y < 0) { dirty_h += y; y = 0; }
    if (x + dirty_w > width) dirty_w = width - x;
    if (y + dirty_h > height) dirty_h = height - y;
    if (dirty_w <= 0 || dirty_h <= 0)
        return 0;
    if (ctx->shared->width != (uint32_t)width ||
        ctx->shared->height != (uint32_t)height)
        return publish_frame(ctx, title, width, height, pixels, stride);

    uint32_t sequence = begin_surface_write(ctx);
    uint8_t *dst = shared_pixels(ctx);
    for (int row = 0; row < dirty_h; row++)
        memcpy(dst + (y + row) * width + x,
               pixels + (y + row) * stride + x, (size_t)dirty_w);
    end_surface_write(ctx, sequence, width, height);

    struct guiapp_frame frame;
    init_frame(&frame, GUIAPP_FRAME_DIRTY);
    frame.width = width;
    frame.height = height;
    frame.x = x;
    frame.y = y;
    frame.dirty_w = dirty_w;
    frame.dirty_h = dirty_h;
    frame.sequence = sequence;
    copy_field(frame.title, title, GUIAPP_TITLE_MAX);
    return write_full(ctx->frame_fd, &frame, (int)sizeof(frame));
}

int guiapp_request_launch(struct guiapp_ctx *ctx, const char *target,
                          const char *argument) {
    struct guiapp_frame frame;
    if (!ctx || !target || !target[0])
        return -1;
    init_frame(&frame, GUIAPP_FRAME_LAUNCH);
    copy_field(frame.target, target, GUIAPP_PATH_MAX);
    copy_field(frame.argument, argument, GUIAPP_PATH_MAX);
    return write_full(ctx->frame_fd, &frame, (int)sizeof(frame));
}

int guiapp_set_clipboard(struct guiapp_ctx *ctx, const char *text) {
    struct guiapp_frame frame;
    if (!ctx || !text)
        return -1;
    init_frame(&frame, GUIAPP_FRAME_CLIPBOARD);
    copy_field(frame.argument, text, GUIAPP_PATH_MAX);
    return write_full(ctx->frame_fd, &frame, (int)sizeof(frame));
}

int guiapp_request_exec(struct guiapp_ctx *ctx, const char *path) {
    struct guiapp_frame frame;
    if (!ctx || !path || !path[0])
        return -1;
    init_frame(&frame, GUIAPP_FRAME_EXEC);
    copy_field(frame.target, path, GUIAPP_PATH_MAX);
    return write_full(ctx->frame_fd, &frame, (int)sizeof(frame));
}
