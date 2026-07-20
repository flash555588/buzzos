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
    if (!ctx || argc < 4 || strcmp(argv[1], "--buzz-gui") != 0)
        return -1;
    ctx->event_fd = atoi(argv[2]);
    ctx->frame_fd = atoi(argv[3]);
    if (ctx->event_fd < 0 || ctx->frame_fd < 0)
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

int guiapp_send_frame(struct guiapp_ctx *ctx, const char *title,
                      int width, int height, const uint8_t *pixels) {
    struct guiapp_frame frame;
    if (!ctx || !pixels || width <= 0 || height <= 0)
        return -1;
    int send_w = width > GUIAPP_MAX_W ? GUIAPP_MAX_W : width;
    int send_h = height > GUIAPP_MAX_H ? GUIAPP_MAX_H : height;
    init_frame(&frame, GUIAPP_FRAME_FULL);
    frame.width = send_w;
    frame.height = send_h;
    frame.x = 0;
    frame.y = 0;
    frame.dirty_w = send_w;
    frame.dirty_h = send_h;
    copy_field(frame.title, title, GUIAPP_TITLE_MAX);
    if (write_full(ctx->frame_fd, &frame, (int)sizeof(frame)) < 0)
        return -1;
    if (send_w == width && send_h == height)
        return write_full(ctx->frame_fd, pixels, send_w * send_h);
    for (int y = 0; y < send_h; y++) {
        if (write_full(ctx->frame_fd, pixels + y * width, send_w) < 0)
            return -1;
    }
    return 0;
}

int guiapp_send_dirty(struct guiapp_ctx *ctx, const char *title,
                      int width, int height, int x, int y, int dirty_w, int dirty_h,
                      const uint8_t *pixels, int stride) {
    struct guiapp_frame frame;
    if (!ctx || !pixels || width <= 0 || height <= 0 || stride <= 0)
        return -1;
    if (width > GUIAPP_MAX_W || height > GUIAPP_MAX_H)
        return guiapp_send_frame(ctx, title, width, height, pixels);
    if (x < 0) { dirty_w += x; x = 0; }
    if (y < 0) { dirty_h += y; y = 0; }
    if (x + dirty_w > width) dirty_w = width - x;
    if (y + dirty_h > height) dirty_h = height - y;
    if (dirty_w <= 0 || dirty_h <= 0)
        return guiapp_send_frame(ctx, title, width, height, pixels);
    init_frame(&frame, GUIAPP_FRAME_DIRTY);
    frame.width = width;
    frame.height = height;
    frame.x = x;
    frame.y = y;
    frame.dirty_w = dirty_w;
    frame.dirty_h = dirty_h;
    copy_field(frame.title, title, GUIAPP_TITLE_MAX);
    if (write_full(ctx->frame_fd, &frame, (int)sizeof(frame)) < 0)
        return -1;
    for (int row = 0; row < dirty_h; row++) {
        const uint8_t *src = pixels + (y + row) * stride + x;
        if (write_full(ctx->frame_fd, src, dirty_w) < 0)
            return -1;
    }
    return 0;
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
