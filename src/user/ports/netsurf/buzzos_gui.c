#include "appui.h"
#include "guiapp.h"
#include "libc.h"

#include "utils/errors.h"
#include "utils/log.h"
#include "utils/nsoption.h"
#include "utils/nsurl.h"
#include "content/backing_store.h"
#include "netsurf/browser_window.h"
#include "netsurf/keypress.h"
#include "netsurf/fetch.h"
#include "netsurf/misc.h"
#include "netsurf/mouse.h"
#include "netsurf/netsurf.h"
#include "netsurf/plotters.h"
#include "netsurf/window.h"
#include "desktop/browser_history.h"

#include "monkey/bitmap.h"
#include "monkey/browser.h"
#include "monkey/layout.h"
#include "monkey/schedule.h"

#include "buzzos_gui_plot.h"
#include "buzzos_netsurf_resources.h"
#include "buzzos_http_fetch.h"
#include "buzzos_png.h"

#define TOOLBAR_HEIGHT 54
#define STATUS_HEIGHT 22
#define URL_CAPACITY 512

static uint8_t pixels[GUIAPP_MAX_W * GUIAPP_MAX_H];
static char address_text[URL_CAPACITY] = "http://example.com/";
static char window_title[GUIAPP_TITLE_MAX] = "NetSurf";
static char status_text[128] = "NetSurf engine ready";
static int address_length;
static bool address_focused = true;
static int frame_width = 800;
static int frame_height = 560;
static int previous_buttons;
static bool page_mouse_pressed;
static struct guiapp_ctx application;
static struct guiapp_event pending_event;
static volatile int pending_event_state;
static volatile bool redraw_pending = true;

static nserror (*monkey_invalidate)(struct gui_window *, const struct rect *);
static nserror (*monkey_event)(struct gui_window *, enum gui_window_event);

static nserror buzzos_window_invalidate(struct gui_window *window,
                                        const struct rect *rect) {
    redraw_pending = true;
    return monkey_invalidate(window, rect);
}

static nserror buzzos_window_event(struct gui_window *window,
                                   enum gui_window_event event) {
    if (event == GW_EVENT_UPDATE_EXTENT || event == GW_EVENT_NEW_CONTENT ||
        event == GW_EVENT_STOP_THROBBER)
        redraw_pending = true;
    return monkey_event(window, event);
}

static void read_gui_events(void) {
    for (;;) {
        struct guiapp_event event;
        if (guiapp_read_event(&application, &event) < 0) {
            pending_event_state = -1;
            futex_wake((int *)&pending_event_state, 1);
            return;
        }
        while (pending_event_state == 1)
            futex_wait((int *)&pending_event_state, 1);
        pending_event = event;
        __sync_synchronize();
        pending_event_state = 1;
        futex_wake((int *)&pending_event_state, 1);
        if (event.type == GUIAPP_EVT_CLOSE)
            return;
    }
}

static void redirect_logs_to_serial(void) {
    int serial_fd = open("/dev/serial", O_WRONLY);
    if (serial_fd < 0)
        return;
    dup2(serial_fd, 1);
    dup2(serial_fd, 2);
    if (serial_fd > 2)
        close(serial_fd);
}

static const char *filetype(const char *path) {
    const char *extension = strrchr(path, '.');
    if (!extension) return "text/plain";
    if (!strcasecmp(extension, ".html") || !strcasecmp(extension, ".htm")) return "text/html";
    if (!strcasecmp(extension, ".css")) return "text/css";
    if (!strcasecmp(extension, ".png")) return "image/png";
    return "application/octet-stream";
}

static nserror get_resource_data(const char *path, const uint8_t **data,
                                 size_t *data_length) {
    if (!strcmp(path, "default.css")) {
        *data = buzzos_default_css; *data_length = buzzos_default_css_size;
    } else if (!strcmp(path, "adblock.css")) {
        *data = buzzos_adblock_css; *data_length = buzzos_adblock_css_size;
    } else if (!strcmp(path, "internal.css")) {
        *data = buzzos_internal_css; *data_length = buzzos_internal_css_size;
    } else if (!strcmp(path, "quirks.css")) {
        *data = buzzos_quirks_css; *data_length = buzzos_quirks_css_size;
    } else if (!strcmp(path, "user.css")) {
        *data = buzzos_empty_css; *data_length = 0;
    } else {
        return NSERROR_NOT_FOUND;
    }
    return NSERROR_OK;
}

