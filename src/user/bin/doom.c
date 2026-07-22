#include "appui.h"
#include "guiapp.h"
#include "libc.h"

#include "doomgeneric.h"
#include "doomkeys.h"

#define DOOM_W 320
#define DOOM_H 200
#define KEY_QUEUE_SIZE 128
#define DEFAULT_WAD "/fs/games/doom/doom1.wad"

static struct guiapp_ctx gui;
static volatile int gui_ready;
static volatile int gui_closed;
static volatile int requested_w = 640;
static volatile int requested_h = 400;
static volatile unsigned key_head;
static volatile unsigned key_tail;
static uint16_t key_queue[KEY_QUEUE_SIZE];
static uint8_t held_keys[256];
static uint32_t release_at[256];
static uint8_t *frame_pixels;
static int frame_capacity;
static uint8_t native_pixels[DOOM_W * DOOM_H];
static uint16_t scale_x[GUIAPP_MAX_W];
static uint16_t scale_y[GUIAPP_MAX_H];
static char window_title[GUIAPP_TITLE_MAX] = "DOOM";

static uint8_t rgb_to_index(uint32_t pixel) {
    unsigned r = ((pixel >> 16) & 255u) * 5u + 127u;
    unsigned g = ((pixel >> 8) & 255u) * 5u + 127u;
    unsigned b = (pixel & 255u) * 5u + 127u;
    return (uint8_t)(40u + (r / 255u) * 36u + (g / 255u) * 6u + b / 255u);
}

static unsigned char doom_key(int key) {
    switch (key) {
    case GUIAPP_KEY_UP: return KEY_UPARROW;
    case GUIAPP_KEY_DOWN: return KEY_DOWNARROW;
    case GUIAPP_KEY_LEFT: return KEY_LEFTARROW;
    case GUIAPP_KEY_RIGHT: return KEY_RIGHTARROW;
    case '\n': case '\r': return KEY_ENTER;
    case GUIAPP_KEY_BACKSPACE: return KEY_BACKSPACE;
    case ' ': return KEY_USE;
    /* BuzzOS does not expose modifier key events yet. These aliases make the
     * game fully controllable with ordinary key events. */
    case 'x': case 'X': return KEY_FIRE;
    case 'c': case 'C': return KEY_RSHIFT;
    case 'v': case 'V': return KEY_LALT;
    case 'm': case 'M': return KEY_ESCAPE;
    default:
        if (key >= 'A' && key <= 'Z') return (unsigned char)(key - 'A' + 'a');
        return key > 0 && key < 256 ? (unsigned char)key : 0;
    }
}

static void queue_key(int pressed, unsigned char key) {
    if (!key) return;
    unsigned next = (key_tail + 1u) % KEY_QUEUE_SIZE;
    if (next == key_head) return;
    key_queue[key_tail] = (uint16_t)((pressed ? 0x100u : 0u) | key);
    __sync_synchronize();
    key_tail = next;
}

static void gui_event_reader(void) {
    struct guiapp_event event;
    while (guiapp_read_event(&gui, &event) == 0) {
        if (event.type == GUIAPP_EVT_CLOSE) {
            gui_closed = 1;
            break;
        }
        if (event.type == GUIAPP_EVT_INIT || event.type == GUIAPP_EVT_RESIZE) {
            if (event.width > 0) requested_w = event.width;
            if (event.height > 0) requested_h = event.height;
            gui_ready = 1;
        } else if (event.type == GUIAPP_EVT_KEY) {
            unsigned char key = doom_key(event.key);
            if (key) {
                uint32_t now = monotonic_ms();
                if (!held_keys[key]) {
                    held_keys[key] = 1;
                    queue_key(1, key);
                }
                /* Keyboard repeat refreshes the hold. This is replaced by
                 * real release events when the desktop input ABI grows one. */
                release_at[key] = now + 350u;
            }
        }
    }
    gui_closed = 1;
}

void DG_Init(void) {
    while (!gui_ready && !gui_closed)
        sleep_ms(1);
}

