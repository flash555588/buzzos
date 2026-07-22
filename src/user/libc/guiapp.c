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
        mapping.size < GUIAPP_SHARED_SIZE)
        return -1;
    ctx->shared = (struct guiapp_shared_surface *)mapping.address;
    return 0;
}

int guiapp_read_event(struct guiapp_ctx *ctx, struct guiapp_event *ev) {
    if (!ctx || !ev)
        return -1;
    if (read_full(ctx->event_fd, ev, (int)sizeof(*ev)) < 0)
        return -1;
    return ev->magic == GUIAPP_MAGIC ? 0 : -1;
}

static uint8_t *shared_buffer(struct guiapp_ctx *ctx, int index) {
    return (uint8_t *)ctx->shared + GUIAPP_SHARED_HEADER_SIZE +
           index * GUIAPP_SHARED_PIXELS;
}

static int publish_frame(struct guiapp_ctx *ctx, const char *title,
                         int width, int height, const uint8_t *pixels, int stride) {
    struct guiapp_frame frame;
    if (!ctx || !ctx->shared || !pixels || width <= 0 || height <= 0 || stride <= 0)
        return -1;
    int send_w = width > GUIAPP_MAX_W ? GUIAPP_MAX_W : width;
    int send_h = height > GUIAPP_MAX_H ? GUIAPP_MAX_H : height;
    uint32_t front = ctx->shared->front;
    uint32_t reader = ctx->shared->reader;
    int index = 0;
    while ((uint32_t)index == front || (uint32_t)index == reader)
        index++;
    if (index >= GUIAPP_SHARED_BUFFERS)
        index = (int)((front + 1u) % GUIAPP_SHARED_BUFFERS);
    uint8_t *dst = shared_buffer(ctx, index);
    for (int y = 0; y < send_h; y++)
        memcpy(dst + y * send_w, pixels + y * stride, (size_t)send_w);
    __sync_synchronize();
    ctx->shared->front = (uint32_t)index;
    ctx->shared->sequence++;
    __sync_synchronize();

    init_frame(&frame, GUIAPP_FRAME_SHARED);
    frame.width = send_w;
    frame.height = send_h;
    frame.x = 0;
    frame.y = 0;
    frame.dirty_w = send_w;
    frame.dirty_h = send_h;
    frame.buffer_index = index;
    copy_field(frame.title, title, GUIAPP_TITLE_MAX);
    return write_full(ctx->frame_fd, &frame, (int)sizeof(frame));
}

int guiapp_send_frame(struct guiapp_ctx *ctx, const char *title,
                      int width, int height, const uint8_t *pixels) {
    return publish_frame(ctx, title, width, height, pixels, width);
}

int guiapp_send_dirty(struct guiapp_ctx *ctx, const char *title,
                      int width, int height, int x, int y, int dirty_w, int dirty_h,
                      const uint8_t *pixels, int stride) {
    if (!ctx || !pixels || width <= 0 || height <= 0 || stride <= 0)
        return -1;
    (void)x; (void)y; (void)dirty_w; (void)dirty_h;
    return publish_frame(ctx, title, width, height, pixels, stride);
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