static nserror release_resource_data(const uint8_t *data) {
    (void)data;
    return NSERROR_OK;
}

static nserror schedule_callback(int delay, void (*callback)(void *), void *context) {
    return monkey_schedule(delay, callback, context);
}

static void quit_callback(void) { }

static struct gui_misc_table misc_table = {
    .schedule = schedule_callback,
    .quit = quit_callback,
};

static struct gui_fetch_table fetch_table = {
    .filetype = filetype,
    .get_resource_data = get_resource_data,
    .release_resource_data = release_resource_data,
};

static nserror option_defaults(struct nsoption_s *defaults) {
    (void)defaults;
    nsoption_setnull_charp(cookie_file, strdup("/fs/netsurf.cookies"));
    nsoption_setnull_charp(cookie_jar, strdup("/fs/netsurf.cookies"));
    nsoption_setnull_charp(url_file, strdup("/fs/netsurf.urls"));
    nsoption_setnull_charp(downloads_directory, strdup("/fs"));
    nsoption_setnull_charp(disc_cache_path, strdup("/fs/netsurf-cache"));
    return NSERROR_OK;
}

static bool log_stream(FILE *stream) {
    setbuf(stream, NULL);
    return true;
}

static void pump_netsurf(int maximum_rounds) {
    for (int round = 0; round < maximum_rounds; round++) {
        int delay = monkey_schedule_run();
        if (delay < 0) break;
        /* Immediate callbacks are drained by monkey_schedule_run in the same
         * tick. Only wait for genuinely future short timers; cache and other
         * long-running housekeeping stays asynchronous. */
        if (delay > 50) break;
        if (delay > 0)
            sleep_ms((unsigned int)delay);
    }
}

static struct gui_window *current_window(void) {
    return monkey_find_window_by_num(0);
}

static void draw_browser_page(void) {
    struct gui_window *window = current_window();
    if (!window || !window->bw) return;
    int content_height = frame_height - TOOLBAR_HEIGHT - STATUS_HEIGHT;
    if (content_height < 1) return;
    window->width = frame_width;
    window->height = content_height;
    appui_fill(pixels, frame_width, frame_height,
               (struct appui_rect){0, TOOLBAR_HEIGHT, frame_width, content_height}, 15);
    struct buzzos_plot_target target;
    buzzos_plot_target_init(&target, pixels, frame_width, frame_height, TOOLBAR_HEIGHT);
    struct rect clip = {0, 0, frame_width, content_height};
    struct redraw_context context = {
        .interactive = true,
        .background_images = true,
        .plot = &buzzos_plotters,
        .priv = &target,
    };
    browser_window_redraw(window->bw, window->scrollx,
                          window->scrolly, &clip, &context);
}