void DG_DrawFrame(void) {
    if (gui_closed)
        exit(0);
    int w = requested_w;
    int h = requested_h;
    if (w < 320) w = 320;
    if (h < 200) h = 200;
    if (w > GUIAPP_MAX_W) w = GUIAPP_MAX_W;
    if (h > GUIAPP_MAX_H) h = GUIAPP_MAX_H;
    int needed = w * h;
    if (needed > frame_capacity) {
        uint8_t *grown = realloc(frame_pixels, (size_t)needed);
        if (!grown) return;
        frame_pixels = grown;
        frame_capacity = needed;
    }

    int view_w = w;
    int view_h = (w * DOOM_H) / DOOM_W;
    if (view_h > h) {
        view_h = h;
        view_w = (h * DOOM_W) / DOOM_H;
    }
    int ox = (w - view_w) / 2;
    int oy = (h - view_h) / 2;
    /* Quantize the native DOOM frame once. Quantizing after scaling made a
     * maximized window perform up to a million RGB conversions per frame. */
    for (int i = 0; i < DOOM_W * DOOM_H; i++)
        native_pixels[i] = rgb_to_index(DG_ScreenBuffer[i]);

    /* Division in the inner scaling loop dominated large windows under
     * emulation. Build compact coordinate maps once per frame instead. */
    for (int x = 0; x < view_w; x++)
        scale_x[x] = (uint16_t)(x * DOOM_W / view_w);
    for (int y = 0; y < view_h; y++)
        scale_y[y] = (uint16_t)(y * DOOM_H / view_h);

    memset(frame_pixels, 0, (size_t)needed);
    for (int y = 0; y < view_h; y++) {
        int sy = scale_y[y];
        uint8_t *dst = frame_pixels + (oy + y) * w + ox;
        for (int x = 0; x < view_w; x++)
            dst[x] = native_pixels[sy * DOOM_W + scale_x[x]];
    }
    if (guiapp_send_frame(&gui, window_title, w, h, frame_pixels) < 0)
        gui_closed = 1;
}

void DG_SleepMs(uint32_t ms) { sleep_ms(ms); }
uint32_t DG_GetTicksMs(void) { return monotonic_ms(); }

int DG_GetKey(int *pressed, unsigned char *key) {
    uint32_t now = monotonic_ms();
    for (int i = 1; i < 256; i++) {
        if (held_keys[i] && (int32_t)(now - release_at[i]) >= 0) {
            held_keys[i] = 0;
            queue_key(0, (unsigned char)i);
            break;
        }
    }
    if (key_head == key_tail) return 0;
    uint16_t value = key_queue[key_head];
    key_head = (key_head + 1u) % KEY_QUEUE_SIZE;
    *pressed = (value & 0x100u) != 0;
    *key = (unsigned char)value;
    return 1;
}

void DG_SetWindowTitle(const char *title) {
    if (!title) return;
    int i = 0;
    while (title[i] && i + 1 < GUIAPP_TITLE_MAX) {
        window_title[i] = title[i];
        i++;
    }
    window_title[i] = 0;
}

static int wad_exists(const char *path) {
    struct stat status;
    return stat(path, &status) == 0 && status.st_type == DT_REG && status.st_size > 0;
}

static int missing_wad_loop(void) {
    struct guiapp_event event;
    int w = 640, h = 400;
    uint8_t *pixels = malloc((size_t)GUIAPP_MAX_W * GUIAPP_MAX_H);
    if (!pixels) return 1;
    for (;;) {
        if (guiapp_read_event(&gui, &event) < 0 || event.type == GUIAPP_EVT_CLOSE)
            break;
        if (event.type == GUIAPP_EVT_INIT || event.type == GUIAPP_EVT_RESIZE) {
            w = event.width; h = event.height;
            if (w < 360) w = 360; if (w > GUIAPP_MAX_W) w = GUIAPP_MAX_W;
            if (h < 220) h = 220; if (h > GUIAPP_MAX_H) h = GUIAPP_MAX_H;
        }
        appui_fill(pixels, w, h, (struct appui_rect){0, 0, w, h}, appui_gray(2));
        appui_text(pixels, w, h, 28, 28, "DOOM engine is ready", 15, -1,
                   (struct appui_rect){20, 20, w - 40, 28});
        appui_text(pixels, w, h, 28, 72, "Game data was not found:", appui_rgb6(5, 3, 1), -1,
                   (struct appui_rect){20, 64, w - 40, 28});
        appui_text(pixels, w, h, 28, 104, DEFAULT_WAD, 15, appui_gray(1),
                   (struct appui_rect){20, 96, w - 40, 30});
        appui_text(pixels, w, h, 28, 150,
                   "Install the shareware doom1.wad, then reopen DOOM.", 15, -1,
                   (struct appui_rect){20, 142, w - 40, 28});
        if (guiapp_send_frame(&gui, "DOOM - WAD required", w, h, pixels) < 0)
            break;
    }
    free(pixels);
    return 0;
}

int main(int argc, char **argv) {
    if (guiapp_parse_args(argc, argv, &gui) < 0)
        return 1;
    if (!wad_exists(DEFAULT_WAD))
        return missing_wad_loop();

    mkdir("/fs/games");
    mkdir("/fs/games/doom");
    chdir("/fs/games/doom");
    if (spawn(gui_event_reader) < 0)
        return 2;
    char *doom_argv[] = {
        "doom", "-iwad", DEFAULT_WAD,
        "-config", "/fs/games/doom/default.cfg",
        "-mb", "12",
    };
    doomgeneric_Create((int)(sizeof(doom_argv) / sizeof(doom_argv[0])), doom_argv);
    /* doomgeneric_Create initializes the engine, but every platform must
     * continue driving the game loop itself. */
    while (!gui_closed)
        doomgeneric_Tick();
    return 0;
}
