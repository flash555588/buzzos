#ifndef BUZZOS_GUIAPP_H
#define BUZZOS_GUIAPP_H

#include <stddef.h>
#include <stdint.h>

#define GUIAPP_MAGIC 0x47554941u
#define GUIAPP_MAX_W 1920
#define GUIAPP_MAX_H 1200
#define GUIAPP_TITLE_MAX 32
#define GUIAPP_PATH_MAX 128
#define GUIAPP_TEXT_MAX 32
#define GUIAPP_SHARED_HEADER_SIZE 4096
#define GUIAPP_SHARED_PIXELS (GUIAPP_MAX_W * GUIAPP_MAX_H)
/* Surfaces are 32bpp 0x00RRGGBB (4 bytes per pixel). */
#define GUIAPP_BYTES_PER_PIXEL 4u
#define GUIAPP_SHARED_SIZE_FOR_PIXELS(pixels) \
    (GUIAPP_SHARED_HEADER_SIZE + (size_t)(pixels) * GUIAPP_BYTES_PER_PIXEL)
#define GUIAPP_SHARED_SIZE GUIAPP_SHARED_SIZE_FOR_PIXELS(GUIAPP_SHARED_PIXELS)

enum {
    GUIAPP_EVT_INIT = 1,
    GUIAPP_EVT_RESIZE = 2,
    GUIAPP_EVT_KEY = 3,
    GUIAPP_EVT_MOUSE = 4,
    GUIAPP_EVT_CLOSE = 5,
    GUIAPP_EVT_TEXT = 6,
    GUIAPP_EVT_COMMAND = 7,
    /* Desktop heartbeat for applications with live status surfaces. */
    GUIAPP_EVT_TICK = 8,
};

enum { GUIAPP_CMD_COPY = 1, GUIAPP_CMD_PASTE = 2, GUIAPP_CMD_CUT = 3 };

enum {
    GUIAPP_KEY_ESC = 0x1B,
    GUIAPP_KEY_BACKSPACE = 0x08,
    GUIAPP_KEY_UP = 256,
    GUIAPP_KEY_DOWN,
    GUIAPP_KEY_RIGHT,
    GUIAPP_KEY_LEFT,
};

struct guiapp_event {
    uint32_t magic;
    uint32_t type;
    int32_t width;
    int32_t height;
    int32_t x;
    int32_t y;
    int32_t key;
    int32_t buttons;
    int32_t wheel;
    char text[GUIAPP_TEXT_MAX];
};

struct guiapp_frame {
    uint32_t magic;
    int32_t type;
    int32_t width;
    int32_t height;
    int32_t x;
    int32_t y;
    int32_t dirty_w;
    int32_t dirty_h;
    uint32_t sequence;
    char title[GUIAPP_TITLE_MAX];
    char target[GUIAPP_PATH_MAX];
    char argument[GUIAPP_PATH_MAX];
};

enum {
    GUIAPP_FRAME_FULL = 1,
    GUIAPP_FRAME_DIRTY = 2,
    GUIAPP_FRAME_LAUNCH = 3,
    GUIAPP_FRAME_CLIPBOARD = 4,
    GUIAPP_FRAME_EXEC = 5,
    GUIAPP_FRAME_SCALED = 6,
    /* App -> desktop: text caret in content-local pixels (frame.x/y). */
    GUIAPP_FRAME_CARET = 7,
};

struct guiapp_shared_surface {
    volatile uint32_t sequence;
    uint32_t capacity_pixels;
    volatile uint32_t width;
    volatile uint32_t height;
    /* Desktop → app: latest content size for live-resize.  guiapp_read_event
     * overlays these onto INIT/RESIZE so queued intermediate events still
     * deliver the current geometry (Wayland-style configure coalesce). */
    volatile uint32_t configure_width;
    volatile uint32_t configure_height;
};

struct guiapp_ctx {
    int event_fd;
    int frame_fd;
    uint32_t shm_token;
    struct guiapp_shared_surface *shared;
    uint32_t capacity_pixels;
};

int guiapp_parse_args(int argc, char **argv, struct guiapp_ctx *ctx);
/* Blocking read.  INIT/RESIZE width/height are replaced with the desktop's
 * latest configure size when present, so apps always layout to current
 * geometry even if several RESIZE events were queued. */
int guiapp_read_event(struct guiapp_ctx *ctx, struct guiapp_event *ev);
int guiapp_send_frame(struct guiapp_ctx *ctx, const char *title,
                      int width, int height, const uint32_t *pixels);
int guiapp_send_scaled_frame(struct guiapp_ctx *ctx, const char *title,
                             int width, int height, const uint32_t *pixels,
                             int source_width, int source_height);
int guiapp_send_dirty(struct guiapp_ctx *ctx, const char *title,
                      int width, int height, int x, int y, int dirty_w, int dirty_h,
                      const uint32_t *pixels, int stride);
int guiapp_request_launch(struct guiapp_ctx *ctx, const char *target,
                          const char *argument);
int guiapp_set_clipboard(struct guiapp_ctx *ctx, const char *text);
int guiapp_request_exec(struct guiapp_ctx *ctx, const char *path);
int guiapp_send_caret(struct guiapp_ctx *ctx, int x, int y);

#endif