static void render_frame(struct guiapp_ctx *application) {
    appui_fill(pixels, frame_width, frame_height,
               (struct appui_rect){0, 0, frame_width, frame_height}, appui_gray(3));
    appui_fill(pixels, frame_width, frame_height,
               (struct appui_rect){0, 0, frame_width, TOOLBAR_HEIGHT}, appui_gray(2));
    appui_button(pixels, frame_width, frame_height,
                 (struct appui_rect){7, 11, 52, 30}, "Back", 1);
    appui_button(pixels, frame_width, frame_height,
                 (struct appui_rect){63, 11, 48, 30}, "Reload", 1);
    struct appui_rect address = {117, 11, frame_width - 185, 30};
    appui_fill(pixels, frame_width, frame_height, address, 15);
    appui_border(pixels, frame_width, frame_height, address, appui_gray(9), appui_gray(1));
    appui_text(pixels, frame_width, frame_height, address.x + 6, address.y + 7,
               address_text, 0, -1,
               (struct appui_rect){address.x + 4, address.y + 2, address.w - 8, address.h - 4});
    int cursor = address.x + 6 + appui_text_width(address_text);
    if (cursor < address.x + address.w - 4)
        appui_fill(pixels, frame_width, frame_height,
                   (struct appui_rect){cursor, address.y + 5, 1, 19}, appui_rgb6(0, 2, 5));
    appui_button(pixels, frame_width, frame_height,
                 (struct appui_rect){frame_width - 62, 11, 54, 30}, "Go", 1);
    draw_browser_page();
    appui_fill(pixels, frame_width, frame_height,
               (struct appui_rect){0, frame_height - STATUS_HEIGHT,
                                   frame_width, STATUS_HEIGHT}, appui_gray(2));
    appui_text(pixels, frame_width, frame_height, 8, frame_height - 17,
               status_text, appui_gray(12), -1,
               (struct appui_rect){5, frame_height - STATUS_HEIGHT,
                                   frame_width - 10, STATUS_HEIGHT});
    guiapp_send_frame(application, window_title, frame_width, frame_height, pixels);
}

static void navigate(void) {
    struct gui_window *window = current_window();
    if (!window || !window->bw || !address_text[0]) return;
    char target[URL_CAPACITY + 64];
    if (!strstr(address_text, "://") && strncmp(address_text, "about:", 6) &&
        strncmp(address_text, "data:", 5)) {
        snprintf(target, sizeof(target), "http://www.bing.com/search?q=%s", address_text);
    } else {
        snprintf(target, sizeof(target), "%s", address_text);
    }
    nsurl *url = NULL;
    nserror error = nsurl_create(target, &url);
    if (error == NSERROR_OK) {
        error = browser_window_navigate(window->bw, url, NULL, BW_NAVIGATE_HISTORY,
                                        NULL, NULL, NULL);
        nsurl_unref(url);
    }
    if (error == NSERROR_OK) {
        snprintf(status_text, sizeof(status_text), "Loading %s", target);
        redraw_pending = true;
    } else {
        snprintf(status_text, sizeof(status_text), "Navigation error %d", error);
    }
}

static void handle_key(int key) {
    struct gui_window *window = current_window();
    if (!address_focused && window && window->bw) {
        uint32_t nskey = 0;
        if (key == GUIAPP_KEY_BACKSPACE || key == 127) nskey = NS_KEY_DELETE_LEFT;
        else if (key == '\r' || key == '\n') nskey = NS_KEY_CR;
        else if (key == GUIAPP_KEY_UP) nskey = NS_KEY_UP;
        else if (key == GUIAPP_KEY_DOWN) nskey = NS_KEY_DOWN;
        else if (key == GUIAPP_KEY_LEFT) nskey = NS_KEY_LEFT;
        else if (key == GUIAPP_KEY_RIGHT) nskey = NS_KEY_RIGHT;
        else if (key == GUIAPP_KEY_ESC) nskey = NS_KEY_ESCAPE;
        else if (key >= 32 && key < 127) nskey = (uint32_t)key;
        if (nskey != 0 && browser_window_key_press(window->bw, nskey))
            return;
        /* Leave unhandled arrows available for page scrolling. */
        if (key == GUIAPP_KEY_UP || key == GUIAPP_KEY_DOWN) {
            window->scrolly += key == GUIAPP_KEY_UP ? -42 : 42;
            if (window->scrolly < 0) window->scrolly = 0;
        }
        return;
    }
    if (key == GUIAPP_KEY_BACKSPACE || key == 127) {
        if (address_length > 0) {
            address_length = appui_utf8_prev(address_text, address_length);
            address_text[address_length] = 0;
        }
    } else if (key == '\r' || key == '\n') {
        navigate();
    } else if (key == GUIAPP_KEY_UP || key == GUIAPP_KEY_DOWN) {
        struct gui_window *window = current_window();
        if (window) {
            window->scrolly += key == GUIAPP_KEY_UP ? -42 : 42;
            if (window->scrolly < 0) window->scrolly = 0;
        }
    } else if (key >= 32 && key < 127 && address_length + 1 < URL_CAPACITY) {
        address_text[address_length++] = (char)key;
        address_text[address_length] = 0;
    }
}

static void handle_text(const char *text) {
    struct gui_window *window = current_window();
    if (!address_focused && window && window->bw) {
        const unsigned char *cursor = (const unsigned char *)text;
        while (*cursor) {
            uint32_t cp;
            if (cursor[0] < 0x80) {
                cp = *cursor++;
            } else if ((cursor[0] & 0xe0) == 0xc0 && cursor[1]) {
                cp = ((uint32_t)(cursor[0] & 0x1f) << 6) |
                     (uint32_t)(cursor[1] & 0x3f);
                cursor += 2;
            } else if ((cursor[0] & 0xf0) == 0xe0 && cursor[1] && cursor[2]) {
                cp = ((uint32_t)(cursor[0] & 0x0f) << 12) |
                     ((uint32_t)(cursor[1] & 0x3f) << 6) |
                     (uint32_t)(cursor[2] & 0x3f);
                cursor += 3;
            } else if ((cursor[0] & 0xf8) == 0xf0 && cursor[1] &&
                       cursor[2] && cursor[3]) {
                cp = ((uint32_t)(cursor[0] & 0x07) << 18) |
                     ((uint32_t)(cursor[1] & 0x3f) << 12) |
                     ((uint32_t)(cursor[2] & 0x3f) << 6) |
                     (uint32_t)(cursor[3] & 0x3f);
                cursor += 4;
            } else {
                cp = 0xfffdu;
                cursor++;
            }
            browser_window_key_press(window->bw, cp);
        }
        return;
    }
    while (*text && address_length + 1 < URL_CAPACITY)
        address_text[address_length++] = *text++;
    address_text[address_length] = 0;
}

static void handle_mouse(int x, int y, int buttons, int wheel) {
    struct gui_window *window = current_window();
    if (!window) return;
    int pressed = (buttons & 1) && !(previous_buttons & 1);
    int released = !(buttons & 1) && (previous_buttons & 1);
    bool over_page = y >= TOOLBAR_HEIGHT && y < frame_height - STATUS_HEIGHT;
    if (wheel) {
        window->scrolly -= wheel * 48;
        if (window->scrolly < 0) window->scrolly = 0;
    }
    if (pressed) {
        if (appui_inside(x, y, (struct appui_rect){7, 11, 52, 30})) {
            browser_window_history_back(window->bw, false);
            redraw_pending = true;
        } else if (appui_inside(x, y, (struct appui_rect){63, 11, 48, 30})) {
            browser_window_reload(window->bw, false);
            redraw_pending = true;
        } else if (appui_inside(x, y,
                    (struct appui_rect){frame_width - 62, 11, 54, 30})) {
            navigate();
        } else if (appui_inside(x, y,
                    (struct appui_rect){117, 11, frame_width - 185, 30})) {
            address_focused = true;
        } else if (over_page) {
            address_focused = false;
            page_mouse_pressed = true;
            browser_window_mouse_click(window->bw, BROWSER_MOUSE_PRESS_1,
                                       x + window->scrollx,
                                       y - TOOLBAR_HEIGHT + window->scrolly);
            redraw_pending = true;
        }
    }
    if (released && page_mouse_pressed) {
        if (over_page) {
            browser_window_mouse_click(window->bw, BROWSER_MOUSE_CLICK_1,
                                       x + window->scrollx,
                                       y - TOOLBAR_HEIGHT + window->scrolly);
        } else {
            browser_window_mouse_track(window->bw, 0,
                                       x + window->scrollx,
                                       y - TOOLBAR_HEIGHT + window->scrolly);
        }
        page_mouse_pressed = false;
        redraw_pending = true;
    } else if (!pressed && !released && over_page) {
        browser_mouse_state state = (buttons & 1) ? BROWSER_MOUSE_HOLDING_1 : 0;
        browser_window_mouse_track(window->bw, state,
                                   x + window->scrollx,
                                   y - TOOLBAR_HEIGHT + window->scrolly);
    }
    previous_buttons = buttons;
}

int main(int argc, char **argv) {
    if (guiapp_parse_args(argc, argv, &application) < 0) {
        puts("netsurf: launch from the BuzzOS desktop Browser icon");
        return 1;
    }
    redirect_logs_to_serial();
    monkey_invalidate = monkey_window_table->invalidate;
    monkey_event = monkey_window_table->event;
    monkey_window_table->invalidate = buzzos_window_invalidate;
    monkey_window_table->event = buzzos_window_event;
    struct netsurf_table table = {
        .misc = &misc_table,
        .window = monkey_window_table,
        .download = monkey_download_table,
        .fetch = &fetch_table,
        .bitmap = monkey_bitmap_table,
        .layout = buzzos_layout_table,
        .llcache = filesystem_llcache_table,
    };
    if (netsurf_register(&table) != NSERROR_OK) return 2;
    nslog_init(log_stream, &argc, argv);
    if (nsoption_init(option_defaults, &nsoptions, &nsoptions_default) != NSERROR_OK) return 3;
    mkdir("/fs/netsurf-cache");
    if (netsurf_init("/fs/netsurf-cache") != NSERROR_OK) return 4;
    if (buzzos_png_init() != NSERROR_OK) return 5;
    if (buzzos_http_fetch_register() != NSERROR_OK) return 6;

    const char *welcome =
        "data:text/html,<html><head><style>body%7Bfont-family:sans-serif;"
        "background:%23ffffff;color:%23202b38;padding:24px%7D"
        "h1%7Bcolor:%23006fa8%7Dcode%7Bbackground:%23eef3f6;padding:3px%7D"
        "</style></head><body><h1>NetSurf%20on%20BuzzOS</h1>"
        "<p>The%20real%20NetSurf%20HTML%20and%20CSS%20engine%20is%20rendering%20this%20page.</p>"
        "<p>Enter%20an%20address%20or%20search%20term%20above.</p>"
        "<p><code>BuzzOS%20native%20frontend</code></p></body></html>";
    nsurl *url = NULL;
    if (nsurl_create(welcome, &url) != NSERROR_OK ||
        browser_window_create(BW_CREATE_HISTORY, url, NULL, NULL, NULL) != NSERROR_OK) {
        if (url) nsurl_unref(url);
        return 7;
    }
    nsurl_unref(url);
    pump_netsurf(300);
    address_length = (int)strlen(address_text);

    int event_thread = spawn(read_gui_events);
    if (event_thread < 0) return 8;
    for (;;) {
        bool handled_event = false;
        if (pending_event_state < 0) break;
        if (pending_event_state == 1) {
            __sync_synchronize();
            struct guiapp_event event = pending_event;
            pending_event_state = 0;
            futex_wake((int *)&pending_event_state, 1);
            if (event.type == GUIAPP_EVT_CLOSE) break;
            if (event.type == GUIAPP_EVT_INIT || event.type == GUIAPP_EVT_RESIZE) {
                frame_width = appui_max(320, appui_min(GUIAPP_MAX_W, event.width));
                frame_height = appui_max(220, appui_min(GUIAPP_MAX_H, event.height));
            } else if (event.type == GUIAPP_EVT_KEY && event.buttons)
                handle_key(event.key);
            else if (event.type == GUIAPP_EVT_TEXT) handle_text(event.text);
            else if (event.type == GUIAPP_EVT_MOUSE)
                handle_mouse(event.x, event.y, event.buttons, event.wheel);
            handled_event = true;
            redraw_pending = true;
        }
        pump_netsurf(50);
        if (redraw_pending) {
            redraw_pending = false;
            render_frame(&application);
        }
        if (!handled_event)
            sleep_ms(10);
    }
    struct gui_window *window = current_window();
    if (window) browser_window_destroy(window->bw);
    netsurf_exit();
    nsoption_finalise(nsoptions, nsoptions_default);
    nslog_finalise();
    return 0;
}
