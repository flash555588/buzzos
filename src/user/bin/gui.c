#include "libc.h"
#include "guiapp.h"
#include "palette.h"
#include "uikit.h"
#include "font_ui_display.h"
#include "app_identity.h"
#include "metro_tiles.h"
#include "ui_motion.h"
#include "gpucomp.h"
#include "pinyin_data.h"
#include "../../kernel/drv/font_builtin.h"

enum {
    KEY_ESC = 0x1B,
    KEY_BACKSPACE = 0x08,
    KEY_UP = 256,
    KEY_DOWN,
    KEY_RIGHT,
    KEY_LEFT,
    KEY_HOME,
    KEY_END,
    KEY_DELETE,
    KEY_WINDOW_CLOSE = 300,
    KEY_WORKSPACE,
    KEY_TASK_SWITCH,
    KEY_MAXIMIZE,
    KEY_MINIMIZE,
    KEY_SNAP_RIGHT,
    KEY_SNAP_LEFT,

    WIN_LAUNCHER = 0,
    WIN_STATUS = 1,
    WIN_APP_BASE = 2,
    MAX_GUI_APPS = 10,
    WIN_COUNT = WIN_APP_BASE + MAX_GUI_APPS,

    MAX_APPS = 16,
    APP_DEFAULT_W = 680,
    APP_DEFAULT_H = 430,
    APP_SURFACE_MAX_W = GUIAPP_MAX_W,
    APP_SURFACE_MAX_H = GUIAPP_MAX_H,
    MAX_SW = GUIAPP_MAX_W,
    MAX_SH = GUIAPP_MAX_H,
    DISPLAY_MODE_COUNT = 14,
    DISPLAY_MODE_COLS = 2,
    DISPLAY_BTN_H = 34,
    DISPLAY_BTN_GAP = 6,
    DISPLAY_GROUP_LABEL_H = 22,
    DISPLAY_GROUP_GAP = 14,
    /* The System pane's resolution section starts below the flowed text rows;
     * its Y is derived by status_res_head_y(), not hardcoded, so it cannot
     * collide with the last row when the UI font or the row count changes. */
    STATUS_TEXT_ROWS = 10,
    STATUS_SECTION_GAP = 10,
    /* Brand strip and floating dock frame the application workspace. */
    WORK_TOP = 72,
    WINDOW_TITLE_H = UI_TITLEBAR_H,
    TASKBAR_H = UI_TASKBAR_H,
    LAUNCHER_HEADER_H = 224,
    LAUNCHER_ROW_STEP = 144,
    LAUNCHER_ROW_H = 128,
    /* Taskbar buttons are icon-only and square-ish, as in the theme; the label
     * moves to the hover tooltip. */
    TB_BTN_W = UI_TASKBAR_BTN_W,
    TB_BTN_H = UI_TASKBAR_BTN_H,
    TB_ICON = UI_TASKBAR_ICON,
    TB_GAP = 4,
    TB_STEP = TB_BTN_W + TB_GAP,
    TB_TRAY_PAD = 8,
    TB_CLOCK_W = 92,
    TB_MAX_ITEMS = WIN_COUNT + 8,
    /* Start menu flyout. */
    START_W = 480,
    START_ROW_H = 72,
    START_PAD = 24,
    START_HEADER_H = 108,
    START_FOOTER_H = 72,
    CONTEXT_MENU_W = 150,
    CONTEXT_ITEM_STEP = 40,
    CONTEXT_ITEM_H = 36,
    CONTEXT_MENU_H = 128,
    WIN_MIN_W = 260,
    WIN_MIN_H = 170,
    RESIZE_PAD = 6,
    /* Software cursor sprite bounds (see draw_pointer).  Damage must cover
     * the full sprite or partial redraws leave trails. */
    POINTER_W = 16,
    POINTER_H = 16,
    POINTER_DAMAGE_PAD = 1,
};

struct rect {
    int x;
    int y;
    int w;
    int h;
};

struct display_mode {
    int width;
    int height;
    const char *label;  /* short button text, e.g. "1280x720" */
    const char *ratio;  /* aspect-ratio group, e.g. "16:9" */
};

/* Grouped by aspect ratio so the System panel can offer more than a single
 * 16:9 "xxxp" ladder.  All modes fit GUIAPP_MAX (1920x1200) and the FB cap. */
static const struct display_mode display_modes[DISPLAY_MODE_COUNT] = {
    {1280, 720, "1280x720", "16:9"},
    {1600, 900, "1600x900", "16:9"},
    {1920, 1080, "1920x1080", "16:9"},
    {1280, 800, "1280x800", "16:10"},
    {1440, 900, "1440x900", "16:10"},
    {1680, 1050, "1680x1050", "16:10"},
    {1920, 1200, "1920x1200", "16:10"},
    {1024, 768, "1024x768", "4:3"},
    {1280, 960, "1280x960", "4:3"},
    {1600, 1200, "1600x1200", "4:3"},
    {1280, 1024, "1280x1024", "5:4"},
    {800, 600, "800x600", "4:3"},
    {640, 960, "640x960", "2:3 portrait"},
    {800, 1200, "800x1200", "2:3 portrait"},
};

struct window {
    const char *title;
    const char *dock_label;
    /* Executable name, used to pick the taskbar/Start glyph.  The title is
     * app-controlled and can change at runtime, so it is not a stable key. */
    char app_name[24];
    struct rect r;
    struct rect restore;
    int visible;
    int active;
    int minimized;
    int maximized;
};

struct app_entry {
    char name[24];
    char path[64];
    uint32_t size;
};

struct app_session {
    int used;
    int pid;
    int to_fd;
    int from_fd;
    int surface_w;
    int surface_h;
    int source_w;
    int source_h;
    int scaled_surface;
    int scale_map_w;
    int scale_map_h;
    int scale_source_w;
    int scale_source_h;
    int want_w;
    int want_h;
    /* Written from the UI thread and the app reader thread. */
    volatile int resize_dirty;
    /* 1 after RESIZE/INIT sent until a frame arrives — paces configures to
     * the app's present rate (modern compositor in-flight limit). */
    volatile int resize_inflight;
    uint32_t last_resize_sent_ms;
    int gpu_sync_warned;
    int reader_tid;
    volatile int reader_dead;
    volatile int closing;
    /* Opt-in desktop heartbeat (System Monitor). Broadcasting TICK to every
     * app forced full redraws every 500 ms even when idle. */
    int wants_tick;
    uint32_t shm_token;
    struct guiapp_shared_surface *shared;
    uint32_t events_sent;
    int script_busy_visible;
    int input_overflow;
    uint32_t gpu_resource;
    uint32_t *gpu_pixels;
    int gpu_resource_w;
    int gpu_resource_h;
    int gpu_content_w;
    int gpu_content_h;
    int gpu_resource_canvas;
    int gpu_front_bank;
    int canvas_mode;
    uint16_t canvas_count;
    uint16_t canvas_string_bytes;
    struct guiapp_canvas_command canvas[GUIAPP_CANVAS_MAX_COMMANDS];
    char canvas_strings[GUIAPP_CANVAS_STRING_BYTES];
    volatile int dirty_lock;
    int dirty_valid;
    struct rect dirty_rect;
    uint32_t last_sequence;
    /* Content-local caret (app pixels); valid after GUIAPP_FRAME_CARET. */
    int caret_x;
    int caret_y;
    int caret_valid;
    uint16_t xmap[APP_SURFACE_MAX_W];
    uint16_t ymap[APP_SURFACE_MAX_H];
    char title[GUIAPP_TITLE_MAX];
};

static int sw;
static int sh;
static uint32_t display_backend;
/* Compose linear framebuffers offscreen; also used if GPU mapping fails. */
static uint32_t fb_local[MAX_SW * MAX_SH];
/* Compose target: GPU backing memory (explicit flush) or fb_local. */
static uint32_t *fb = fb_local;
static int fb_stride = MAX_SW; /* pixels per row in fb */
static int scanout_direct;     /* 1 = writing guest scanout memory */
static int running = 1;
static int display_acquired;
static struct window windows[WIN_COUNT];
static int z_order[WIN_COUNT];
static struct app_entry apps[MAX_APPS];
static struct app_session app_sessions[MAX_GUI_APPS];
static int app_count;
static int app_selected;
static int preferences_saved;
static int taskbar_hover = -1;
static int taskbar_expanded;
static int start_open;
static char start_query[64];
static int start_selected;
static int start_scroll;
enum { MOTION_FRAME_MS = 16, HOME_MOTION_MS = 220, HOME_STAGGER_MS = 14 };
static struct ui_motion search_motion;
static struct ui_motion window_motion[WIN_COUNT];
static int window_motion_pending[WIN_COUNT];
enum { WINDOW_EXIT_NONE, WINDOW_EXIT_MINIMIZE, WINDOW_EXIT_CLOSE };
static int window_exit[WIN_COUNT];
static struct ui_motion caption_motion[WIN_COUNT][3];
static struct ui_motion dock_hover_motion[WIN_COUNT + 4];
static struct ui_motion dock_press_motion[WIN_COUNT + 4];
static struct ui_motion dock_active_motion[WIN_COUNT + 4];
static struct ui_motion home_scroll_motion[2], result_scroll_motion;
static struct ui_motion tile_hover_motion[MAX_APPS];
static struct ui_motion tile_press_motion[MAX_APPS];
static uint32_t motion_frame_ms, home_motion_start;
static int home_motion_active, motion_home_visible, motion_pointer_down;
static int launcher_press = -1;
static void finish_window_exits(void);
static void refresh_pointer_hover_damage(void);
static int pointer_x;
static int pointer_y;
static int prev_buttons;
static int drag_win = -1;
static int drag_dx;
static int drag_dy;
static int scroll_drag_win = -1;
static int scroll_drag_axis;
static int scroll_drag_mouse;
static int scroll_drag_value;
static int resize_win = -1;
static int resize_edges;
static int resize_start_x;
static int resize_start_y;
static struct rect resize_start_rect;
static int scroll_x[WIN_COUNT];
static int scroll_y[WIN_COUNT];
static int focus = WIN_LAUNCHER;
static int app_mouse_capture = -1;
static int hover_app = -1;
/* Pointer-dependent chrome: paint uses live pointer_*, so damage must cover
 * the whole widget on enter/leave.  Cursor-sized damage alone leaves a
 * trail of 18x18 tiles that look like the highlight "paints in" slowly. */
static int hover_status_mode = -1;
static int hover_chrome_win = -1;
static int hover_chrome_ctl = -1;
static int hover_launcher_row = -1;
static int hover_start_tile = -1;
/* Last position actually composed into the backbuffer / scanned out.  Any
 * dirty-rect render must re-erase this spot; otherwise a later partial
 * update (common when skimming the app list's per-row hover damage) leaves
 * a ghost cursor. */
static int pointer_drawn_x;
static int pointer_drawn_y;
static int pointer_drawn_valid;
static int hardware_cursor_ready;
static int keyevent_fd = -1;
/* Releases follow the app that received the press, even while searching or
 * switching windows. A palette must not leave games with a stuck held key. */
static uint8_t key_owner[GUIAPP_KEY_COUNT];
static unsigned int tick;
static uint32_t last_app_tick_ms;
static uint32_t last_clock_second = (uint32_t)-1;
static volatile int desktop_dirty = 1;
static volatile uint32_t app_frame_dirty_mask;
static int gpu_present_ready;

static int find_caret_window(void);
static struct rect get_caret_area(void);
static struct rect pointer_damage_rect(int x, int y);
enum {
    SNAP_NONE = 0,
    SNAP_LEFT,
    SNAP_RIGHT,
    SNAP_MAX,
    SNAP_EDGE = 12,
};
static void draw_ime(void);
static void draw_snap_preview(void);
static int snap_zone_at(int x, int y);
static struct rect snap_target_rect(int zone);
static int bind_scanout(void);
static void gpu_present_init(void);
static void gpu_present_shutdown(void);

static uint32_t scaled_scanline[APP_SURFACE_MAX_W];
static struct rect compose_clip = {0, 0, MAX_SW, MAX_SH};
enum {
    COMPOSE_ALL = 0,
    COMPOSE_GPU_BASE,
    COMPOSE_GPU_OVERLAY,
};
/* The GPU shell is split into an opaque base and a straight-alpha overlay.
 * App pixels are sampled straight from imported SHM textures between them. */
static int compose_skip_app_pixels;
static int compose_pass;
static uint32_t *gpu_overlay_pixels;
static int gpu_overlay_stride;
static int gpu_blur_valid;
static struct rect pending_damage;
static int pending_damage_valid;
static int damage_lock;
static uint32_t last_wheel_seq;
static int last_wheel_value;
static int ime_enabled;
/* Composition buffer holds pure a-z pinyin (ü as v). */
enum {
    IME_BUF_CAP = 32,
    IME_CAND_CAP = 72,
    IME_PAGE_SIZE = 9,
    IME_MATCH_CAP = 96
};
static char ime_buffer[IME_BUF_CAP];
static int ime_length;
static int ime_page;
static int ime_cand_count;
static char ime_cands[IME_CAND_CAP][GUIAPP_TEXT_MAX];
/* How many leading pinyin letters each candidate consumes on commit. */
static uint8_t ime_cand_consume[IME_CAND_CAP];
static char clipboard[GUIAPP_PATH_MAX];
static int context_open;
static int context_x;
static int context_y;
static int context_target = -1;
static unsigned int mode_error_until;

static int min_i(int a, int b) { return a < b ? a : b; }
static int max_i(int a, int b) { return a > b ? a : b; }
static int clamp_i(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int inside(int x, int y, struct rect r) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static struct rect intersect_rect(struct rect a, struct rect b) {
    int x1 = max_i(a.x, b.x);
    int y1 = max_i(a.y, b.y);
    int x2 = min_i(a.x + a.w, b.x + b.w);
    int y2 = min_i(a.y + a.h, b.y + b.h);
    if (x2 <= x1 || y2 <= y1)
        return (struct rect){0, 0, 0, 0};
    return (struct rect){x1, y1, x2 - x1, y2 - y1};
}

/* All fill/pixel paths honor compose_clip.  Window body drawing must push the
 * content rect so widgets that ignore an explicit text clip (buttons, rounded
 * fills) cannot paint into the title bar, chrome, or neighboring desktop. */
static struct rect compose_clip_push(struct rect limit) {
    struct rect previous = compose_clip;
    compose_clip = intersect_rect(compose_clip, limit);
    return previous;
}

static void compose_clip_pop(struct rect previous) {
    compose_clip = previous;
}

static struct rect union_rect(struct rect a, struct rect b) {
    if (a.w <= 0 || a.h <= 0) return b;
    if (b.w <= 0 || b.h <= 0) return a;
    int x1 = min_i(a.x, b.x);
    int y1 = min_i(a.y, b.y);
    int x2 = max_i(a.x + a.w, b.x + b.w);
    int y2 = max_i(a.y + a.h, b.y + b.h);
    return (struct rect){x1, y1, x2 - x1, y2 - y1};
}

static void queue_damage(struct rect area) {
    while (__sync_lock_test_and_set(&damage_lock, 1))
        yield();
    area = intersect_rect(area, (struct rect){0, 0, sw, sh});
    if (area.w > 0 && area.h > 0) {
        pending_damage = pending_damage_valid
            ? union_rect(pending_damage, area) : area;
        pending_damage_valid = 1;
    }
    __sync_lock_release(&damage_lock);
}

static int take_damage(struct rect *out) {
    int valid;
    while (__sync_lock_test_and_set(&damage_lock, 1))
        yield();
    valid = pending_damage_valid;
    if (valid) {
        *out = pending_damage;
        pending_damage_valid = 0;
        pending_damage = (struct rect){0, 0, 0, 0};
    }
    __sync_lock_release(&damage_lock);
    return valid;
}

/* Expand a rect to cover the drop shadow drawn around it.
 *
 * The pre-theme shadow was two hard bands extending 6px right and down only,
 * so damage of (w+6, h+6) covered it.  The themed shadow is a soft blur that
 * spreads UI_ELEV_FLYOUT_R in *every* direction, offset downward -- damaging
 * only right and down leaves the left and top fringe unpainted, which is what
 * smears a shadow trail across the wallpaper during a drag.  Every damage site
 * that covers a shadow goes through here so the margin cannot drift from the
 * blur radius again. */
enum { WIN_SHADOW_PAD = UI_ELEV_FLYOUT_R + 6 };

static struct rect shadow_bounds(struct rect r) {
    return (struct rect){r.x - WIN_SHADOW_PAD, r.y - WIN_SHADOW_PAD,
                         r.w + 2 * WIN_SHADOW_PAD,
                         r.h + 2 * WIN_SHADOW_PAD};
}

/* Damage a window including its drop shadow. */
/* Presentation-only translation: layout, hit tests and app configures retain
 * their final geometry. Both software pixels and imported GPU layers use it. */
static struct rect motion_paint_rect(int id, struct rect r) {
    r.y += window_motion[id].value;
    return r;
}

static void window_motion_cancel(int id) {
    struct rect old = motion_paint_rect(id, windows[id].r);
    ui_motion_reset(&window_motion[id], 0);
    window_motion_pending[id] = 0;
    window_exit[id] = WINDOW_EXIT_NONE;
    queue_damage(union_rect(shadow_bounds(old), shadow_bounds(windows[id].r)));
}

static void motion_cancel_all(void) {
    finish_window_exits();
    for (int id = 0; id < WIN_COUNT; id++) window_motion_cancel(id);
    for (int i = 0; i < MAX_APPS; i++) {
        ui_motion_reset(&tile_hover_motion[i], 0);
        ui_motion_reset(&tile_press_motion[i], 0);
    }
    ui_motion_reset(&search_motion, start_open ? 255 : 0);
    home_motion_active = 0;
    motion_home_visible = focus == WIN_LAUNCHER;
    launcher_press = -1;
    for (int id = 0; id < WIN_COUNT; id++)
        for (int control = 0; control < 3; control++)
            ui_motion_reset(&caption_motion[id][control], 0);
    for (int i = 0; i < WIN_COUNT + 4; i++) {
        ui_motion_reset(&dock_hover_motion[i], 0);
        ui_motion_reset(&dock_press_motion[i], 0);
        ui_motion_reset(&dock_active_motion[i], 0);
    }
    ui_motion_reset(&home_scroll_motion[0], scroll_x[WIN_LAUNCHER]);
    ui_motion_reset(&home_scroll_motion[1], scroll_y[WIN_LAUNCHER]);
    ui_motion_reset(&result_scroll_motion, start_scroll);
}

static void win_damage(int id) {
    if (id < 0 || id >= WIN_COUNT)
        return;
    queue_damage(union_rect(shadow_bounds(windows[id].r),
                           shadow_bounds(motion_paint_rect(id, windows[id].r))));
}

/* Compact applications use all space above the taskbar. Larger displays retain
 * the workspace heading above floating, maximised and snapped windows. */
static struct rect work_area(void) {
    int top = sw < 1000 ? 0 : WORK_TOP;
    return (struct rect){0, top, sw, sh - top - TASKBAR_H};
}

/* Damage the taskbar plus the tooltip and overflow-flyout area above it.
 *
 * App reader threads call this concurrently, so it derives its bounds from
 * constants rather than from shared cached geometry: the taskbar recentres as
 * apps open and close, and repainting the whole bottom band also clears the
 * old tooltip and flyout in one step. */
static void taskbar_damage(void) {
    int overflow_h = 12 + MAX_GUI_APPS * 40 + 8;
    int top = sh - TASKBAR_H - overflow_h - UI_ELEV_FLYOUT_R;
    if (top < 0)
        top = 0;
    queue_damage((struct rect){0, top, sw, sh - top});
}

/* Damage the IME tray badge and the candidate panel area. */
static void ime_damage(void) {
    taskbar_damage();
    /* Cover previous and next caret-adjacent panel positions. */
    if (ime_enabled && ime_length > 0) {
        struct rect caret = get_caret_area();
        int pad = 48;
        int panel_h = 72;
        int x = max_i(0, caret.x - pad);
        int y = max_i(0, caret.y - panel_h - pad);
        int w = min_i(sw - x, 480 + 2 * pad);
        int h = min_i(sh - y, panel_h + caret.h + 2 * pad + 24);
        queue_damage((struct rect){x, y, w, h});
    }
}

static void app_dirty_lock(int slot) {
    while (__sync_lock_test_and_set(&app_sessions[slot].dirty_lock, 1))
        yield();
}

static void app_dirty_unlock(int slot) {
    __sync_lock_release(&app_sessions[slot].dirty_lock);
}

static void app_note_dirty(int slot, struct rect area) {
    if (slot < 0 || slot >= MAX_GUI_APPS || area.w <= 0 || area.h <= 0)
        return;
    app_dirty_lock(slot);
    app_sessions[slot].dirty_rect = app_sessions[slot].dirty_valid
        ? union_rect(app_sessions[slot].dirty_rect, area) : area;
    app_sessions[slot].dirty_valid = 1;
    app_dirty_unlock(slot);
    __sync_fetch_and_or(&app_frame_dirty_mask, 1u << slot);
}

static int app_take_dirty(int slot, struct rect *out) {
    int valid;
    app_dirty_lock(slot);
    valid = app_sessions[slot].dirty_valid;
    if (valid) {
        *out = app_sessions[slot].dirty_rect;
        app_sessions[slot].dirty_valid = 0;
        app_sessions[slot].dirty_rect = (struct rect){0, 0, 0, 0};
    }
    app_dirty_unlock(slot);
    return valid;
}

static void copy_text(char *dst, const char *src, size_t cap) {
    size_t i = 0;
    if (!cap)
        return;
    while (i + 1 < cap && src && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static int app_target_allowed(const char *path) {
    for (int i = 0; i < app_count; i++)
        if (strcmp(path, apps[i].path) == 0)
            return 1;
    return 0;
}

static int exec_target_allowed(const char *path) {
    if (!path || path[0] != '/' || !path[1])
        return 0;
    for (int i = 1; path[i]; i++) {
        unsigned char ch = (unsigned char)path[i];
        int safe = (ch >= 'a' && ch <= 'z') ||
                   (ch >= 'A' && ch <= 'Z') ||
                   (ch >= '0' && ch <= '9') ||
                   ch == '/' || ch == '.' || ch == '_' || ch == '-';
        if (!safe)
            return 0;
    }
    struct stat st;
    if (stat(path, &st) < 0 || st.st_type != DT_REG)
        return 0;
    uint8_t magic[4];
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    int n = read(fd, magic, sizeof(magic));
    close(fd);
    return n == 4 && magic[0] == 0x7Fu && magic[1] == 'E' &&
           magic[2] == 'L' && magic[3] == 'F';
}

static void append_text(char *dst, const char *src, size_t cap) {
    size_t n = strlen(dst);
    size_t i = 0;
    while (n + 1 < cap && src && src[i])
        dst[n++] = src[i++];
    if (cap)
        dst[n] = 0;
}

static void append_uint(char *dst, unsigned int v, size_t cap) {
    char tmp[16];
    int i = 0;
    if (v == 0) {
        append_text(dst, "0", cap);
        return;
    }
    while (v && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0) {
        char s[2];
        s[0] = tmp[--i];
        s[1] = 0;
        append_text(dst, s, cap);
    }
}

static void int_to_dec(int value, char *dst, size_t cap) {
    char tmp[16];
    unsigned int v;
    int n = 0;
    int pos = 0;
    if (!cap)
        return;
    if (value < 0) {
        dst[pos++] = '-';
        v = (unsigned int)(-value);
    } else {
        v = (unsigned int)value;
    }
    if (v == 0)
        tmp[n++] = '0';
    while (v && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0 && pos + 1 < (int)cap)
        dst[pos++] = tmp[--n];
    dst[pos] = 0;
}

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

/* Bridge to the uikit rendering kernel.
 *
 * uikit draws into a surface it clips itself, so handing it compose_clip on
 * every call keeps the shell's existing damage-driven clipping authoritative:
 * there is one clip rect, not two that could drift apart.  The scanout may be
 * wider than the visible width, hence the explicit stride. */
static struct ui_surface ui_target(void) {
    struct ui_surface s = compose_pass == COMPOSE_GPU_OVERLAY
        ? ui_surface_alpha_stride(fb, sw, sh, fb_stride)
        : ui_surface_stride(fb, sw, sh, fb_stride);
    s.clip = ui_rect_intersect(s.clip,
                               ui_rect_make(compose_clip.x, compose_clip.y,
                                            compose_clip.w, compose_clip.h));
    return s;
}

static int shell_acrylic(struct ui_surface *s, struct ui_rect r, int radius,
                          uint32_t tint, int tint_alpha) {
    /* The GPU overlay contains only controls, strokes, text and shadows.  Its
     * acrylic backdrop is generated later from the completed scene texture. */
    if (compose_pass == COMPOSE_GPU_OVERLAY)
        return 0;
    return ui_acrylic(s, r, radius, tint, tint_alpha);
}

static struct ui_rect ui_of(struct rect r) {
    return ui_rect_make(r.x, r.y, r.w, r.h);
}

/* Map an application to a chrome glyph.  Matching on the executable name
 * keeps this table the only place that knows about specific apps; anything
 * unrecognised falls back to a generic window tile rather than going blank. */
static int app_icon_for(const char *name) {
    static const struct {
        const char *name;
        uint8_t icon;
    } table[] = {
        {"terminal", UI_ICON_TERMINAL},
        {"taskmanager", UI_ICON_CHART},
        {"textedit", UI_ICON_DOCUMENT},
        {"paint", UI_ICON_IMAGE},
        {"calculator", UI_ICON_CALCULATOR},
        {"filemanager", UI_ICON_FOLDER},
        {"browser", UI_ICON_GLOBE},
        {"doom", UI_ICON_GAMEPAD},
        {"gameboy", UI_ICON_GAMEPAD},
        {"music", UI_ICON_MUSIC},
        {"luaide", UI_ICON_CODE},
    };
    if (!name)
        return UI_ICON_DOCUMENT;
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        const char *a = table[i].name;
        const char *b = name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*a && !*b)
            return table[i].icon;
    }
    return UI_ICON_DOCUMENT;
}

static int window_icon(int id) {
    if (id == WIN_LAUNCHER)
        return UI_ICON_GRID;
    if (id == WIN_STATUS)
        return UI_ICON_SETTINGS;
    return app_icon_for(windows[id].app_name);
}

static void fill(struct rect r, int color) {
    uint32_t c = ((uint32_t)color & 0x00FFFFFFu) |
                 (compose_pass == COMPOSE_GPU_OVERLAY ? 0xFF000000u : 0u);
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > sw) r.w = sw - r.x;
    if (r.y + r.h > sh) r.h = sh - r.y;
    r = intersect_rect(r, compose_clip);
    if (r.w <= 0 || r.h <= 0)
        return;
    for (int yy = 0; yy < r.h; yy++) {
        uint32_t *row = fb + (r.y + yy) * fb_stride + r.x;
        for (int xx = 0; xx < r.w; xx++)
            row[xx] = c;
    }
}

static void pixel(int x, int y, int color) {
    if (x >= 0 && y >= 0 && x < sw && y < sh &&
        inside(x, y, compose_clip))
        fb[y * fb_stride + x] =
            ((uint32_t)color & 0x00FFFFFFu) |
            (compose_pass == COMPOSE_GPU_OVERLAY ? 0xFF000000u : 0u);
}

static uint32_t gui_utf8_next(const char **text) {
    const uint8_t *s = (const uint8_t *)*text;
    uint32_t cp;
    int extra;
    if (s[0] < 0x80u) { *text = (const char *)(s + 1); return s[0]; }
    if (s[0] >= 0xC2u && s[0] <= 0xDFu) { cp = s[0] & 0x1Fu; extra = 1; }
    else if (s[0] >= 0xE0u && s[0] <= 0xEFu) { cp = s[0] & 0x0Fu; extra = 2; }
    else if (s[0] >= 0xF0u && s[0] <= 0xF4u) { cp = s[0] & 7u; extra = 3; }
    else { *text = (const char *)(s + 1); return 0xFFFDu; }
    for (int i = 1; i <= extra; i++) {
        if (!s[i] || (s[i] & 0xC0u) != 0x80u) {
            *text = (const char *)(s + 1); return 0xFFFDu;
        }
        cp = (cp << 6) | (s[i] & 0x3Fu);
    }
    if ((extra == 2 && cp < 0x800u) || (extra == 3 && cp < 0x10000u) ||
        (cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu) {
        *text = (const char *)(s + 1); return 0xFFFDu;
    }
    *text = (const char *)(s + extra + 1);
    return cp;
}

enum { GUI_TAB_COLUMNS = 4 };

static int gui_tab_width(void) { return KFONT_WIDTH * GUI_TAB_COLUMNS; }

static int gui_tab_advance(int x_from_line_start) {
    int tab = gui_tab_width();
    if (tab <= 0)
        return KFONT_WIDTH;
    if (x_from_line_start < 0)
        x_from_line_start = 0;
    int advance = tab - (x_from_line_start % tab);
    return advance <= 0 ? tab : advance;
}

static int gui_codepoint_width(uint32_t cp) {
    if (cp == '\t')
        return gui_tab_width();
    if (cp == '\r' || cp == '\n')
        return 0;
    if (cp < 0x80u)
        return KFONT_WIDTH;
    uint8_t bits[FONT_GLYPH_BYTES];
    int width = font_glyph(cp, bits, sizeof(bits));
    return width > 0 ? width : KFONT_WIDTH;
}

static int gui_codepoint_advance(uint32_t cp, int x_from_line_start) {
    if (cp == '\t')
        return gui_tab_advance(x_from_line_start);
    return gui_codepoint_width(cp);
}

/* Themed button: a filled rounded rect with a hairline stroke, accent-filled
 * when it is the default action.  Accent fills use the light end of the ramp
 * with black text, which is what makes a the theme primary button read as bright
 * rather than navy. */
static void button_state(struct rect r, const char *label, int active,
                         int disabled) {
    struct ui_surface s = ui_target();
    struct ui_rect box = ui_of(r);
    int hovered = !disabled && inside(pointer_x, pointer_y, r);
    int pressed = hovered && (prev_buttons & 1);
    uint32_t bg, edge, fg;
    struct ui_text_style ts;

    if (disabled) {
        bg = UI_CTRL_DISABLED;
        edge = UI_STROKE_CONTROL;
        fg = UI_TEXT_DISABLED;
    } else if (active) {
        bg = pressed ? UI_ACCENT_FILL_PRESS
                     : (hovered ? UI_ACCENT_FILL_HOVER : UI_ACCENT_FILL);
        edge = UI_ACCENT_DARK1;
        fg = UI_TEXT_ON_ACCENT;
    } else {
        bg = pressed ? UI_CTRL_PRESSED
                     : (hovered ? UI_CTRL_HOVER : UI_CTRL_REST);
        edge = UI_STROKE_CONTROL;
        fg = pressed ? UI_TEXT_SECONDARY : UI_TEXT_PRIMARY;
    }

    ui_fill_round(&s, box, UI_RADIUS_CONTROL, bg);
    ui_stroke_round(&s, box, UI_RADIUS_CONTROL, 1, edge, 255);
    ts = ui_style(UI_FONT_BODY, fg);
    ts.align = UI_ALIGN_CENTER;
    ui_text_in(&s, ui_rect_inset(box, 6), label, ts);
}

static void button(struct rect r, const char *label, int active) {
    button_state(r, label, active, 0);
}

static int read_raw_poll(void) {
    unsigned char c;
    int n = read(0, &c, 1);
    if (n > 0)
        return c;
    return -1;
}

static int read_key_poll(void) {
    int c = read_raw_poll();
    if (c < 0)
        return -1;
    if (c != KEY_ESC)
        return c;
    int c1 = -1;
    for (int i = 0; i < 8 && c1 < 0; i++) {
        c1 = read_raw_poll();
        if (c1 < 0)
            yield();
    }
    if (c1 != '[')
        return KEY_ESC;
    int c2 = -1;
    for (int i = 0; i < 8 && c2 < 0; i++) {
        c2 = read_raw_poll();
        if (c2 < 0)
            yield();
    }
    switch (c2) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case '+': return GUIAPP_KEY_ZOOM_IN;
    case '-': return GUIAPP_KEY_ZOOM_OUT;
    case '0': return GUIAPP_KEY_ZOOM_RESET;
    case '3':
        (void)read_raw_poll(); /* Delete's terminating '~'. */
        return KEY_DELETE;
    case 'Q': return KEY_WINDOW_CLOSE;
    case 'W': return KEY_WORKSPACE;
    case 'T': return KEY_TASK_SWITCH;
    case 'a': return KEY_MAXIMIZE;
    case 'b': return KEY_MINIMIZE;
    case 'c': return KEY_SNAP_RIGHT;
    case 'd': return KEY_SNAP_LEFT;
    default:
        /* Consume an unsupported CSI without mistaking it for Escape. */
        for (int i = 0; i < 12 && c2 >= 0 && c2 < 0x40; i++)
            c2 = read_raw_poll();
        return 299;
    }
}

static int max_scroll_y(int id);
static struct rect content_rect(int id);
static int status_resolution_bottom(void);
static struct rect launcher_row_paint_rect(int index);
static int hit_launcher_row_at(int x, int y);
static int top_window_at(int x, int y);
static void close_window(int id);
static void clamp_scroll(int id);
static void activate(int id);
static void update_hover_app(int force);
static void run_app_with_arg(const char *path, const char *argument);
static void save_preferences(void);
static void gui_log(const char *message) {
    puts(message);
}

static void scan_apps(void) {
    app_count = 0;
    (void)mkdir("/fs/apps");
    int fd = open("/fs/apps", O_RDONLY);
    if (fd < 0)
        return;
    struct dirent ents[8];
    for (;;) {
        int n = getdents(fd, ents, sizeof(ents));
        if (n <= 0)
            break;
        int entries = n / (int)sizeof(ents[0]);
        for (int i = 0; i < entries && app_count < MAX_APPS; i++) {
            if (ents[i].d_type != DT_REG)
                continue;
            int executable = 1;
            for (int j = 0; ents[i].d_name[j]; j++) {
                if (ents[i].d_name[j] == '.') {
                    executable = 0;
                    break;
                }
            }
            if (!executable)
                continue;
            char app_path[64];
            char manifest_path[72];
            struct stat manifest_st;
            copy_text(app_path, "/fs/apps/", sizeof(app_path));
            append_text(app_path, ents[i].d_name, sizeof(app_path));
            copy_text(manifest_path, app_path, sizeof(manifest_path));
            append_text(manifest_path, ".app", sizeof(manifest_path));
            if (stat(manifest_path, &manifest_st) < 0 || manifest_st.st_type != DT_REG)
                continue;
            copy_text(apps[app_count].name, ents[i].d_name, sizeof(apps[app_count].name));
            copy_text(apps[app_count].path, app_path, sizeof(apps[app_count].path));
            apps[app_count].size = ents[i].d_size;
            app_count++;
        }
    }
    close(fd);
    if (app_selected >= app_count)
        app_selected = app_count > 0 ? app_count - 1 : 0;
}

static int app_slot_for_win(int id) {
    int slot = id - WIN_APP_BASE;
    return slot >= 0 && slot < MAX_GUI_APPS ? slot : -1;
}

/* Keep the shell out of a blocking write when the browser's renderer cannot
 * consume input. 64 complete events fit well inside the 8192-byte pipe.
 * The bounded backlog retains input; further input is rejected while full.
 * Stop/close is signalled separately and remains available even then. */
static int app_event_has_capacity(int slot) {
    struct app_session *app = &app_sessions[slot];
    if (!app->shared || !app->shared->script_control_enabled) return 1;
    uint32_t consumed = __atomic_load_n(&app->shared->events_consumed, __ATOMIC_ACQUIRE);
    return (uint32_t)(app->events_sent - consumed) < 64;
}

static int app_send_event(int slot, int type, int x, int y, int key, int buttons, int wheel) {
    if (slot < 0 || slot >= MAX_GUI_APPS || !app_sessions[slot].used)
        return -1;
    struct guiapp_shared_surface *shared = app_sessions[slot].shared;
    if (shared && shared->script_control_enabled &&
        (type == GUIAPP_EVT_CLOSE ||
         (type == GUIAPP_EVT_KEY && buttons && shared->script_running &&
          (key == GUIAPP_KEY_ESC || key == 18)))) {
        shared->script_control_key = type == GUIAPP_EVT_CLOSE ? 0 : (uint32_t)key;
        __atomic_add_fetch(&shared->script_cancel_sequence, 1, __ATOMIC_RELEASE);
        /* Escape belongs to the browser stop action while script is busy,
         * never to a later page key event. Reload is also out of band and
         * is consumed by the renderer's main loop after it unwinds. */
        if (type == GUIAPP_EVT_KEY) return 0;
    }
    if (!app_event_has_capacity(slot)) {
        app_sessions[slot].input_overflow = 1;
        return 0;
    }
    if (app_sessions[slot].input_overflow && type != GUIAPP_EVT_INPUT_RESET) {
        app_sessions[slot].input_overflow = 0;
        app_send_event(slot, GUIAPP_EVT_INPUT_RESET, 0, 0, 0, 0, 0);
        if (!app_event_has_capacity(slot)) {
            app_sessions[slot].input_overflow = 1;
            return 0;
        }
    }
    struct guiapp_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.magic = GUIAPP_MAGIC;
    ev.type = (uint32_t)type;
    ev.width = app_sessions[slot].want_w;
    ev.height = app_sessions[slot].want_h;
    ev.x = x;
    ev.y = y;
    ev.key = key;
    ev.buttons = buttons;
    ev.wheel = wheel;
    int result = write_full(app_sessions[slot].to_fd, &ev, (int)sizeof(ev));
    if (result == 0) app_sessions[slot].events_sent++;
    return result;
}

static int app_send_text(int slot, const char *value) {
    if (slot < 0 || slot >= MAX_GUI_APPS || !app_sessions[slot].used || !value)
        return -1;
    if (!app_event_has_capacity(slot)) {
        app_sessions[slot].input_overflow = 1;
        return 0;
    }
    if (app_sessions[slot].input_overflow) {
        app_sessions[slot].input_overflow = 0;
        app_send_event(slot, GUIAPP_EVT_INPUT_RESET, 0, 0, 0, 0, 0);
        if (!app_event_has_capacity(slot)) {
            app_sessions[slot].input_overflow = 1;
            return 0;
        }
    }
    struct guiapp_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.magic = GUIAPP_MAGIC;
    ev.type = GUIAPP_EVT_TEXT;
    ev.width = app_sessions[slot].want_w;
    ev.height = app_sessions[slot].want_h;
    copy_text(ev.text, value, sizeof(ev.text));
    int result = write_full(app_sessions[slot].to_fd, &ev, (int)sizeof(ev));
    if (result == 0) app_sessions[slot].events_sent++;
    return result;
}

static int app_handle_escape(int id) {
    int slot = app_slot_for_win(id);
    if (slot < 0 || !app_sessions[slot].used || !app_sessions[slot].shared ||
        !app_sessions[slot].shared->script_control_enabled) return 0;
    app_send_event(slot, GUIAPP_EVT_KEY, 0, 0, GUIAPP_KEY_ESC, 1, 0);
    key_owner[GUIAPP_KEY_ESC] = slot + 1;
    return 1;
}

static void app_target_size(int id, int *tw, int *th);

static int app_read_frame(int slot) {
    if (slot < 0 || slot >= MAX_GUI_APPS || !app_sessions[slot].used)
        return -1;
    struct guiapp_frame frame;
    for (;;) {
        if (read_full(app_sessions[slot].from_fd, &frame, (int)sizeof(frame)) < 0)
            return -1;
        if (frame.magic != GUIAPP_MAGIC)
            return -1;
        frame.target[GUIAPP_PATH_MAX - 1] = 0;
        frame.argument[GUIAPP_PATH_MAX - 1] = 0;
        if (frame.type == GUIAPP_FRAME_CLIPBOARD) {
            copy_text(clipboard, frame.argument, sizeof(clipboard));
            if (context_open)
                queue_damage((struct rect){context_x, context_y,
                                           CONTEXT_MENU_W, CONTEXT_MENU_H});
            (void)gui_event_signal();
            continue;
        }
        if (frame.type == GUIAPP_FRAME_CARET) {
            int old_x = app_sessions[slot].caret_x;
            int old_y = app_sessions[slot].caret_y;
            app_sessions[slot].caret_x = frame.x;
            app_sessions[slot].caret_y = frame.y;
            app_sessions[slot].caret_valid = 1;
            if (focus == WIN_APP_BASE + slot && ime_enabled && ime_length > 0 &&
                (old_x != frame.x || old_y != frame.y))
                ime_damage();
            (void)gui_event_signal();
            continue;
        }
        if (frame.type == GUIAPP_FRAME_EXEC) {
            if (exec_target_allowed(frame.target))
                run_app_with_arg("/fs/apps/terminal", frame.target);
            else
                gui_log("[gui] exec request rejected");
            (void)gui_event_signal();
            continue;
        }
        if (frame.type != GUIAPP_FRAME_LAUNCH)
            break;
        if (app_target_allowed(frame.target))
            run_app_with_arg(frame.target, frame.argument[0] ? frame.argument : 0);
        else
            gui_log("[gui] launch request rejected");
        (void)gui_event_signal();
    }
    if (frame.width <= 0 || frame.height <= 0 ||
        frame.width > APP_SURFACE_MAX_W || frame.height > APP_SURFACE_MAX_H)
        return -1;
    if ((frame.type != GUIAPP_FRAME_FULL &&
         frame.type != GUIAPP_FRAME_DIRTY &&
         frame.type != GUIAPP_FRAME_SCALED &&
         frame.type != GUIAPP_FRAME_CANVAS) ||
        !app_sessions[slot].shared)
        return -1;
    frame.title[GUIAPP_TITLE_MAX - 1] = 0;
    if ((frame.type == GUIAPP_FRAME_SCALED ||
         frame.type == GUIAPP_FRAME_DIRTY) &&
        (frame.dirty_w <= 0 || frame.dirty_h <= 0 ||
         frame.dirty_w > APP_SURFACE_MAX_W || frame.dirty_h > APP_SURFACE_MAX_H))
        return -1;
    int scaled = frame.type == GUIAPP_FRAME_SCALED;
    int canvas = frame.type == GUIAPP_FRAME_CANVAS;
    if (canvas &&
        !(app_sessions[slot].shared->capabilities & GUIAPP_CAP_GPU_CANVAS))
        return 0;
    int source_w = scaled ? frame.dirty_w : frame.width;
    int source_h = scaled ? frame.dirty_h : frame.height;
    if ((uint32_t)source_w >
        app_sessions[slot].shared->capacity_pixels / (uint32_t)source_h)
        return -1;
    uint32_t shared_sequence = app_sessions[slot].shared->sequence;
    if ((shared_sequence & 1u) || shared_sequence != frame.sequence)
        return 0; /* A newer notification in the pipe describes the surface. */
    if (app_sessions[slot].shared->width != (uint32_t)source_w ||
        app_sessions[slot].shared->height != (uint32_t)source_h)
        return -1;
    if (canvas) {
        uint16_t count = app_sessions[slot].shared->canvas_count;
        uint16_t strings = app_sessions[slot].shared->canvas_string_bytes;
        if (count > GUIAPP_CANVAS_MAX_COMMANDS ||
            strings > GUIAPP_CANVAS_STRING_BYTES)
            return -1;
        app_dirty_lock(slot);
        memcpy(app_sessions[slot].canvas,
               app_sessions[slot].shared->canvas,
               (size_t)count * sizeof(app_sessions[slot].canvas[0]));
        memcpy(app_sessions[slot].canvas_strings,
               app_sessions[slot].shared->canvas_strings, strings);
        __sync_synchronize();
        if (app_sessions[slot].shared->sequence != frame.sequence) {
            app_dirty_unlock(slot);
            return 0;
        }
        app_sessions[slot].canvas_count = count;
        app_sessions[slot].canvas_string_bytes = strings;
        app_dirty_unlock(slot);
    }
    if (frame.type == GUIAPP_FRAME_DIRTY &&
        (app_sessions[slot].scaled_surface ||
         app_sessions[slot].surface_w != frame.width ||
         app_sessions[slot].surface_h != frame.height ||
         frame.x < 0 || frame.y < 0 ||
         frame.x + frame.dirty_w > frame.width ||
         frame.y + frame.dirty_h > frame.height))
        return -1;
    int title_changed = strcmp(app_sessions[slot].title, frame.title) != 0;
    int full_change = app_sessions[slot].surface_w != frame.width ||
        app_sessions[slot].surface_h != frame.height ||
        app_sessions[slot].scaled_surface != scaled ||
        app_sessions[slot].canvas_mode != canvas ||
        app_sessions[slot].source_w != source_w ||
        app_sessions[slot].source_h != source_h ||
        title_changed;
    app_sessions[slot].surface_w = frame.width;
    app_sessions[slot].surface_h = frame.height;
    app_sessions[slot].scaled_surface = scaled;
    app_sessions[slot].canvas_mode = canvas;
    __sync_synchronize();
    if (canvas &&
        !(app_sessions[slot].shared->capabilities & GUIAPP_CAP_GPU_CANVAS)) {
        app_sessions[slot].canvas_mode = 0;
        app_sessions[slot].surface_w = 0;
        app_sessions[slot].surface_h = 0;
        return 0;
    }
    app_sessions[slot].source_w = source_w;
    app_sessions[slot].source_h = source_h;
    app_sessions[slot].last_sequence = frame.sequence;
    /* App presented — allow the next live-resize configure. */
    app_sessions[slot].resize_inflight = 0;
    copy_text(app_sessions[slot].title, frame.title, sizeof(app_sessions[slot].title));
    windows[WIN_APP_BASE + slot].title = app_sessions[slot].title[0]
        ? app_sessions[slot].title : "Application";
    if (title_changed && focus == WIN_APP_BASE + slot)
        taskbar_damage();
    clamp_scroll(WIN_APP_BASE + slot);
    {
        int tw;
        int th;
        app_target_size(WIN_APP_BASE + slot, &tw, &th);
        if (tw != app_sessions[slot].want_w || th != app_sessions[slot].want_h)
            app_sessions[slot].resize_dirty = 1;
    }
    if (full_change) {
        /* Title changes affect the Dock label; size-only frames must not
         * thrash the whole dock.  While the user is live-dragging this
         * window's edge, the mouse path already damages the chrome — only
         * mark the content dirty so we do not fight a second full redraw. */
        if (title_changed)
            taskbar_damage();
        if (gpu_present_ready || resize_win == WIN_APP_BASE + slot)
            app_note_dirty(slot, (struct rect){0, 0, source_w, source_h});
        if (resize_win != WIN_APP_BASE + slot)
            win_damage(WIN_APP_BASE + slot);
    } else {
        struct rect dirty = frame.type == GUIAPP_FRAME_DIRTY
            ? (struct rect){frame.x, frame.y, frame.dirty_w, frame.dirty_h}
            : (struct rect){0, 0, source_w, source_h};
        app_note_dirty(slot, dirty);
    }
    return 0;
}

static void app_reader_loop(int slot) {
    while (app_sessions[slot].used && !app_sessions[slot].closing) {
        if (app_read_frame(slot) < 0)
            break;
        /* app_read_frame runs on a pipe-reader thread.  Publish only after
         * its dirty state is visible so the sleeping compositor cannot miss
         * a completed frame. */
        (void)gui_event_signal();
    }
    if (!app_sessions[slot].closing) {
        app_sessions[slot].reader_dead = 1;
        win_damage(WIN_APP_BASE + slot);
        taskbar_damage();
        (void)gui_event_signal();
    }
}

#define APP_READER_WRAPPER(n) static void app_reader_##n(void) { app_reader_loop(n); }
APP_READER_WRAPPER(0) APP_READER_WRAPPER(1) APP_READER_WRAPPER(2)
APP_READER_WRAPPER(3) APP_READER_WRAPPER(4) APP_READER_WRAPPER(5)
APP_READER_WRAPPER(6) APP_READER_WRAPPER(7) APP_READER_WRAPPER(8)
APP_READER_WRAPPER(9)

static thread_fn app_reader_functions[MAX_GUI_APPS] = {
    app_reader_0, app_reader_1, app_reader_2, app_reader_3, app_reader_4,
    app_reader_5, app_reader_6, app_reader_7, app_reader_8, app_reader_9
};

static void app_target_size(int id, int *tw, int *th) {
    struct rect c = content_rect(id);
    *tw = clamp_i(c.w, 180, APP_SURFACE_MAX_W);
    *th = clamp_i(c.h, 140, APP_SURFACE_MAX_H);
}

/* Publish latest content size into SHM so apps coalesce live-resize. */
static void publish_app_configure(int slot) {
    if (slot < 0 || slot >= MAX_GUI_APPS || !app_sessions[slot].used ||
        !app_sessions[slot].shared)
        return;
    int tw;
    int th;
    app_target_size(WIN_APP_BASE + slot, &tw, &th);
    if (tw <= 0 || th <= 0)
        return;
    app_sessions[slot].shared->configure_width = (uint32_t)tw;
    app_sessions[slot].shared->configure_height = (uint32_t)th;
    __sync_synchronize();
}

/* force=0: one configure in flight until the app presents (live resize).
 * force=1: mouse-up / maximize / mode change — always push final size. */
static int sync_app_size(int id, int force) {
    int slot = app_slot_for_win(id);
    if (slot < 0 || !app_sessions[slot].used)
        return -1;
    int target_w;
    int target_h;
    app_target_size(id, &target_w, &target_h);
    if (target_w <= 0 || target_h <= 0)
        return -1;
    publish_app_configure(slot);
    if (target_w == app_sessions[slot].want_w &&
        target_h == app_sessions[slot].want_h) {
        app_sessions[slot].resize_dirty = 0;
        return 0;
    }
    if (!force && app_sessions[slot].resize_inflight)
        return 0;
    if (!force && gpu_present_ready) {
        uint32_t now = monotonic_ms();
        /* DWM-style frame pacing: coalesce pointer-rate configures to at most
         * one application layout/GPU commit per display interval.  Window
         * chrome still follows every input packet in the compositor. */
        if ((uint32_t)(now - app_sessions[slot].last_resize_sent_ms) < 16u)
            return 0;
        app_sessions[slot].last_resize_sent_ms = now;
    }
    app_sessions[slot].want_w = target_w;
    app_sessions[slot].want_h = target_h;
    if (app_send_event(slot, GUIAPP_EVT_RESIZE, 0, 0, 0, 0, 0) < 0)
        return -1;
    app_sessions[slot].resize_inflight = 1;
    app_sessions[slot].resize_dirty = 0;
    scroll_x[id] = 0;
    scroll_y[id] = 0;
    return 0;
}

static void run_app_with_arg(const char *path, const char *argument) {
    char msg[96];
    int slot = -1;
    for (int i = 0; i < MAX_GUI_APPS; i++) {
        if (!app_sessions[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        gui_log("[gui] no free app window");
        return;
    }

    struct shm_mapping mapping;
    uint32_t surface_pixels = (uint32_t)sw * (uint32_t)sh;
    if (shm_create(GUIAPP_SHARED_SIZE_FOR_PIXELS(surface_pixels), &mapping) < 0) {
        gui_log("[gui] shared surface failed");
        return;
    }
    struct guiapp_shared_surface *shared =
        (struct guiapp_shared_surface *)mapping.address;
    shared->sequence = 0;
    shared->capacity_pixels = surface_pixels;
    shared->width = 0;
    shared->height = 0;
    shared->configure_width = 0;
    shared->configure_height = 0;
    shared->capabilities = gpu_present_ready ? GUIAPP_CAP_GPU_CANVAS : 0;
    shared->canvas_count = 0;
    shared->canvas_string_bytes = 0;
    shared->script_control_enabled = 0;
    shared->script_running = 0;
    shared->script_started_ms = 0;
    shared->script_cancel_sequence = 0;
    shared->script_control_key = 0;
    shared->events_consumed = 0;

    int ev_pipe[2] = {-1, -1};
    int frame_pipe[2] = {-1, -1};
    if (pipe(ev_pipe) < 0 || pipe(frame_pipe) < 0) {
        if (ev_pipe[0] >= 0) close(ev_pipe[0]);
        if (ev_pipe[1] >= 0) close(ev_pipe[1]);
        (void)shm_unmap(mapping.token);
        gui_log("[gui] pipe failed");
        return;
    }

    char ev_fd[12];
    char frame_fd[12];
    char shm_token[12];
    int_to_dec(ev_pipe[0], ev_fd, sizeof(ev_fd));
    int_to_dec(frame_pipe[1], frame_fd, sizeof(frame_fd));
    int_to_dec((int)mapping.token, shm_token, sizeof(shm_token));
    char *argv[6];
    argv[0] = (char *)path;
    argv[1] = "--buzz-gui";
    argv[2] = ev_fd;
    argv[3] = frame_fd;
    argv[4] = (char *)(argument ? argument : "");
    argv[5] = shm_token;
    int argc = 6;

    copy_text(msg, "launch ", sizeof(msg));
    append_text(msg, path, sizeof(msg));
    gui_log(msg);
    int pid = spawn_process_args(path, argv, argc,
                                 SPAWN_FLAG_SILENT |
                                 SPAWN_FLAG_INHERIT_FDS |
                                 SPAWN_FLAG_SERIAL_STDIO);
    close(ev_pipe[0]);
    close(frame_pipe[1]);
    if (pid < 0) {
        close(ev_pipe[1]);
        close(frame_pipe[0]);
        (void)shm_unmap(mapping.token);
        gui_log("[gui] launch failed");
        return;
    }

    int id = WIN_APP_BASE + slot;
    int default_w = APP_DEFAULT_W + 30;
    int default_h = APP_DEFAULT_H + 70;
    if (strcmp(path, "/fs/apps/taskmanager") == 0) {
        default_w = 820;
        default_h = 590;
    }
    windows[id].title = app_sessions[slot].title;
    /* Remember the executable name so the taskbar can pick a glyph; the
     * title belongs to the app and may change at any time. */
    {
        const char *base = path;
        for (const char *p = path; *p; p++)
            if (*p == '/')
                base = p + 1;
        copy_text(windows[id].app_name, base, sizeof(windows[id].app_name));
    }
    int initial_w = min_i(default_w, sw - 120);
    int initial_h = min_i(default_h, work_area().h - 64);
    windows[id].r = (struct rect){
        clamp_i((sw - initial_w) / 2 + slot * 24, 24, sw - initial_w - 24),
        min_i(work_area().y + 40 + slot * 24, sh - TASKBAR_H - initial_h - 16),
        initial_w, initial_h
    };
    windows[id].restore = windows[id].r;
    windows[id].visible = 1;
    windows[id].minimized = 0;
    windows[id].maximized = 0;
    if (sw < 1000) {
        windows[id].r = work_area();
        windows[id].maximized = 1;
    }

    app_sessions[slot].used = 1;
    app_sessions[slot].events_sent = 0;
    app_sessions[slot].script_busy_visible = 0;
    app_sessions[slot].input_overflow = 0;
    app_sessions[slot].pid = pid;
    app_sessions[slot].to_fd = ev_pipe[1];
    app_sessions[slot].from_fd = frame_pipe[0];
    app_target_size(id, &app_sessions[slot].want_w, &app_sessions[slot].want_h);
    app_sessions[slot].surface_w = 0;
    app_sessions[slot].surface_h = 0;
    app_sessions[slot].resize_dirty = 0;
    app_sessions[slot].resize_inflight = 0;
    app_sessions[slot].last_resize_sent_ms = 0;
    app_sessions[slot].gpu_sync_warned = 0;
    app_sessions[slot].reader_dead = 0;
    app_sessions[slot].closing = 0;
    app_sessions[slot].wants_tick = path && strstr(path, "taskmanager") != 0;
    app_sessions[slot].shm_token = mapping.token;
    app_sessions[slot].shared = shared;
    app_sessions[slot].gpu_resource = 0;
    app_sessions[slot].gpu_resource_w = 0;
    app_sessions[slot].gpu_resource_h = 0;
    app_sessions[slot].gpu_front_bank = 0;
    app_sessions[slot].dirty_lock = 0;
    app_sessions[slot].dirty_valid = 0;
    app_sessions[slot].dirty_rect = (struct rect){0, 0, 0, 0};
    app_sessions[slot].last_sequence = 0;
    app_sessions[slot].caret_x = 0;
    app_sessions[slot].caret_y = 0;
    app_sessions[slot].caret_valid = 0;
    copy_text(app_sessions[slot].title, "Application", sizeof(app_sessions[slot].title));
    publish_app_configure(slot);

    app_sessions[slot].reader_tid = spawn(app_reader_functions[slot]);
    if (app_sessions[slot].reader_tid < 0 ||
        app_send_event(slot, GUIAPP_EVT_INIT, 0, 0, 0, 0, 0) < 0) {
        gui_log("[gui] app protocol failed");
        close_window(id);
        return;
    }
    /* INIT is an in-flight configure until the first presented frame. */
    app_sessions[slot].resize_inflight = 1;

    copy_text(msg, "started pid ", sizeof(msg));
    append_uint(msg, (unsigned int)pid, sizeof(msg));
    gui_log(msg);
    activate(id);
    for (int i = 0; i < app_count; i++) {
        if (strcmp(apps[i].path, path) == 0) {
            app_selected = i;
            save_preferences();
            break;
        }
    }
}

static void run_app(const char *path) {
    run_app_with_arg(path, 0);
}

static void activate(int id) {
    if (id < 0 || id >= WIN_COUNT)
        return;
    int old_focus = focus;
    int was_hidden = !windows[id].visible || windows[id].minimized;
    if (old_focus != id) {
        int old_slot = app_slot_for_win(old_focus);
        if (old_slot >= 0 && app_sessions[old_slot].used) {
            for (int key = 0; key < GUIAPP_KEY_COUNT; key++) {
                if (key_owner[key] != old_slot + 1) continue;
                (void)app_send_event(old_slot, GUIAPP_EVT_KEY, 0, 0, key, 0, 0);
                key_owner[key] = 0;
            }
        }
    }
    windows[id].visible = 1;
    windows[id].minimized = 0;
    for (int i = 0; i < WIN_COUNT; i++)
        windows[i].active = 0;
    windows[id].active = 1;
    int pos = -1;
    for (int i = 0; i < WIN_COUNT; i++) {
        if (z_order[i] == id) {
            pos = i;
            break;
        }
    }
    if (pos >= 0 && id != WIN_LAUNCHER) {
        for (int i = pos; i < WIN_COUNT - 1; i++)
            z_order[i] = z_order[i + 1];
        z_order[WIN_COUNT - 1] = id;
    }
    focus = id;
    if (id != WIN_LAUNCHER && (old_focus != id || was_hidden) && !motion_pointer_down) {
        window_motion_cancel(id);
        ui_motion_reset(&window_motion[id], 24);
        /* Start the clock after launch/configure/preferences I/O returns to
         * the event loop, rather than spending the arrival during that I/O. */
        window_motion_pending[id] = 1;
        win_damage(id);
    }
    if (old_focus != id) {
        taskbar_damage();
        win_damage(old_focus);
        win_damage(id);
        taskbar_damage();
    }
    /* Focus can change without a pointer move (keyboard navigation, launch,
     * minimize/close).  Reconcile application hover state immediately so a
     * button under the stationary pointer gets the same feedback as one
     * reached by moving the pointer into it. */
    update_hover_app(1);
}

static void layout(void) {
    int margin = max_i(18, sw / 48);
    int top = work_area().y;
    int dock = TASKBAR_H;
    int content_h = sh - top - dock - margin * 2;
    int right_w = min_i(max_i(400, sw / 3), 620);

    windows[WIN_LAUNCHER].title = "Workspace";
    windows[WIN_LAUNCHER].dock_label = "Apps";
    windows[WIN_LAUNCHER].r = work_area();
    windows[WIN_LAUNCHER].restore = windows[WIN_LAUNCHER].r;
    windows[WIN_LAUNCHER].visible = 1;

    windows[WIN_STATUS].title = "Settings";
    windows[WIN_STATUS].dock_label = "Sys";
    windows[WIN_STATUS].r = (struct rect){
        sw - right_w - margin,
        top + margin,
        right_w,
        min_i(content_h, max_i(460, (content_h * 2) / 3))
    };
    windows[WIN_STATUS].restore = windows[WIN_STATUS].r;
    windows[WIN_STATUS].visible = 0;

    z_order[0] = WIN_LAUNCHER;
    z_order[1] = WIN_STATUS;
    for (int i = 0; i < MAX_GUI_APPS; i++) {
        int id = WIN_APP_BASE + i;
        windows[id].title = "Application";
        windows[id].dock_label = 0;
        windows[id].r = (struct rect){100 + i * 32, 90 + i * 32, 520, 360};
        windows[id].restore = windows[id].r;
        windows[id].visible = 0;
        windows[id].active = 0;
        windows[id].minimized = 0;
        windows[id].maximized = 0;
        z_order[WIN_APP_BASE + i] = id;
    }
    activate(WIN_LAUNCHER);
}

/* Static paper-and-shape wallpaper; clipped circles keep damage cheap. */
static int metro_start_visible(void) { return focus == WIN_LAUNCHER; }

static void draw_background(void) {
    struct ui_surface s = ui_target();
    ui_fill(&s, ui_rect_make(0, 0, sw, sh), 0x180052u);
}

static struct rect caption_rect(int id, int control);
static struct rect control_hit_rect(int id, int control);
static int control_hovered(int id, int control);
static int top_window_at(int x, int y);

/* One caption button: subtle fill on hover, red on close, and the outer
 * corner rounded so it sits flush inside the window frame. */
static void draw_caption_button(struct ui_surface *s, int id, int control) {
    struct rect b = motion_paint_rect(id, caption_rect(id, control));
    struct ui_rect box = ui_of(b);
    int hovered = control_hovered(id, control);
    int pressed = hovered && (prev_buttons & 1);
    int is_close = control == 2;
    int corners = is_close ? UI_CORNER_TR : 0;
    uint32_t glyph = windows[id].active ? UI_TEXT_PRIMARY : UI_TEXT_TERTIARY;
    int icon;

    int feedback = caption_motion[id][control].value;
    if (feedback || pressed) {
        uint32_t fill = is_close
            ? (pressed ? UI_CAPTION_CLOSE_PRESS : UI_CAPTION_CLOSE)
            : (pressed ? UI_SUBTLE_PRESSED : UI_SUBTLE_HOVER);
        ui_fill_round_mask(s, box, UI_RADIUS_WINDOW, corners, fill,
                           pressed ? 255 : feedback);
        glyph = is_close ? ui_blend(0xFFFFFFu, glyph, feedback) : UI_TEXT_PRIMARY;
    }
    if (control == 0)
        icon = UI_ICON_MINIMIZE;
    else if (control == 1)
        icon = windows[id].maximized ? UI_ICON_RESTORE : UI_ICON_MAXIMIZE;
    else
        icon = UI_ICON_CLOSE;
    ui_icon_in(s, icon, box, 10, glyph, 255);
}

static void draw_window_frame(int id) {
    struct window *w = &windows[id];
    struct ui_surface s;
    struct ui_rect frame, title;
    struct ui_text_style ts;
    int radius;

    if (!w->visible || w->minimized)
        return;
    s = ui_target();
    frame = ui_of(motion_paint_rect(id, w->r));
    /* A maximised window has no wallpaper around it to round against, so it
     * squares off as it does on any modern desktop. */
    radius = w->maximized ? 0 : UI_RADIUS_WINDOW;

    if (!w->maximized)
        ui_shadow(&s, frame, radius, UI_ELEV_FLYOUT_R,
                  w->active ? UI_ELEV_DIALOG_A / 2 : UI_ELEV_CARD_A / 2, 4);
    ui_fill_round(&s, frame, radius, UI_BG_SOLID);
    title = ui_rect_make(frame.x, frame.y, frame.w, WINDOW_TITLE_H);
    const char *identity = id == WIN_STATUS ? "settings" : w->app_name;
    uint32_t title_color = ui_blend(ui_app_identity(identity)->color,
                                     UI_BG_SOLID, w->active ? 95 : 45);
    ui_fill_round_mask(&s, title, radius, UI_CORNER_TOP,
                       title_color, 255);
    ui_fill_a(&s, ui_rect_make(frame.x, frame.y + WINDOW_TITLE_H, frame.w, 1),
              UI_STROKE_DIVIDER, w->active ? 255 : 160);

    ts = ui_style(UI_FONT_BODY,
                  w->active ? UI_TEXT_PRIMARY : UI_TEXT_TERTIARY);
    ui_app_badge(&s, id == WIN_STATUS ? "settings" : w->app_name,
                 frame.x + 14, frame.y + (WINDOW_TITLE_H - 26) / 2, 26);
    ui_text_in(&s, ui_rect_make(frame.x + 52, frame.y,
                                frame.w - 52 - 3 * UI_CAPTION_BTN_W - 8,
                                WINDOW_TITLE_H),
               app_slot_for_win(id) >= 0 &&
                   app_sessions[app_slot_for_win(id)].script_busy_visible
                   ? "Page busy - Esc to stop" : w->title, ts);

    draw_caption_button(&s, id, 0);
    draw_caption_button(&s, id, 1);
    draw_caption_button(&s, id, 2);

    /* Outline last, so it sits above the title bar fill and the caption
     * button hover states rather than being painted over by them. */
    ui_stroke_round(&s, frame, radius, 1,
                    w->active ? UI_STROKE_SURFACE : UI_STROKE_CONTROL, 255);

    if (!w->maximized) {
        /* Resize grip: three hairlines in the bottom-right corner. */
        for (int i = 0; i < 3; i++)
            ui_fill_a(&s,
                      ui_rect_make(frame.x + frame.w - 6 - i * 4,
                                   frame.y + frame.h - 14 + i * 4, 4, 1),
                      UI_TEXT_TERTIARY, 200);
    }
}

static struct rect content_rect(int id) {
    if (id == WIN_LAUNCHER) {
        int pad = sw >= 1280 ? 120 : (sw < 1000 ? 24 : 64);
        int top = sw < 1000 ? 132 : 184;
        return (struct rect){pad, top, sw - pad * 2, sh - top - 64};
    }
    struct rect r = windows[id].r;
    return (struct rect){r.x + 12, r.y + WINDOW_TITLE_H + 12,
                         r.w - 30, r.h - WINDOW_TITLE_H - 44};
}

/* Compact layouts use the available width while keeping readable controls.
 * Landscape cards stay short; portrait cards gain space for icons and data. */
static int compact_columns(struct rect c) { return c.w < 600 ? 2 : 3; }
static int compact_tile_width(struct rect c) {
    int cols = compact_columns(c);
    return (c.w - (cols - 1) * 12) / cols;
}
static int compact_tile_height(struct rect c) {
    return sh < 700 ? 150 : min_i(200, compact_tile_width(c));
}
static int compact_grid_x(struct rect c) {
    int cols = compact_columns(c);
    return c.x + (c.w - cols * compact_tile_width(c) - (cols - 1) * 12) / 2;
}

static int metro_layout_scale(struct rect c) {
    int tablet = sw >= 1000 && (sw < 1440 || sh < 850);
    int scale = metro_scale(c.h + (tablet ? 160 : 0));
    int native_width = metro_group_x(2, 100) + metro_span(4, 100);
    int width_scale = c.w * 100 / native_width;
    return min_i(scale, max_i(30, width_scale));
}

static int launcher_neighbor(int key) {
    if (app_count < 1) return 0;
    struct rect current = launcher_row_paint_rect(app_selected);
    int cx = current.x + current.w / 2, cy = current.y + current.h / 2;
    int best = app_selected, best_score = 0x7FFFFFFF;
    for (int i = 0; i < app_count; i++) {
        if (i == app_selected) continue;
        struct rect tile = launcher_row_paint_rect(i);
        int dx = tile.x + tile.w / 2 - cx, dy = tile.y + tile.h / 2 - cy;
        int vertical = key == KEY_UP || key == KEY_DOWN;
        int forward = vertical ? dy : dx, cross = vertical ? dx : dy;
        if (key == KEY_UP || key == KEY_LEFT) forward = -forward;
        if (forward <= 0) continue;
        int score = forward * forward + 4 * cross * cross;
        if (score < best_score) { best_score = score; best = i; }
    }
    return best;
}

/* Fit a committed source aspect ratio into the current content rect.  GPU
 * composition passes the dimensions belonging to its front texture rather
 * than producer-side metadata for a frame that may still be uploading. */
static struct rect scaled_view_rect_for(int id, int source_w, int source_h) {
    struct rect c = content_rect(id);
    if (source_w <= 0 || source_h <= 0)
        return c;
    int vw = c.w;
    int vh = c.w * source_h / source_w;
    if (vh > c.h) {
        vh = c.h;
        vw = c.h * source_w / source_h;
    }
    if (vw < 1)
        vw = 1;
    if (vh < 1)
        vh = 1;
    return (struct rect){c.x + (c.w - vw) / 2,
                         c.y + (c.h - vh) / 2, vw, vh};
}

static struct rect scaled_view_rect(int id, int slot) {
    if (!app_sessions[slot].scaled_surface)
        return content_rect(id);
    return scaled_view_rect_for(id, app_sessions[slot].source_w,
                                app_sessions[slot].source_h);
}

/* Nearest-neighbor scale into dst.  Source size is taken from the shared
 * header inside the seqlock so a concurrent resize cannot pair a new
 * buffer layout with a stale stride (that reads as diagonal striping). */
static int blit_shared_scaled(int slot, const uint32_t *pixels, struct rect dst) {
    if (dst.w <= 0 || dst.h <= 0)
        return 1;
    struct rect visible = intersect_rect(dst, compose_clip);
    if (visible.w <= 0 || visible.h <= 0)
        return 1;
    struct guiapp_shared_surface *shared = app_sessions[slot].shared;
    if (!shared || !pixels)
        return 0;
    int vw = dst.w;
    int vh = dst.h;
    int dx = dst.x;
    int dy = dst.y;
    int copied = 0;
    for (int attempt = 0; attempt < 100 && !copied; attempt++) {
        uint32_t sequence = shared->sequence;
        if (sequence & 1u) {
            yield();
            continue;
        }
        __sync_synchronize();
        int aw = (int)shared->width;
        int ah = (int)shared->height;
        if (aw <= 0 || ah <= 0 ||
            aw > APP_SURFACE_MAX_W || ah > APP_SURFACE_MAX_H) {
            yield();
            continue;
        }
        int use_map = vw <= APP_SURFACE_MAX_W && vh <= APP_SURFACE_MAX_H;
        if (use_map &&
            (app_sessions[slot].scale_map_w != vw ||
             app_sessions[slot].scale_map_h != vh ||
             app_sessions[slot].scale_source_w != aw ||
             app_sessions[slot].scale_source_h != ah)) {
            for (int x = 0; x < vw; x++)
                app_sessions[slot].xmap[x] = (uint16_t)(x * aw / vw);
            for (int y = 0; y < vh; y++)
                app_sessions[slot].ymap[y] = (uint16_t)(y * ah / vh);
            app_sessions[slot].scale_map_w = vw;
            app_sessions[slot].scale_map_h = vh;
            app_sessions[slot].scale_source_w = aw;
            app_sessions[slot].scale_source_h = ah;
        }
        int xscale = vw / aw;
        int yscale = vh / ah;
        int integer_scale = xscale > 0 && yscale > 0 &&
            xscale * aw == vw && yscale * ah == vh;
        if (integer_scale) {
            int first_x = visible.x - dx;
            int cached_sy = -1;
            for (int y = 0; y < visible.h; y++) {
                uint32_t *row = fb + (visible.y + y) * fb_stride + visible.x;
                int sy = (visible.y + y - dy) / yscale;
                if (sy != cached_sy) {
                    const uint32_t *src = pixels + sy * aw;
                    int out = 0;
                    int pos = first_x;
                    while (out < visible.w) {
                        int sx = pos / xscale;
                        int run = xscale - pos % xscale;
                        if (run > visible.w - out)
                            run = visible.w - out;
                        for (int k = 0; k < run; k++)
                            scaled_scanline[out + k] = src[sx];
                        out += run;
                        pos += run;
                    }
                    cached_sy = sy;
                }
                memcpy(row, scaled_scanline, (size_t)visible.w * sizeof(uint32_t));
            }
        } else if (use_map) {
            int cached_sy = -1;
            for (int y = 0; y < visible.h; y++) {
                uint32_t *row = fb + (visible.y + y) * fb_stride + visible.x;
                int sy = app_sessions[slot].ymap[visible.y + y - dy];
                if (sy != cached_sy) {
                    const uint32_t *src = pixels + sy * aw;
                    int sx0 = visible.x - dx;
                    for (int x = 0; x < visible.w; x++)
                        scaled_scanline[x] =
                            src[app_sessions[slot].xmap[sx0 + x]];
                    cached_sy = sy;
                }
                memcpy(row, scaled_scanline, (size_t)visible.w * sizeof(uint32_t));
            }
        } else {
            for (int y = 0; y < visible.h; y++) {
                uint32_t *row = fb + (visible.y + y) * fb_stride + visible.x;
                int sy = (visible.y + y - dy) * ah / vh;
                const uint32_t *src = pixels + sy * aw;
                for (int xx = 0; xx < visible.w; xx++)
                    row[xx] = src[(visible.x - dx + xx) * aw / vw];
            }
        }
        __sync_synchronize();
        copied = shared->sequence == sequence &&
            shared->width == (uint32_t)aw &&
            shared->height == (uint32_t)ah;
        if (!copied && (attempt & 3) == 3)
            yield();
    }
    return copied;
}

/* 1:1 blit of the live shared buffer into content (top-left).  Stride always
 * comes from shared->width inside the seqlock — never from a stale
 * session surface_w (FileManager-sized frames made this look like 花纹). */
static int blit_shared_1to1(int slot, const uint32_t *pixels, struct rect content) {
    struct guiapp_shared_surface *shared = app_sessions[slot].shared;
    if (!shared || !pixels || content.w <= 0 || content.h <= 0)
        return 0;
    int copied = 0;
    for (int attempt = 0; attempt < 100 && !copied; attempt++) {
        uint32_t sequence = shared->sequence;
        if (sequence & 1u) {
            yield();
            continue;
        }
        __sync_synchronize();
        int aw = (int)shared->width;
        int ah = (int)shared->height;
        if (aw <= 0 || ah <= 0 ||
            aw > APP_SURFACE_MAX_W || ah > APP_SURFACE_MAX_H) {
            yield();
            continue;
        }
        struct rect src = {content.x, content.y, aw, ah};
        struct rect visible = intersect_rect(
            intersect_rect(src, content), compose_clip);
        if (visible.w > 0 && visible.h > 0) {
            int sx = visible.x - content.x;
            int sy = visible.y - content.y;
            for (int y = 0; y < visible.h; y++)
                memcpy(fb + (visible.y + y) * fb_stride + visible.x,
                       pixels + (sy + y) * aw + sx,
                       (size_t)visible.w * sizeof(uint32_t));
        }
        __sync_synchronize();
        copied = shared->sequence == sequence &&
            shared->width == (uint32_t)aw &&
            shared->height == (uint32_t)ah;
        if (!copied && (attempt & 3) == 3)
            yield();
    }
    return copied;
}

/* Caption buttons are full-height rectangles flush with the top-right
 * corner, not round traffic lights: full-height targets, hairline glyphs, and
 * a red close button on hover. */
static struct rect caption_rect(int id, int control) {
    struct rect r = windows[id].r;
    /* control 0 = minimise, 1 = maximise, 2 = close, right to left. */
    int slot = 2 - control;
    return (struct rect){r.x + r.w - (slot + 1) * UI_CAPTION_BTN_W, r.y,
                         UI_CAPTION_BTN_W, UI_CAPTION_BTN_H};
}

static struct rect control_hit_rect(int id, int control) {
    return caption_rect(id, control);
}

static int control_hovered(int id, int control) {
    return top_window_at(pointer_x, pointer_y) == id &&
           inside(pointer_x, pointer_y, control_hit_rect(id, control));
}

static int content_width(int id) {
    int slot = app_slot_for_win(id);
    if (slot >= 0 && app_sessions[slot].used)
        return content_rect(id).w;
    if (id == WIN_LAUNCHER) {
        if (sw < 1000) return content_rect(id).w;
        int scale = metro_layout_scale(content_rect(id));
        int groups = 3;
        for (int i = 0; i < app_count; i++)
            if (metro_tile_for(apps[i].name, i).group == 3) groups = 4;
        return metro_group_x(groups - 1, scale) + metro_span(4, scale);
    }
    if (id == WIN_STATUS)
        return content_rect(id).w;
    return 520;
}

static int content_height(int id) {
    int slot = app_slot_for_win(id);
    if (slot >= 0 && app_sessions[slot].used)
        return content_rect(id).h;
    if (id == WIN_LAUNCHER) {
        if (sw < 1000) {
            struct rect c = content_rect(id);
            int cols = compact_columns(c);
            return ((app_count + cols - 1) / cols) * (compact_tile_height(c) + 12) + 80;
        }
        return content_rect(id).h;
    }
    if (id == WIN_STATUS)
        return status_resolution_bottom();
    return 250;
}

static int max_scroll_x(int id) {
    struct rect c = content_rect(id);
    return max_i(0, content_width(id) - c.w);
}

static int max_scroll_y(int id) {
    struct rect c = content_rect(id);
    return max_i(0, content_height(id) - c.h);
}

static void clamp_scroll(int id) {
    scroll_x[id] = clamp_i(scroll_x[id], 0, max_scroll_x(id));
    scroll_y[id] = clamp_i(scroll_y[id], 0, max_scroll_y(id));
}

static int gcd_i(int a, int b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) {
        int t = a % b;
        a = b;
        b = t;
    }
    return a > 0 ? a : 1;
}

static void append_aspect(char *out, int cap, int width, int height) {
    int g = gcd_i(width, height);
    append_uint(out, (unsigned int)(width / g), cap);
    append_text(out, ":", cap);
    append_uint(out, (unsigned int)(height / g), cap);
}

static int current_display_mode(void) {
    for (int i = 0; i < DISPLAY_MODE_COUNT; i++)
        if (display_modes[i].width == sw && display_modes[i].height == sh)
            return i;
    return -1;
}

/* Walk the aspect-ratio groups used to lay out resolution buttons. */
static int display_group_at(int group_index, int *start_out, int *count_out) {
    int group = 0;
    int i = 0;
    while (i < DISPLAY_MODE_COUNT) {
        int start = i;
        const char *ratio = display_modes[i].ratio;
        while (i < DISPLAY_MODE_COUNT &&
               strcmp(display_modes[i].ratio, ratio) == 0)
            i++;
        if (group == group_index) {
            if (start_out) *start_out = start;
            if (count_out) *count_out = i - start;
            return 1;
        }
        group++;
    }
    return 0;
}

/* Whether the System pane carries the extra "virgl 3D available" row.  Cached
 * rather than queried per paint because the resolution grid's origin is
 * derived from the row count: hit testing runs between paints and must agree
 * with what was last drawn. */
static int status_virgl_row;

static void status_refresh_caps(void) {
    struct gpu3d_caps caps;
    status_virgl_row = gpu3d_info(&caps) == 0 && caps.available;
}

/* Row pitch shared by the System pane's text block and by everything that
 * measures below it. */
static int status_row_step(void) {
    return ui_line_height(UI_FONT_BODY) + 6;
}

/* Content Y of the "Resolution by aspect" heading: directly below the flowed
 * text rows.  The previous fixed 236 assumed a smaller UI font, so the heading
 * painted on top of "Esc returns to shell". */
static int status_res_head_y(void) {
    int rows = STATUS_TEXT_ROWS + (status_virgl_row ? 1 : 0);
    return rows * status_row_step() + STATUS_SECTION_GAP;
}

/* Content Y of the first aspect-group label, below the heading. */
static int status_res_body_y(void) {
    return status_res_head_y() + ui_line_height(UI_FONT_BODY_LG) + 6;
}

/* Content-local layout of mode button `index` inside a panel of width `inner_w`. */
static void display_mode_cell(int index, int inner_w,
                              int *x_out, int *y_out, int *w_out, int *h_out) {
    int y = status_res_body_y();
    int group = 0;
    int start = 0;
    int count = 0;
    while (display_group_at(group, &start, &count)) {
        y += DISPLAY_GROUP_LABEL_H;
        if (index >= start && index < start + count) {
            int local = index - start;
            int row = local / DISPLAY_MODE_COLS;
            int col = local % DISPLAY_MODE_COLS;
            int cols = count - row * DISPLAY_MODE_COLS;
            if (cols > DISPLAY_MODE_COLS)
                cols = DISPLAY_MODE_COLS;
            int gap = DISPLAY_BTN_GAP;
            int btn_w = (inner_w - gap * (cols - 1)) / cols;
            if (btn_w < 1) btn_w = 1;
            *x_out = col * (btn_w + gap);
            *y_out = y + row * (DISPLAY_BTN_H + gap);
            *w_out = col + 1 == cols ? inner_w - *x_out : btn_w;
            *h_out = DISPLAY_BTN_H;
            return;
        }
        int rows = (count + DISPLAY_MODE_COLS - 1) / DISPLAY_MODE_COLS;
        y += rows * DISPLAY_BTN_H + (rows - 1) * DISPLAY_BTN_GAP +
             DISPLAY_GROUP_GAP;
        group++;
    }
    *x_out = 0;
    *y_out = status_res_body_y();
    *w_out = inner_w;
    *h_out = DISPLAY_BTN_H;
}

static int status_resolution_bottom(void) {
    int x = 0, y = 0, w = 0, h = 0;
    /* Width only affects column sizing; bottom Y is independent of it. */
    display_mode_cell(DISPLAY_MODE_COUNT - 1, 400, &x, &y, &w, &h);
    return y + h + 12;
}

static struct rect status_mode_rect(int index) {
    struct rect c = content_rect(WIN_STATUS);
    int ox = c.x - scroll_x[WIN_STATUS];
    int oy = c.y - scroll_y[WIN_STATUS];
    int x = 0, y = 0, w = 0, h = 0;
    display_mode_cell(index, max_i(1, c.w), &x, &y, &w, &h);
    return (struct rect){ox + x, oy + y, w, h};
}

static struct rect status_group_label_rect(int group_index) {
    struct rect c = content_rect(WIN_STATUS);
    int ox = c.x - scroll_x[WIN_STATUS];
    int oy = c.y - scroll_y[WIN_STATUS];
    int y = status_res_body_y();
    int group = 0;
    int start = 0;
    int count = 0;
    while (display_group_at(group, &start, &count)) {
        if (group == group_index)
            return (struct rect){ox, oy + y, c.w, DISPLAY_GROUP_LABEL_H};
        y += DISPLAY_GROUP_LABEL_H;
        int rows = (count + DISPLAY_MODE_COLS - 1) / DISPLAY_MODE_COLS;
        y += rows * DISPLAY_BTN_H + (rows - 1) * DISPLAY_BTN_GAP +
             DISPLAY_GROUP_GAP;
        group++;
    }
    return (struct rect){ox, oy + status_res_body_y(), c.w, DISPLAY_GROUP_LABEL_H};
}

static int window_min_width(int id) {
    return strcmp(windows[id].app_name, "calculator") == 0 ? 320 : WIN_MIN_W;
}

static int window_min_height(int id) {
    return strcmp(windows[id].app_name, "calculator") == 0 ? 380 : WIN_MIN_H;
}

static struct rect fit_window_rect(int id, struct rect r, int old_sw, int old_sh,
                                   int dock_y) {
    if (old_sw > 0) {
        r.x = r.x * sw / old_sw;
        r.w = r.w * sw / old_sw;
    }
    if (old_sh > 0) {
        r.y = r.y * sh / old_sh;
        r.h = r.h * sh / old_sh;
    }
    int max_w = max_i(WIN_MIN_W, sw - 16);
    int top = work_area().y;
    int max_h = max_i(WIN_MIN_H, dock_y - top - 10);
    if (r.w < window_min_width(id)) r.w = window_min_width(id);
    if (r.h < window_min_height(id)) r.h = window_min_height(id);
    if (r.w > max_w) r.w = max_w;
    if (r.h > max_h) r.h = max_h;
    if (r.x < 8) r.x = 8;
    if (r.y < top + 4) r.y = top + 4;
    if (r.x + r.w > sw - 8) r.x = sw - 8 - r.w;
    if (r.y + r.h > dock_y - 6) r.y = dock_y - 6 - r.h;
    return r;
}

static void relayout_after_mode_change(int old_sw, int old_sh) {
    int margin = max_i(18, sw / 48);
    int top = work_area().y;
    int content_h = sh - top - TASKBAR_H - margin * 2;
    int right_w = min_i(max_i(400, sw / 3), 620);
    struct rect status = {
        sw - right_w - margin, top + margin, right_w,
        min_i(content_h, max_i(460, (content_h * 2) / 3))
    };
    int dock_y = sh - TASKBAR_H;
    struct rect work = work_area();

    windows[WIN_LAUNCHER].restore = work;
    windows[WIN_LAUNCHER].r = work;
    windows[WIN_STATUS].restore = status;
    windows[WIN_STATUS].r = windows[WIN_STATUS].maximized ? work : status;

    for (int slot = 0; slot < MAX_GUI_APPS; slot++) {
        int id = WIN_APP_BASE + slot;
        struct rect restore = windows[id].maximized ? windows[id].restore
                                                     : windows[id].r;
        restore = fit_window_rect(id, restore, old_sw, old_sh, dock_y);
        windows[id].restore = restore;
        windows[id].r = windows[id].maximized ? work : restore;
        if (app_sessions[slot].used) {
            app_sessions[slot].resize_dirty = 1;
            (void)sync_app_size(id, 1);
        }
    }
    for (int id = 0; id < WIN_COUNT; id++)
        clamp_scroll(id);
    pointer_x = clamp_i(pointer_x, 0, sw - 1);
    pointer_y = clamp_i(pointer_y, 0, sh - 1);
    compose_clip = (struct rect){0, 0, sw, sh};
    context_open = 0;
    taskbar_expanded = 0;
    drag_win = -1;
    resize_win = -1;
    scroll_drag_win = -1;
    app_mouse_capture = -1;
    hover_status_mode = -1;
    hover_chrome_win = -1;
    hover_chrome_ctl = -1;
    hover_launcher_row = -1;
    pending_damage_valid = 0;
    desktop_dirty = 1;
}

static void save_preferences(void) {
    const uint32_t data[4] = {0x425A5531u, (uint32_t)sw, (uint32_t)sh,
                              (uint32_t)app_selected};
    int fd = open("/fs/desktop.settings", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        preferences_saved = -1;
        return;
    }
    int written = write_full(fd, data, sizeof(data));
    int closed = close(fd);
    preferences_saved = written == 0 && closed == 0 ? 1 : -1;
}

static int switch_display_mode(int index) {
    if (index < 0 || index >= DISPLAY_MODE_COUNT)
        return -1;
    if (current_display_mode() == index)
        return 0;
    int old_sw = sw;
    int old_sh = sh;
    motion_cancel_all();
    /* Destroy virgl surfaces before the kernel replaces their scanout
     * resource.  Reusing object handles that still reference the old target
     * is rejected by virglrenderer on the next frame. */
    gpu_present_shutdown();
    (void)bind_scanout();
    if (gfx_set_mode(display_modes[index].width,
                     display_modes[index].height) < 0) {
        gpu_present_init();
        desktop_dirty = 1;
        mode_error_until = tick + 180u;
        win_damage(WIN_STATUS);
        return -1;
    }
    struct gfx_info info;
    if (gfx_info(&info) < 0 || info.width == 0 || info.height == 0 ||
        info.width > MAX_SW || info.height > MAX_SH) {
        mode_error_until = tick + 180u;
        return -1;
    }
    sw = (int)info.width;
    sh = (int)info.height;
    display_backend = info.backend;
    mode_error_until = 0;
    /* gpu_present_init now binds the kernel's new 3-D render target. */
    (void)bind_scanout();
    gpu_present_init();
    status_refresh_caps();
    relayout_after_mode_change(old_sw, old_sh);
    pointer_x = clamp_i(pointer_x, 0, sw - 1);
    pointer_y = clamp_i(pointer_y, 0, sh - 1);
    if (hardware_cursor_ready &&
        gfx_cursor_move(pointer_x, pointer_y, 1) < 0)
        hardware_cursor_ready = 0;
    save_preferences();
    return 0;
}

static struct rect vscroll_track(int id) {
    struct rect c = content_rect(id);
    return (struct rect){c.x + c.w + 4, c.y, 10, c.h};
}

static struct rect hscroll_track(int id) {
    struct rect c = content_rect(id);
    return (struct rect){c.x, c.y + c.h + 6, c.w, 10};
}

static struct rect vscroll_thumb(int id) {
    struct rect t = vscroll_track(id);
    int maxs = max_scroll_y(id);
    if (maxs <= 0)
        return (struct rect){t.x, t.y, t.w, t.h};
    int total = content_height(id);
    int thumb_h = max_i(24, (t.h * t.h) / max_i(t.h, total));
    if (thumb_h > t.h) thumb_h = t.h;
    int y = t.y + (scroll_y[id] * (t.h - thumb_h)) / maxs;
    return (struct rect){t.x, y, t.w, thumb_h};
}

static struct rect hscroll_thumb(int id) {
    struct rect t = hscroll_track(id);
    int maxs = max_scroll_x(id);
    if (maxs <= 0)
        return (struct rect){t.x, t.y, t.w, t.h};
    int total = content_width(id);
    int thumb_w = max_i(28, (t.w * t.w) / max_i(t.w, total));
    if (thumb_w > t.w) thumb_w = t.w;
    int x = t.x + (scroll_x[id] * (t.w - thumb_w)) / maxs;
    return (struct rect){x, t.y, thumb_w, t.h};
}

/* Modern scrollbars are a thin rounded thumb on a near-invisible track that
 * only fills in on hover, so the bar reads as part of the content rather than
 * as chrome around it. */
static void draw_scrollbars(int id) {
    struct ui_surface s = ui_target();
    struct rect vt, ht;

    clamp_scroll(id);
    vt = motion_paint_rect(id, vscroll_track(id));
    ht = motion_paint_rect(id, hscroll_track(id));
    /* Tracks live just outside the content clip.  Clear them on every
     * redraw so a window that shrinks from scrollable to non-scrollable does
     * not retain the previous track/thumb pixels. */
    int start = id == WIN_LAUNCHER;
    uint32_t track = start ? 0x180052u : UI_BG_SOLID;
    uint32_t thumb = start ? 0xDAD2EAu : UI_TEXT_TERTIARY;
    uint32_t hover = start ? 0xFFFFFFu : UI_TEXT_SECONDARY;
    fill(vt, track);
    fill(ht, track);
    if (max_scroll_y(id) > 0) {
        struct rect th = motion_paint_rect(id, vscroll_thumb(id));
        int hot = inside(pointer_x, pointer_y, vt) || scroll_drag_win == id;
        if (hot)
            ui_fill_round(&s, ui_of(vt), 0, start ? 0x32156Bu : UI_BG_MICA_ALT);
        ui_fill_round(&s,
                      ui_rect_make(th.x + (hot ? 2 : 3), th.y + 2,
                                   th.w - (hot ? 4 : 6), th.h - 4),
                      0, hot ? hover : thumb);
    }
    if (max_scroll_x(id) > 0) {
        struct rect th = motion_paint_rect(id, hscroll_thumb(id));
        int hot = inside(pointer_x, pointer_y, ht) || scroll_drag_win == id;
        if (hot)
            ui_fill_round(&s, ui_of(ht), 0, start ? 0x32156Bu : UI_BG_MICA_ALT);
        ui_fill_round(&s,
                      ui_rect_make(th.x + 2, th.y + (hot ? 2 : 3),
                                   th.w - 4, th.h - (hot ? 4 : 6)),
                      0, hot ? hover : thumb);
    }
}

/* Native Segoe UI Light headlines avoid magnifying the small chrome atlas. */
static void desktop_display_text(struct ui_surface *s, struct ui_rect area,
                                 const char *text, uint32_t color) {
    static int ink_top = -1, ink_bottom;
    if (ink_top < 0) {
        ink_top = UIFONT_DISPLAY_HEIGHT;
        for (int glyph = 0; glyph < UIFONT_DISPLAY_COUNT; glyph++)
            for (int y = 0; y < UIFONT_DISPLAY_HEIGHT; y++)
                for (int x = 0; x < UIFONT_DISPLAY_WIDTH; x++)
                    if (uifont_display_alpha[glyph][y][x]) {
                        if (y < ink_top) ink_top = y;
                        if (y > ink_bottom) ink_bottom = y;
                    }
    }
    struct ui_rect saved = ui_clip_push(s, area);
    int cursor = area.x;
    int top = area.y + (area.h - (ink_bottom - ink_top + 1)) / 2;
    while (*text && cursor < area.x + area.w) {
        unsigned int cp = (unsigned char)*text++;
        if (cp < UIFONT_DISPLAY_FIRST || cp >= UIFONT_DISPLAY_FIRST + UIFONT_DISPLAY_COUNT)
            cp = '?';
        int glyph = (int)cp - UIFONT_DISPLAY_FIRST;
        for (int y = ink_top; y <= ink_bottom; y++)
            for (int x = 0; x < UIFONT_DISPLAY_WIDTH; x++) {
                int alpha = uifont_display_alpha[glyph][y][x];
                if (alpha) ui_pixel_a(s, cursor + x, top + y - ink_top, color, alpha);
            }
        cursor += uifont_display_advance[glyph];
    }
    ui_clip_pop(s, saved);
}

static struct rect metro_tile_rect(struct metro_tile tile) {
    struct rect c = content_rect(WIN_LAUNCHER);
    int tablet = sw >= 1000 && (sw < 1440 || sh < 850);
    if (tablet) {
        if (strcmp(tile.name, "browser") == 0) tile.rows = 2;
        if (strcmp(tile.name, "taskmanager") == 0) { tile.group = 2; tile.row = 0; }
        if (strcmp(tile.name, "music") == 0) tile.row = 2;
        if (strcmp(tile.name, "doom") == 0 || strcmp(tile.name, "gameboy") == 0) tile.row = 4;
    }
    int scale = metro_layout_scale(c), step = metro_unit(scale) + metro_gap(scale);
    return (struct rect){c.x - scroll_x[WIN_LAUNCHER] + metro_group_x(tile.group, scale) + tile.col * step,
                         c.y + tile.row * step, metro_span(tile.cols, scale), metro_span(tile.rows, scale)};
}

/* 0 Settings, 1 Search, 2 All apps, 3 header Search, 4 return to shell. */
static struct rect metro_action_rect(int action) {
    struct rect c = content_rect(WIN_LAUNCHER);
    if (action < 2) {
        if (sw < 1000) {
            int cols = compact_columns(c);
            int x = compact_grid_x(c);
            return (struct rect){x + action * 80, c.y - scroll_y[WIN_LAUNCHER] +
                                 ((app_count + cols - 1) / cols) * (compact_tile_height(c) + 12), 70, 70};
        }
        return metro_tile_rect(metro_tile_for(action ? "search" : "settings", 0));
    }
    if (action == 2) return (struct rect){c.x, sh - 54, 36, 36};
    return (struct rect){sw - c.x - (action == 3 ? 40 : 94), 52, 40, 40};
}

static void draw_launcher(void) {
    if (!windows[WIN_LAUNCHER].visible || windows[WIN_LAUNCHER].minimized) return;
    struct ui_surface s = ui_target();
    struct rect c = content_rect(WIN_LAUNCHER);
    desktop_display_text(&s, ui_rect_make(c.x, sw < 1000 ? 20 : 32, 400,
                                        sw < 1000 ? 70 : 90), "Start", 0xFFFFFFu);
    struct ui_text_style user = ui_style(UI_FONT_SUBTITLE, 0xFFFFFFu);
    user.align = UI_ALIGN_RIGHT;
    ui_text_in(&s, ui_rect_make(sw - c.x - 304, 50, 150, 44), "BuzzOS", user);
    ui_fill(&s, ui_rect_make(sw - c.x - 142, 52, 40, 40), 0xFFFFFFu);
    ui_circle(&s, sw - c.x - 122, 64, 6, 0x180052u, 255);
    ui_circle(&s, sw - c.x - 122, 85, 11, 0x180052u, 255);
    for (int action = 2; action < 5; action++) {
        struct ui_rect box = ui_of(metro_action_rect(action));
        int icon = action == 2 ? UI_ICON_CHEVRON_DOWN : action == 3 ? UI_ICON_SEARCH : UI_ICON_POWER;
        if (action == 2) ui_stroke_round(&s, box, 18, 2, 0xFFFFFFu, 220);
        if (metro_start_visible() && inside(pointer_x, pointer_y, metro_action_rect(action)))
            ui_fill_a(&s, box, 0xFFFFFFu, 35);
        ui_icon_in(&s, icon, box, action == 2 ? 16 : 22, 0xFFFFFFu, 255);
    }
    static const char *groups[] = {"Everyday", "Create", "Play", "More apps"};
    int scale = metro_layout_scale(c);
    struct ui_rect label_clip = ui_clip_push(&s, ui_rect_make(c.x, c.y - 48, c.w, 40));
    for (int g = 0; g < (sw < 1000 ? 1 : (content_width(WIN_LAUNCHER) > metro_group_x(3, scale) ? 4 : 3)); g++)
        ui_text_in(&s, ui_rect_make(c.x - scroll_x[WIN_LAUNCHER] + metro_group_x(g, scale), c.y - 40,
                                  metro_span(g ? 4 : 6, scale), 32), sw < 1000 ? "Apps" : groups[g],
                   ui_style(UI_FONT_SUBTITLE, 0xDAD2EAu));
    ui_clip_pop(&s, label_clip);
    struct rect saved = compose_clip_push(c);
    s = ui_target();
    for (int i = 0; i < app_count; i++) {
        struct metro_tile tile = metro_tile_for(apps[i].name, i);
        struct rect layout_r = launcher_row_paint_rect(i);
        struct rect r = layout_r;
        int reveal = home_motion_active
            ? ui_motion_reveal(motion_frame_ms, home_motion_start,
                                (uint32_t)min_i(i, 10) * HOME_STAGGER_MS, HOME_MOTION_MS) : 255;
        int hot = tile_hover_motion[i].value;
        int press = tile_press_motion[i].value;
        int inset = press * 3 / 255;
        r.x += inset;
        r.y += (255 - reveal) * 24 / 255 - hot * 2 / 255 + inset;
        r.w -= inset * 2;
        r.h -= inset * 2;
        struct ui_rect box = ui_of(r);
        uint32_t color = ui_lerp(tile.color, 0xFFFFFFu, hot * 18 / 255);
        color = ui_lerp(0x180052u, color, reveal);
        ui_fill(&s, box, color);
        if (i == app_selected || hot)
            ui_stroke_round(&s, ui_rect_inset(box, 2), 0, i == app_selected ? 3 : 2,
                            0xFFFFFFu, i == app_selected ? reveal : hot * 120 * reveal / (255 * 255));
        int icon_side = (layout_r.h >= 250 ? 108 : (layout_r.h >= 200 ? 80 : 64)) * (sw < 1000 ? 100 : scale) / 100;
        const struct ui_app_identity *identity = ui_app_identity(apps[i].name);
        if (strcmp(apps[i].name, "taskmanager") == 0 && layout_r.w >= 250) {
            int active = 0; char number[20] = "";
            for (int slot = 0; slot < MAX_GUI_APPS; slot++) if (app_sessions[slot].used) active++;
            append_uint(number, (unsigned int)active, sizeof(number));
            desktop_display_text(&s, ui_rect_make(r.x + 18, r.y + 12, 90, r.h - 42), number, ui_lerp(color, 0xFFFFFFu, reveal));
            ui_text_in(&s, ui_rect_make(r.x + 88, r.y + 28, r.w - 100, 28), "apps running",
                       ui_style(UI_FONT_BODY, ui_lerp(color, 0xFFFFFFu, reveal)));
        } else {
            ui_icon_in(&s, identity->icon, ui_rect_make(r.x, r.y + 8, r.w, r.h - 42),
                       icon_side * (255 - press * 12 / 255) / 255, 0xFFFFFFu, reveal);
        }
        ui_text_in(&s, ui_rect_make(r.x + 12, r.y + r.h - 32, r.w - 24, 26),
                   ui_app_title(apps[i].name), ui_style(layout_r.w < 140 ? UI_FONT_CAPTION : UI_FONT_BODY, ui_lerp(color, 0xFFFFFFu, reveal)));
    }
    for (int action = 0; action < 2; action++) {
        struct rect r = metro_action_rect(action);
        ui_fill(&s, ui_of(r), action ? 0xAA00FFu : 0x5133ABu);
        ui_icon_in(&s, action ? UI_ICON_SEARCH : UI_ICON_SETTINGS, ui_of(r),
                   30 * (sw < 1000 ? 100 : scale) / 100, 0xFFFFFFu, 255);
        if (metro_start_visible() && inside(pointer_x, pointer_y, r))
            ui_stroke_round(&s, ui_rect_inset(ui_of(r), 2), 0, 2, 0xFFFFFFu, 160);
    }
    compose_clip_pop(saved);
    if (max_scroll_x(WIN_LAUNCHER) > 0 || max_scroll_y(WIN_LAUNCHER) > 0)
        draw_scrollbars(WIN_LAUNCHER);
}

/* Themed settings pane: section headings, secondary body text, and the
 * resolution grid.  Rows are laid out from a step derived from the line
 * height rather than from hardcoded pixel offsets, so the pane stays aligned
 * if the UI font size moves. */
static void draw_status(void) {
    struct ui_surface s;
    struct rect c, saved_clip;
    struct ui_text_style head, body, faint;
    char line[96];
    int ox, oy, step, row, mode_error, active_mode, group, start, count;

    if (!windows[WIN_STATUS].visible || windows[WIN_STATUS].minimized)
        return;
    draw_window_frame(WIN_STATUS);
    c = motion_paint_rect(WIN_STATUS, content_rect(WIN_STATUS));
    fill(c, UI_BG_SOLID);
    /* Restrict paint to the scrollable body.  The button and rounded-fill
     * helpers only honour compose_clip, so without this push the resolution
     * grid paints through the frame onto the desktop whenever content_height
     * exceeds the window. */
    saved_clip = compose_clip_push(c);
    s = ui_target();
    ox = c.x - scroll_x[WIN_STATUS];
    oy = c.y - scroll_y[WIN_STATUS];
    step = status_row_step();

    head = ui_style(UI_FONT_BODY_LG, UI_TEXT_PRIMARY);
    head.bold = 1;
    body = ui_style(UI_FONT_BODY, UI_TEXT_SECONDARY);
    faint = ui_style(UI_FONT_BODY, UI_TEXT_TERTIARY);

    int display_rows = 4 + (status_virgl_row ? 1 : 0);
    ui_fill_round(&s, ui_rect_make(ox, oy, c.w, display_rows * step),
                  UI_RADIUS_CONTROL, UI_BG_LAYER);
    ui_fill_round(&s, ui_rect_make(ox, oy + (display_rows + 1) * step,
                                  c.w, 5 * step),
                  UI_RADIUS_CONTROL, UI_BG_LAYER);
#define STATUS_ROW(n) ui_rect_make(ox + 12, oy + (n) * step, max_i(1, c.w - 24), step)
    row = 0;
    ui_text_in(&s, STATUS_ROW(row++), "Display", head);
    copy_text(line, "Resolution ", sizeof(line));
    append_uint(line, (unsigned int)sw, sizeof(line));
    append_text(line, " x ", sizeof(line));
    append_uint(line, (unsigned int)sh, sizeof(line));
    append_text(line, "  (", sizeof(line));
    append_aspect(line, sizeof(line), sw, sh);
    append_text(line, ")", sizeof(line));
    ui_text_in(&s, STATUS_ROW(row++), line, body);
    ui_text_in(&s, STATUS_ROW(row++),
               display_backend == GFX_BACKEND_VIRTIO_GPU_2D
                   ? "VirtIO GPU 2D / 32bpp"
                   : "Bochs VBE / 32bpp",
               faint);
    if (status_virgl_row)
        ui_text_in(&s, STATUS_ROW(row++), "virgl 3D available", faint);
    copy_text(line, "Applications ", sizeof(line));
    append_uint(line, (unsigned int)app_count, sizeof(line));
    ui_text_in(&s, STATUS_ROW(row++), line, body);

    row++;
    ui_text_in(&s, STATUS_ROW(row++), "Controls", head);
    ui_text_in(&s, STATUS_ROW(row++), "Ctrl+Space toggles IME", body);
    ui_text_in(&s, STATUS_ROW(row++), "[ ] cycle resolution", body);
    ui_text_in(&s, STATUS_ROW(row++), "Esc returns to shell", body);
    ui_text_in(&s, STATUS_ROW(row++),
               preferences_saved < 0 ? "Settings could not be saved" :
               preferences_saved > 0 ? "Settings saved to disk" :
                                       "Settings save automatically", faint);
#undef STATUS_ROW

    mode_error = mode_error_until && tick < mode_error_until;
    {
        struct ui_text_style st = head;
        if (mode_error)
            st.color = UI_SYS_CRITICAL;
        ui_text_in(&s, ui_rect_make(ox, oy + status_res_head_y(), c.w,
                                    ui_line_height(UI_FONT_BODY_LG)),
                   mode_error ? "Resolution unavailable"
                              : "Resolution by aspect",
                   st);
    }
    active_mode = current_display_mode();
    group = 0;
    start = 0;
    count = 0;
    while (display_group_at(group, &start, &count)) {
        struct rect label_r = motion_paint_rect(WIN_STATUS, status_group_label_rect(group));
        ui_text_in(&s, ui_of(label_r), display_modes[start].ratio, faint);
        for (int i = 0; i < count; i++) {
            int mode = start + i;
            struct rect btn = motion_paint_rect(WIN_STATUS, status_mode_rect(mode));
            if (intersect_rect(btn, c).w <= 0)
                continue;
            button(btn, display_modes[mode].label, mode == active_mode);
        }
        group++;
    }
    compose_clip_pop(saved_clip);
    /* Scrollbars sit just outside the content rect; draw after pop. */
    draw_scrollbars(WIN_STATUS);
}

static void draw_app_window(int id) {
    int slot = app_slot_for_win(id);
    if (slot < 0 || !app_sessions[slot].used ||
        !windows[id].visible || windows[id].minimized)
        return;
    draw_window_frame(id);
    struct rect c = motion_paint_rect(id, content_rect(id));
    if (compose_skip_app_pixels)
        return;
    struct rect clip = c;
    int ox = c.x;
    int oy = c.y;
    int aw = app_sessions[slot].surface_w;
    int ah = app_sessions[slot].surface_h;
    int source_w = app_sessions[slot].source_w;
    int source_h = app_sessions[slot].source_h;
    struct guiapp_shared_surface *shared = app_sessions[slot].shared;
    if (!shared || aw <= 0 || ah <= 0) {
        if (!app_sessions[slot].scaled_surface)
            fill(c, THEME_WIN_BODY);
        return;
    }
    const uint32_t *pixels = (const uint32_t *)((const uint8_t *)shared +
        GUIAPP_SHARED_HEADER_SIZE);
    if (app_sessions[slot].scaled_surface && source_w > 0 && source_h > 0) {
        struct rect view = motion_paint_rect(id, scaled_view_rect(id, slot));
        int vw = view.w;
        int vh = view.h;
        int dx = view.x;
        int dy = view.y;
        fill((struct rect){ox, oy, c.w, dy - oy}, 0);
        fill((struct rect){ox, dy + vh, c.w, (oy + c.h) - (dy + vh)}, 0);
        fill((struct rect){ox, dy, dx - ox, vh}, 0);
        fill((struct rect){dx + vw, dy, (ox + c.w) - (dx + vw), vh}, 0);
        if (!blit_shared_scaled(slot, pixels, view))
            app_note_dirty(slot, (struct rect){0, 0, source_w, source_h});
        return;
    }
    (void)clip;
    (void)aw;
    (void)ah;
    /* Fill content first so lag margins are solid; blit uses SHM width as
     * stride under the seqlock (stale surface_w + new layout = 花纹). */
    fill(c, THEME_WIN_BODY);
    if (!blit_shared_1to1(slot, pixels, c))
        app_note_dirty(slot, (struct rect){0, 0,
            app_sessions[slot].surface_w > 0 ? app_sessions[slot].surface_w : c.w,
            app_sessions[slot].surface_h > 0 ? app_sessions[slot].surface_h : c.h});
}

static int collect_open_apps(int ids[MAX_GUI_APPS]) {
    int count = 0;
    for (int slot = 0; slot < MAX_GUI_APPS; slot++) {
        int id = WIN_APP_BASE + slot;
        if (app_sessions[slot].used && windows[id].visible)
            ids[count++] = id;
    }
    return count;
}

/* The tray slot where a desktop shows the clock.  BuzzOS has no RTC, so it
 * shows uptime rather than inventing a wall-clock time. */
static void format_uptime(char *dst, size_t cap) {
    uint32_t secs = monotonic_ms() / 1000u;
    uint32_t h = secs / 3600u;
    uint32_t m = (secs / 60u) % 60u;
    uint32_t s = secs % 60u;
    dst[0] = 0;
    append_uint(dst, h, cap);
    append_text(dst, m < 10u ? ":0" : ":", cap);
    append_uint(dst, m, cap);
    append_text(dst, s < 10u ? ":0" : ":", cap);
    append_uint(dst, s, cap);
}

/* ---- Taskbar ----------------------------------------------------------
 *
 * Geometry is produced once, by taskbar_items(), and consumed by both the
 * painter and the hit tester.  The previous dock computed its button rects
 * twice from the same inputs, which meant a layout tweak had to be mirrored
 * in two places or clicks would land on the wrong button.
 */

enum {
    TB_NONE = 0,
    TB_START,      /* value unused                       */
    TB_WINDOW,     /* value = window id                  */
    TB_OVERFLOW,   /* value unused; toggles the flyout   */
    TB_TRAY_IME,
    TB_TRAY_CLOCK,
};

struct tb_item {
    uint8_t kind;
    int16_t value;
    struct rect r;
};

static struct rect taskbar_rect(void) {
    return (struct rect){0, sh - TASKBAR_H, sw, TASKBAR_H};
}

/* Fill `out` with every interactive taskbar element, left to right, and
 * return the count.  `out` must hold TB_MAX_ITEMS entries. */
static int taskbar_items(struct tb_item *out) {
    int ids[MAX_GUI_APPS];
    int app_total = collect_open_apps(ids);
    struct rect bar = taskbar_rect();
    int by = bar.y + (TASKBAR_H - TB_BTN_H) / 2;
    int n = 0;
    int tray_x = sw - TB_TRAY_PAD;
    int clock_x, ime_x, group_w, gx, avail, shown, hidden, buttons;

    /* Tray, laid out from the right edge inward. */
    clock_x = tray_x - TB_CLOCK_W;
    ime_x = clock_x - TB_BTN_W;

    /* Button group: Start, the two system windows, then running apps. */
    buttons = 1 + WIN_APP_BASE + app_total;
    avail = ime_x - TB_TRAY_PAD * 2;
    shown = app_total;
    hidden = 0;
    if ((1 + WIN_APP_BASE + shown) * TB_STEP > avail) {
        int room = avail / TB_STEP - (1 + WIN_APP_BASE) - 1; /* -1 for More */
        shown = room > 0 ? room : 0;
        hidden = app_total - shown;
    }
    buttons = 1 + WIN_APP_BASE + shown + (hidden > 0 ? 1 : 0);
    group_w = buttons * TB_STEP - TB_GAP;
    /* Centre on the screen like the theme, but never under the tray. */
    gx = (sw - group_w) / 2;
    if (gx + group_w > ime_x - TB_TRAY_PAD)
        gx = ime_x - TB_TRAY_PAD - group_w;
    if (gx < TB_TRAY_PAD)
        gx = TB_TRAY_PAD;

    out[n].kind = TB_START;
    out[n].value = 0;
    out[n].r = (struct rect){gx, by, TB_BTN_W, TB_BTN_H};
    n++;
    for (int i = 0; i < WIN_APP_BASE; i++) {
        out[n].kind = TB_WINDOW;
        out[n].value = (int16_t)i;
        out[n].r = (struct rect){gx + n * TB_STEP, by, TB_BTN_W, TB_BTN_H};
        n++;
    }
    for (int i = 0; i < shown; i++) {
        out[n].kind = TB_WINDOW;
        out[n].value = (int16_t)ids[i];
        out[n].r = (struct rect){gx + n * TB_STEP, by, TB_BTN_W, TB_BTN_H};
        n++;
    }
    if (hidden > 0) {
        out[n].kind = TB_OVERFLOW;
        out[n].value = (int16_t)hidden;
        out[n].r = (struct rect){gx + n * TB_STEP, by, TB_BTN_W, TB_BTN_H};
        n++;
    } else {
        taskbar_expanded = 0;
    }

    out[n].kind = TB_TRAY_IME;
    out[n].value = 0;
    out[n].r = (struct rect){ime_x, by, TB_BTN_W, TB_BTN_H};
    n++;
    out[n].kind = TB_TRAY_CLOCK;
    out[n].value = 0;
    out[n].r = (struct rect){clock_x, by, TB_CLOCK_W, TB_BTN_H};
    n++;
    return n;
}

static struct rect taskbar_panel_rect(int tray) {
    struct tb_item items[TB_MAX_ITEMS];
    int count = taskbar_items(items);
    int first = tray ? count - 2 : 0;
    int last = tray ? count - 1 : count - 3;
    return (struct rect){items[first].r.x - 12, sh - TASKBAR_H + 8,
                         items[last].r.x + items[last].r.w - items[first].r.x + 24,
                         TASKBAR_H - 16};
}

static void taskbar_clock_damage(void) {
    struct tb_item items[TB_MAX_ITEMS];
    int count = taskbar_items(items);
    for (int i = 0; i < count; i++) {
        if (items[i].kind != TB_TRAY_CLOCK)
            continue;
        queue_damage((struct rect){items[i].r.x - 2, items[i].r.y - 2,
                                   items[i].r.w + 4, items[i].r.h + 4});
        return;
    }
}

/* Windows hidden behind the overflow button, in taskbar order. */
static int taskbar_hidden_windows(int ids[MAX_GUI_APPS]) {
    struct tb_item items[TB_MAX_ITEMS];
    int all[MAX_GUI_APPS];
    int total = collect_open_apps(all);
    int count = taskbar_items(items);
    int shown = 0, n = 0;
    for (int i = 0; i < count; i++)
        if (items[i].kind == TB_WINDOW && items[i].value >= WIN_APP_BASE)
            shown++;
    for (int i = shown; i < total; i++)
        ids[n++] = all[i];
    return n;
}

static struct rect taskbar_overflow_panel(int hidden) {
    int panel_w = min_i(300, sw - 24);
    int panel_h = 12 + hidden * 40;
    struct tb_item items[TB_MAX_ITEMS];
    int count = taskbar_items(items);
    int anchor = sw - panel_w - 12;
    for (int i = 0; i < count; i++)
        if (items[i].kind == TB_OVERFLOW)
            anchor = items[i].r.x + items[i].r.w - panel_w;
    return (struct rect){clamp_i(anchor, 12, max_i(12, sw - panel_w - 12)),
                         sh - TASKBAR_H - panel_h - 8, panel_w, panel_h};
}

static struct rect taskbar_tooltip_rect(void) {
    const char *title;
    int tw, tx, ty;
    if (taskbar_hover < 0 || taskbar_hover >= WIN_COUNT ||
        !windows[taskbar_hover].visible)
        return (struct rect){0, 0, 0, 0};
    title = windows[taskbar_hover].title;
    tw = min_i(sw - 16, ui_text_width(title, UI_FONT_BODY) + 24);
    tx = clamp_i(pointer_x - tw / 2, 8, max_i(8, sw - tw - 8));
    ty = sh - TASKBAR_H - 38;
    return (struct rect){tx, ty, tw, 30};
}

static void draw_taskbar_tooltip(void) {
    struct ui_surface s = ui_target();
    const char *title;
    struct rect tip_rect = taskbar_tooltip_rect();
    struct ui_rect tip;
    if (tip_rect.w <= 0 || tip_rect.h <= 0)
        return;
    title = windows[taskbar_hover].title;
    tip = ui_of(tip_rect);
    ui_shadow(&s, tip, UI_RADIUS_CONTROL, 8, UI_ELEV_CARD_A, 2);
    ui_fill_round(&s, tip, UI_RADIUS_CONTROL, UI_BG_LAYER);
    ui_stroke_round(&s, tip, UI_RADIUS_CONTROL, 1, UI_STROKE_CONTROL, 255);
    ui_text_in(&s, ui_rect_inset(tip, 8), title,
               ui_style(UI_FONT_BODY, UI_TEXT_PRIMARY));
}

/* One taskbar button: a subtle fill when hovered, an accent underline when
 * the window is open, and a wider one when it is the active window. */
static void draw_tb_button(struct ui_surface *s, struct rect r, int icon,
                           int active, int open, int hovered, int pressed,
                           int channel) {
    (void)hovered;
    (void)pressed;
    int feedback = dock_hover_motion[channel].value;
    int down = dock_press_motion[channel].value;
    int selection = dock_active_motion[channel].value;
    struct ui_rect box = ui_of(r);
    uint32_t tint = UI_TEXT_PRIMARY;
    ui_fill_round_a(s, box, UI_RADIUS_CONTROL, UI_SUBTLE_HOVER,
                    max_i(feedback, selection));
    if (down)
        ui_fill_round_a(s, box, UI_RADIUS_CONTROL, UI_SUBTLE_PRESSED, down);
    ui_icon_in(s, icon, box, TB_ICON, tint, active ? 255 : 225);
    if (open) {
        int w = 6 + selection * 18 / 255;
        ui_fill_round(s, ui_rect_make(r.x + (r.w - w) / 2, r.y + r.h - 2, w, 3),
                      1, ui_blend(UI_ACCENT_FILL, UI_TEXT_TERTIARY, selection));
    }
}

static void draw_taskbar(void) {
    if (metro_start_visible()) return;
    struct ui_surface s = ui_target();
    struct tb_item items[TB_MAX_ITEMS];
    int count = taskbar_items(items);
    char clock[16];

    ui_fill(&s, ui_rect_make(0, sh - TASKBAR_H, sw, TASKBAR_H), UI_BG_LAYER_ALT);

    for (int i = 0; i < count; i++) {
        struct tb_item *it = &items[i];
        int hovered = inside(pointer_x, pointer_y, it->r);
        int pressed = hovered && (prev_buttons & 1);
        switch (it->kind) {
        case TB_START:
            draw_tb_button(&s, it->r, UI_ICON_SEARCH, start_open, 0, hovered,
                           pressed, WIN_COUNT);
            break;
        case TB_WINDOW: {
            int id = it->value;
            draw_tb_button(&s, it->r, window_icon(id), windows[id].active,
                           windows[id].visible, hovered, pressed, id);
            const char *name = id == WIN_LAUNCHER ? "apps"
                : (id == WIN_STATUS ? "settings" : windows[id].app_name);
            ui_app_badge(&s, name, it->r.x + (it->r.w - 28) / 2,
                         it->r.y + (it->r.h - 28) / 2, 28);
            break;
        }
        case TB_OVERFLOW:
            draw_tb_button(&s, it->r, UI_ICON_MORE, taskbar_expanded, 0,
                           hovered, pressed, WIN_COUNT + 1);
            break;
        case TB_TRAY_IME: {
            struct ui_rect box = ui_of(it->r);
            ui_fill_round_a(&s, box, UI_RADIUS_CONTROL, UI_SUBTLE_HOVER,
                            dock_hover_motion[WIN_COUNT + 2].value);
            ui_icon_in(&s, UI_ICON_KEYBOARD, box, 20,
                       ime_enabled ? UI_ACCENT_FILL : UI_TEXT_SECONDARY,
                       255);
            break;
        }
        case TB_TRAY_CLOCK: {
            struct ui_rect box = ui_of(it->r);
            struct ui_text_style st = ui_style(UI_FONT_CAPTION,
                                               UI_TEXT_SECONDARY);
            ui_fill_round_a(&s, box, UI_RADIUS_CONTROL, UI_SUBTLE_HOVER,
                            dock_hover_motion[WIN_COUNT + 3].value);
            st.align = UI_ALIGN_CENTER;
            format_uptime(clock, sizeof(clock));
            ui_text_in(&s, box, clock, st);
            break;
        }
        default:
            break;
        }
    }

    if (taskbar_expanded) {
        int ids[MAX_GUI_APPS];
        int hidden = taskbar_hidden_windows(ids);
        if (hidden > 0) {
            struct rect panel = taskbar_overflow_panel(hidden);
            struct ui_rect p = ui_of(panel);
            ui_shadow(&s, p, UI_RADIUS_OVERLAY, UI_ELEV_FLYOUT_R,
                      UI_ELEV_FLYOUT_A, 4);
            shell_acrylic(&s, p, UI_RADIUS_OVERLAY, UI_BG_ACRYLIC, 205);
            ui_stroke_round(&s, p, UI_RADIUS_OVERLAY, 1, UI_STROKE_SURFACE,
                            255);
            for (int i = 0; i < hidden; i++) {
                int id = ids[i];
                struct ui_rect row = ui_rect_make(panel.x + 6,
                                                  panel.y + 6 + i * 40,
                                                  panel.w - 12, 36);
                struct rect hit = {row.x, row.y, row.w, row.h};
                if (inside(pointer_x, pointer_y, hit))
                    ui_fill_round(&s, row, UI_RADIUS_CONTROL,
                                  UI_SUBTLE_HOVER);
                ui_icon(&s, window_icon(id), row.x + 8,
                        row.y + (row.h - 18) / 2, 18, UI_TEXT_PRIMARY, 255);
                ui_text_in(&s, ui_rect_make(row.x + 34, row.y,
                                            row.w - 42, row.h),
                           windows[id].title,
                           ui_style(UI_FONT_BODY, UI_TEXT_PRIMARY));
            }
        }
    }
    draw_taskbar_tooltip();
}

/* ---- Start menu -------------------------------------------------------
 *
 * Same discipline as the taskbar: result rows are described once and both
 * the painter and the hit tester read it.
 */

static int start_matches(int indices[MAX_APPS]) {
    int count = 0;
    for (int i = 0; i < app_count; i++) {
        const struct ui_app_identity *app = ui_app_identity(apps[i].name);
        if (ui_text_contains_ascii_ci(apps[i].name, start_query) ||
            ui_text_contains_ascii_ci(ui_app_title(apps[i].name), start_query) ||
            ui_text_contains_ascii_ci(app->description, start_query))
            indices[count++] = i;
    }
    return count;
}

static struct rect start_menu_rect(void) {
    int w = sw < 1000 ? sw : min_i(START_W, sw - 24);
    int shift = (255 - search_motion.value) * w / 255;
    return (struct rect){sw - w + shift, 0, w, sh};
}

static struct rect start_results_rect(void) {
    struct rect panel = start_menu_rect();
    return (struct rect){panel.x + START_PAD, START_HEADER_H,
                         panel.w - START_PAD * 2, sh - START_HEADER_H - START_FOOTER_H};
}

static void start_clamp_scroll(int count) {
    start_scroll = clamp_i(start_scroll, 0, max_i(0, count * START_ROW_H - start_results_rect().h));
}

static void start_reveal_selection(int count) {
    int top = start_selected * START_ROW_H, bottom = top + START_ROW_H;
    int height = start_results_rect().h;
    if (top < start_scroll) start_scroll = top;
    else if (bottom > start_scroll + height) start_scroll = bottom - height;
    start_clamp_scroll(count);
}

static struct rect start_tile_rect(int index) {
    struct rect body = start_results_rect();
    return (struct rect){body.x, body.y + index * START_ROW_H - start_scroll,
                         body.w, START_ROW_H};
}

static struct rect start_close_rect(void) {
    struct rect panel = start_menu_rect();
    return (struct rect){panel.x + panel.w - START_PAD - 48,
                         panel.y + panel.h - 12 - 48, 48, 48};
}

static struct rect start_app_tile_rect(int app) {
    int indices[MAX_APPS];
    int count = start_matches(indices);
    for (int i = 0; i < count; i++)
        if (indices[i] == app) return start_tile_rect(i);
    return (struct rect){0, 0, 0, 0};
}

static void draw_start_menu(void) {
    struct ui_surface s;
    struct rect panel;
    struct ui_rect p;
    struct ui_text_style head;
    struct rect power;
    struct ui_rect saved;

    if (!start_open && !search_motion.value && !search_motion.active)
        return;
    s = ui_target();
    panel = start_menu_rect();
    p = ui_of(panel);

    ui_fill(&s, p, UI_BG_SOLID);
    ui_fill(&s, ui_rect_make(p.x, p.y, 1, p.h), UI_STROKE_SURFACE);

    head = ui_style(UI_FONT_BODY, UI_TEXT_PRIMARY);
    head.bold = 1;
    ui_text_in(&s, ui_rect_make(panel.x + START_PAD, panel.y + START_PAD,
                                panel.w - START_PAD * 2,
                                ui_line_height(UI_FONT_BODY)),
               "Search", head);

    struct ui_rect search = ui_rect_make(panel.x + START_PAD, panel.y + 48,
                                          panel.w - START_PAD * 2, 48);
    ui_fill(&s, search, UI_BG_SOLID);
    ui_stroke_round(&s, search, 0, 1, UI_ACCENT_TEXT, 255);
    ui_icon_in(&s, UI_ICON_SEARCH, ui_rect_make(search.x + 10, search.y, 24, search.h),
                16, UI_ACCENT_TEXT, 255);
    ui_text_in(&s, ui_rect_make(search.x + 40, search.y, search.w - 52, search.h),
               start_query[0] ? start_query : "Type to find an application...",
               ui_style(UI_FONT_BODY, start_query[0] ? UI_TEXT_PRIMARY : UI_TEXT_TERTIARY));

    int indices[MAX_APPS];
    int matches = start_matches(indices);
    start_clamp_scroll(matches);
    struct rect body = start_results_rect();
    saved = ui_clip_push(&s, ui_of(body));
    for (int i = 0; i < matches; i++) {
        int app = indices[i];
        struct rect tile = start_tile_rect(i);
        struct ui_rect t = ui_of(tile);
        struct ui_text_style label = ui_style(UI_FONT_BODY,
                                              UI_TEXT_PRIMARY);
        int hovered = inside(pointer_x, pointer_y, tile);
        int pressed = hovered && (prev_buttons & 1);
        if (pressed)
            ui_fill_round(&s, t, UI_RADIUS_CONTROL, UI_SUBTLE_PRESSED);
        else if (hovered || i == start_selected)
            ui_fill_round(&s, t, UI_RADIUS_CONTROL, UI_SUBTLE_HOVER);
        if (i == start_selected)
            ui_fill(&s, ui_rect_make(tile.x, tile.y + 6, 3, tile.h - 12), UI_ACCENT_TEXT);
        struct ui_rect badge = ui_rect_make(tile.x + 12, tile.y + 16, 40, 40);
        ui_fill(&s, badge, metro_tile_for(apps[app].name, app).color);
        ui_icon_in(&s, ui_app_identity(apps[app].name)->icon, badge, 24, 0xFFFFFFu, 255);
        ui_text_in(&s, ui_rect_make(tile.x + 66, tile.y + 10, tile.w - 78, 26),
                   ui_app_title(apps[app].name), label);
        ui_text_in(&s, ui_rect_make(tile.x + 66, tile.y + 38, tile.w - 78, 24),
                   ui_app_identity(apps[app].name)->description,
                   ui_style(UI_FONT_CAPTION, UI_TEXT_SECONDARY));
    }
    if (!matches)
        ui_text_in(&s, ui_rect_make(panel.x + START_PAD, panel.y + START_HEADER_H,
                                    panel.w - START_PAD * 2, START_ROW_H),
                   "No matching applications", ui_style(UI_FONT_BODY, UI_TEXT_SECONDARY));
    ui_clip_pop(&s, saved);
    if (matches * START_ROW_H > body.h) {
        int thumb_h = max_i(24, body.h * body.h / (matches * START_ROW_H));
        int thumb_y = body.y + start_scroll * (body.h - thumb_h) / (matches * START_ROW_H - body.h);
        ui_fill(&s, ui_rect_make(panel.x + panel.w - 8, thumb_y, 4, thumb_h), UI_ACCENT_TEXT);
    }

    ui_fill_a(&s, ui_rect_make(panel.x + 1,
                               panel.y + panel.h - START_FOOTER_H, panel.w - 2,
                               1),
              UI_STROKE_DIVIDER, 200);
    ui_text_in(&s, ui_rect_make(panel.x + START_PAD,
                                panel.y + panel.h - START_FOOTER_H,
                                panel.w - START_PAD * 2 - 56, START_FOOTER_H),
               "Enter opens / Esc closes", ui_style(UI_FONT_CAPTION, UI_TEXT_SECONDARY));

    power = start_close_rect();
    {
        struct ui_rect pw = ui_of(power);
        int hovered = inside(pointer_x, pointer_y, power);
        if (hovered)
            ui_fill_round(&s, pw, UI_RADIUS_CONTROL, UI_SUBTLE_HOVER);
        ui_icon_in(&s, UI_ICON_CLOSE, pw, 20, UI_TEXT_SECONDARY, 255);
    }
}

/* Returns the app index to launch, -2 for the close button, or -1 for a
 * click that the menu swallows without acting on. */
static int hit_start_menu(int x, int y) {
    struct rect panel;
    if (!start_open)
        return -1;
    panel = start_menu_rect();
    if (!inside(x, y, panel))
        return -1;
    if (inside(x, y, start_close_rect()))
        return -2;
    if (!inside(x, y, start_results_rect())) return -1;
    int indices[MAX_APPS];
    int matches = start_matches(indices);
    for (int i = 0; i < matches; i++)
        if (inside(x, y, start_tile_rect(i)))
            return indices[i];
    return -1;
}

/* Damage the Start menu plus its shadow. */
static void start_damage(void) {
    /* Cover the resting extent as well as the moving panel, including the
     * final closing sample. Otherwise the old left edge leaves a white trail. */
    struct rect panel = {sw < 1000 ? 0 : sw - START_W, 0,
                         sw < 1000 ? sw : START_W, sh};
    int pad = UI_ELEV_DIALOG_R + 8;
    queue_damage((struct rect){max_i(0, panel.x - pad),
                               max_i(0, panel.y - pad),
                               min_i(sw, panel.w + pad * 2),
                               min_i(sh, panel.h + pad * 2)});
}

/* Open or close the Start menu.
 *
 * Single point of truth for the transition: it repaints the menu region and
 * drops the hovered-tile index.  Toggling is click-driven, so the pointer does
 * not move and refresh_pointer_hover_damage() will not run -- a stale index
 * left here would compare equal on reopen and the highlight under a
 * stationary pointer would never be painted. */
static void start_set_open(int open) {
    if (start_open == open)
        return;
    start_damage();
    start_open = open;
    ui_motion_to(&search_motion, open ? 255 : 0, open ? 220 : 160, monotonic_ms());
    if (open) {
        start_query[0] = 0;
        start_selected = 0;
        start_scroll = 0;
    }
    ui_motion_reset(&result_scroll_motion, start_scroll);
    hover_start_tile = -1;
    desktop_dirty = 1;
}

static void start_handle_key(int key) {
    int indices[MAX_APPS];
    int matches = start_matches(indices);
    if (key == KEY_ESC || key == 23) {
        start_set_open(0);
        return;
    }
    if (key == '\n' || key == '\r') {
        if (matches > 0) {
            int app = indices[clamp_i(start_selected, 0, matches - 1)];
            start_set_open(0);
            run_app(apps[app].path);
        }
        return;
    }
    start_damage(); /* Preserve the old extent before filtering changes height. */
    int length = (int)strlen(start_query);
    if (key == KEY_BACKSPACE || key == 127) {
        if (length) start_query[length - 1] = 0;
        start_selected = 0;
    } else if (key >= 32 && key < 127) {
        if (length + 1 < (int)sizeof(start_query)) {
            start_query[length] = (char)key;
            start_query[length + 1] = 0;
        }
        start_selected = 0;
    } else if (key == KEY_RIGHT || key == '\t') start_selected++;
    else if (key == KEY_LEFT) start_selected--;
    else if (key == KEY_UP) start_selected--;
    else if (key == KEY_DOWN) start_selected++;
    else if (key == KEY_HOME) start_selected = 0;
    else if (key == KEY_END) start_selected = max_i(0, matches - 1);
    matches = start_matches(indices);
    start_selected = clamp_i(start_selected, 0, max_i(0, matches - 1));
    start_reveal_selection(matches);
    hover_start_tile = -1;
    start_damage();
    desktop_dirty = 1;
}

/* ---- Pinyin IME core -------------------------------------------------- */

static int pinyin_key_cmp(const char *key, const char *buf, int n) {
    for (int i = 0; i < n; i++) {
        unsigned char a = (unsigned char)key[i];
        unsigned char b = (unsigned char)buf[i];
        if (!a) return -1;
        if (a != b) return (int)a - (int)b;
    }
    return 0;
}

/* Lower bound: first entry whose key >= prefix (dictionary order). */
static int pinyin_lower_bound(const char *prefix, int n) {
    int lo = 0, hi = PINYIN_ENTRY_COUNT;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        int c = pinyin_key_cmp(pinyin_entries[mid].key, prefix, n);
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static int ime_cand_find(const char *text) {
    for (int i = 0; i < ime_cand_count; i++)
        if (strcmp(ime_cands[i], text) == 0)
            return i;
    return -1;
}

static void ime_cand_push(const char *text, int consume, int prefer_front) {
    if (!text || !text[0] || consume <= 0 || ime_cand_count >= IME_CAND_CAP)
        return;
    int n = (int)strlen(text);
    if (n >= GUIAPP_TEXT_MAX)
        n = GUIAPP_TEXT_MAX - 1;
    int existing = ime_cand_find(text);
    if (existing >= 0) {
        /* Keep the match that consumes more pinyin (better segmentation). */
        if (consume > (int)ime_cand_consume[existing])
            ime_cand_consume[existing] = (uint8_t)consume;
        return;
    }
    int slot = ime_cand_count;
    if (prefer_front && ime_cand_count > 0) {
        for (int i = ime_cand_count; i > 0; i--) {
            copy_text(ime_cands[i], ime_cands[i - 1], GUIAPP_TEXT_MAX);
            ime_cand_consume[i] = ime_cand_consume[i - 1];
        }
        slot = 0;
        ime_cand_count++;
    } else {
        ime_cand_count++;
    }
    for (int i = 0; i < n; i++)
        ime_cands[slot][i] = text[i];
    ime_cands[slot][n] = 0;
    ime_cand_consume[slot] = (uint8_t)(consume > 255 ? 255 : consume);
}

static void ime_push_items(const char *items, int consume, int prefer_front) {
    if (!items)
        return;
    while (*items && ime_cand_count < IME_CAND_CAP) {
        while (*items == ' ')
            items++;
        if (!*items)
            break;
        const char *start = items;
        while (*items && *items != ' ')
            items++;
        int n = (int)(items - start);
        if (n <= 0)
            continue;
        char tmp[GUIAPP_TEXT_MAX];
        if (n >= GUIAPP_TEXT_MAX)
            n = GUIAPP_TEXT_MAX - 1;
        for (int i = 0; i < n; i++)
            tmp[i] = start[i];
        tmp[n] = 0;
        ime_cand_push(tmp, consume, prefer_front);
        prefer_front = 0; /* only the first item of a phrase row is prioritized */
    }
}

/*
 * Build the candidate list for the current composition.
 *
 * Matching policy (in priority order when inserting):
 *  1. Exact key == buffer          (full syllable / full phrase)
 *  2. Key is a prefix of buffer    (continuous: "nihao" hits "nihao","ni","hao"…)
 *     Longer keys first so phrases beat single syllables.
 *  3. Buffer is a prefix of key    (still typing: "zho" → "zhong")
 */
static void ime_rebuild_candidates(void) {
    ime_cand_count = 0;
    ime_page = 0;
    if (ime_length <= 0)
        return;

    struct match {
        int index;
        int key_len;
        int kind; /* 0 exact, 1 key-prefix-of-buf, 2 buf-prefix-of-key */
    } matches[IME_MATCH_CAP];
    int match_count = 0;

    int start = pinyin_lower_bound(ime_buffer, 1);
    for (int i = start; i < PINYIN_ENTRY_COUNT && match_count < IME_MATCH_CAP; i++) {
        const char *key = pinyin_entries[i].key;
        if (key[0] != ime_buffer[0])
            break;
        int klen = (int)strlen(key);
        int kind = -1;
        if (klen == ime_length && memcmp(key, ime_buffer, (size_t)klen) == 0)
            kind = 0;
        else if (klen <= ime_length &&
                 memcmp(key, ime_buffer, (size_t)klen) == 0)
            kind = 1;
        else if (klen > ime_length &&
                 memcmp(key, ime_buffer, (size_t)ime_length) == 0)
            kind = 2;
        if (kind < 0)
            continue;
        matches[match_count].index = i;
        matches[match_count].key_len = klen;
        matches[match_count].kind = kind;
        match_count++;
    }

    /* Sort matches: kind asc, then longer keys first for kind 0/1. */
    for (int a = 1; a < match_count; a++) {
        struct match v = matches[a];
        int b = a;
        while (b > 0) {
            struct match p = matches[b - 1];
            int better = 0;
            if (v.kind < p.kind)
                better = 1;
            else if (v.kind == p.kind && v.key_len > p.key_len)
                better = 1;
            if (!better)
                break;
            matches[b] = p;
            b--;
        }
        matches[b] = v;
    }

    for (int m = 0; m < match_count; m++) {
        const struct pinyin_entry *e = &pinyin_entries[matches[m].index];
        int prefer = matches[m].kind == 0 ||
                     (matches[m].kind == 1 && matches[m].key_len == ime_length);
        /* Incomplete syllables (kind 2) replace the whole composition. */
        int consume = matches[m].kind == 2 ? ime_length : matches[m].key_len;
        ime_push_items(e->items, consume, prefer && m == 0);
    }
}

static int ime_page_count(void) {
    if (ime_cand_count <= 0)
        return 1;
    return (ime_cand_count + IME_PAGE_SIZE - 1) / IME_PAGE_SIZE;
}

static void ime_clamp_page(void) {
    int pages = ime_page_count();
    if (ime_page < 0)
        ime_page = 0;
    if (ime_page >= pages)
        ime_page = pages - 1;
}

static int ime_candidate_at(int page_index, char out[GUIAPP_TEXT_MAX]) {
    int abs = ime_page * IME_PAGE_SIZE + page_index;
    if (abs < 0 || abs >= ime_cand_count)
        return 0;
    copy_text(out, ime_cands[abs], GUIAPP_TEXT_MAX);
    return 1;
}

static int ime_candidate_consume(int page_index) {
    int abs = ime_page * IME_PAGE_SIZE + page_index;
    if (abs < 0 || abs >= ime_cand_count)
        return ime_length;
    return (int)ime_cand_consume[abs];
}

/* Focused app that can receive typed text (IME target). */
static int find_caret_window(void) {
    if (focus < WIN_APP_BASE || focus >= WIN_COUNT)
        return -1;
    if (!windows[focus].visible || windows[focus].minimized)
        return -1;
    int slot = focus - WIN_APP_BASE;
    if (slot < 0 || slot >= MAX_GUI_APPS || !app_sessions[slot].used)
        return -1;
    return focus;
}

/* Screen-space caret rect for IME placement (OS-style: near text cursor). */
static struct rect get_caret_area(void) {
    int id = find_caret_window();
    if (id < 0)
        return (struct rect){sw / 2 - 40, sh / 2 - 20, 80, 28};
    int slot = id - WIN_APP_BASE;
    struct rect content = content_rect(id);
    if (app_sessions[slot].caret_valid) {
        int cx = content.x + app_sessions[slot].caret_x - scroll_x[id];
        int cy = content.y + app_sessions[slot].caret_y - scroll_y[id];
        return (struct rect){cx, cy, 2, KFONT_HEIGHT + 4};
    }
    /* Fallback before the app reports a caret: bottom of content. */
    return (struct rect){content.x + 12, content.y + content.h - 28,
                         80, 24};
}

static void ime_panel_text(char *comp, size_t comp_cap,
                           char *cands, size_t cands_cap) {
    copy_text(comp, ime_buffer, comp_cap);
    if (ime_cand_count > IME_PAGE_SIZE) {
        append_text(comp, "  (", comp_cap);
        append_uint(comp, (unsigned int)(ime_page + 1), comp_cap);
        append_text(comp, "/", comp_cap);
        append_uint(comp, (unsigned int)ime_page_count(), comp_cap);
        append_text(comp, ")", comp_cap);
    }

    cands[0] = 0;
    for (int i = 0; i < IME_PAGE_SIZE; i++) {
        char item[GUIAPP_TEXT_MAX];
        char number[4] = {(char)('1' + i), '.', 0, 0};
        if (!ime_candidate_at(i, item))
            break;
        if (i)
            append_text(cands, "  ", cands_cap);
        append_text(cands, number, cands_cap);
        append_text(cands, item, cands_cap);
    }
    if (!cands[0])
        copy_text(cands, "(no match — Space commits pinyin)", cands_cap);
}

static struct rect ime_panel_rect_for(const char *comp, const char *cands) {
    struct rect caret;
    int need_w, panel_w, panel_h, panel_x, panel_y;

    need_w = max_i(ui_text_width(comp, UI_FONT_BODY),
                   ui_text_width(cands, UI_FONT_BODY)) + 32;
    panel_w = min_i(sw - 24, max_i(300, need_w));
    panel_h = 60;

    /* OS-style: float next to the text caret of the focused app. */
    caret = get_caret_area();
    panel_x = caret.x;
    panel_y = caret.y + caret.h + 6;
    if (panel_x + panel_w > sw - 8)
        panel_x = sw - 8 - panel_w;
    if (panel_x < 8)
        panel_x = 8;
    if (panel_y + panel_h > sh - 8)
        panel_y = caret.y - panel_h - 6;
    if (panel_y < work_area().y + 4)
        panel_y = work_area().y + 4;
    if (panel_y + panel_h > sh - 8)
        panel_y = sh - 8 - panel_h;
    return (struct rect){panel_x, panel_y, panel_w, panel_h};
}

static struct rect ime_panel_rect(void) {
    char comp[96];
    char cands[192];
    if (!ime_enabled || ime_length == 0)
        return (struct rect){0, 0, 0, 0};
    ime_panel_text(comp, sizeof(comp), cands, sizeof(cands));
    return ime_panel_rect_for(comp, cands);
}

static void draw_ime(void) {
    struct ui_surface s = ui_target();
    char comp[96];
    char cands[192];
    struct rect panel;
    struct ui_rect p;

    /* The tray badge is painted by draw_taskbar; only the composition panel
     * is drawn here, and only while composing. */
    if (!ime_enabled || ime_length == 0)
        return;

    ime_panel_text(comp, sizeof(comp), cands, sizeof(cands));
    panel = ime_panel_rect_for(comp, cands);
    p = ui_of(panel);

    ui_shadow(&s, p, UI_RADIUS_OVERLAY, UI_ELEV_FLYOUT_R,
              UI_ELEV_FLYOUT_A, 3);
    shell_acrylic(&s, p, UI_RADIUS_OVERLAY, UI_BG_ACRYLIC, 215);
    ui_stroke_round(&s, p, UI_RADIUS_OVERLAY, 1, UI_STROKE_SURFACE, 255);
    ui_text_in(&s, ui_rect_make(panel.x + 12, panel.y + 6, panel.w - 24, 24),
               comp, ui_style(UI_FONT_BODY, UI_ACCENT_FILL));
    ui_text_in(&s, ui_rect_make(panel.x + 12, panel.y + 30, panel.w - 24, 24),
               cands, ui_style(UI_FONT_BODY, UI_TEXT_PRIMARY));
}

static struct rect context_menu_rect(void) {
    struct rect menu = {context_x, context_y, CONTEXT_MENU_W, CONTEXT_MENU_H};
    if (!context_open)
        return (struct rect){0, 0, 0, 0};
    if (menu.x + menu.w > sw) menu.x = sw - menu.w;
    if (menu.y + menu.h > sh) menu.y = sh - menu.h;
    return menu;
}

/* Themed context menu: an acrylic flyout with hover rows, rather than a stack
 * of framed buttons. */
static void draw_context_menu(void) {
    static const char *labels[] = {"Copy", "Paste", "Cut"};
    static const uint8_t icons[] = {UI_ICON_DOCUMENT, UI_ICON_PLUS,
                                    UI_ICON_MINUS};
    struct ui_surface s;
    struct rect menu;
    struct ui_rect m;

    if (!context_open)
        return;
    s = ui_target();
    menu = context_menu_rect();
    context_x = menu.x; context_y = menu.y;
    m = ui_of(menu);

    ui_shadow(&s, m, UI_RADIUS_OVERLAY, UI_ELEV_FLYOUT_R,
              UI_ELEV_FLYOUT_A, 3);
    shell_acrylic(&s, m, UI_RADIUS_OVERLAY, UI_BG_ACRYLIC, 210);
    ui_stroke_round(&s, m, UI_RADIUS_OVERLAY, 1, UI_STROKE_SURFACE, 255);

    for (int i = 0; i < 3; i++) {
        struct rect row = {menu.x + 4, menu.y + 4 + i * CONTEXT_ITEM_STEP,
                           menu.w - 8, CONTEXT_ITEM_H};
        struct ui_rect rr = ui_of(row);
        int disabled = i == 1 && !clipboard[0];
        int hovered = !disabled && inside(pointer_x, pointer_y, row);
        int pressed = hovered && (prev_buttons & 1);
        uint32_t fg = disabled ? UI_TEXT_DISABLED : UI_TEXT_PRIMARY;
        if (pressed)
            ui_fill_round(&s, rr, UI_RADIUS_CONTROL, UI_SUBTLE_PRESSED);
        else if (hovered)
            ui_fill_round(&s, rr, UI_RADIUS_CONTROL, UI_SUBTLE_HOVER);
        ui_icon(&s, icons[i], row.x + 10, row.y + (row.h - 16) / 2, 16, fg,
                disabled ? 150 : 255);
        ui_text_in(&s, ui_rect_make(row.x + 36, row.y, row.w - 44, row.h),
                   labels[i], ui_style(UI_FONT_BODY, fg));
    }
}

static void draw_pointer(void) {
    static const uint16_t arrow[16] = {
        0x8000,0xC000,0xE000,0xF000,0xF800,0xFC00,0xFE00,0xFF00,
        0xFF80,0xF800,0xDC00,0x8C00,0x0600,0x0600,0x0300,0x0300
    };
    if (hardware_cursor_ready)
        return;
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            if (!(arrow[y] & (0x8000u >> x)))
                continue;
            int edge = x == 0 || y == 0 ||
                       !(arrow[y] & (0x8000u >> (x + 1))) ||
                       (y + 1 < 16 && !(arrow[y + 1] & (0x8000u >> x)));
            pixel(pointer_x + x, pointer_y + y, edge ? 0x000000u : 0xFFFFFFu);
        }
    }
}

static void compose_scene(void) {
    if (compose_pass != COMPOSE_GPU_OVERLAY) {
        draw_background();
        for (int i = 0; i < WIN_COUNT; i++) {
            int id = z_order[i];
            if (id == WIN_LAUNCHER)
                draw_launcher();
            else if (id == WIN_STATUS)
                draw_status();
            else if (id >= WIN_APP_BASE)
                draw_app_window(id);
        }
    }
    if (compose_pass != COMPOSE_GPU_BASE) {
        /* These controls live in the transparent GPU overlay.  Their acrylic
         * backdrops are inserted by gpu_present_scene before this layer. */
        draw_snap_preview();
        draw_start_menu();
        draw_taskbar();
        draw_ime();
        draw_context_menu();
        draw_pointer();
    }
}

static int bind_scanout(void) {
    struct gfx_surface_map map;
    fb = fb_local;
    fb_stride = MAX_SW;
    scanout_direct = 0;
    /* LFB writes are immediately visible: drawing the background and each
     * window there exposes incomplete frames. Compose in RAM, then blit the
     * finished damage. VirtIO backing memory has an explicit present step. */
    if (display_backend == GFX_BACKEND_FRAMEBUFFER)
        return 0;
    if (gfx_map_surface(&map) == 0 && map.pixels &&
        map.width >= (uint32_t)sw && map.height >= (uint32_t)sh &&
        map.stride_pixels >= (uint32_t)sw) {
        fb = map.pixels;
        fb_stride = (int)map.stride_pixels;
        scanout_direct = 1;
        display_backend = map.backend;
        return 0;
    }
    return -1;
}

/* ---- GPU presentation --------------------------------------------------
 *
 * When virgl is available the composed frame is handed to the host GPU as a
 * texture and drawn as a quad, rather than copied to the scanout by the CPU.
 *
 * This is the conservative half of GPU compositing, and it is deliberate: the
 * scene is still composed into `fb` by the software path, so every existing
 * rule in docs/user-gui.md about damage, clipping and live resize continues to
 * hold unchanged, and a virgl-less device keeps working by falling straight
 * back to gfx_present.  What it buys is that the per-frame scanout copy --
 * which on a 1280x720 desktop is 3.6 MB of CPU memcpy every full redraw --
 * becomes a DMA the host GPU performs, and the frame arrives on a surface the
 * GPU can filter rather than one the CPU must resample.
 *
 * Only the damaged sub-rect is uploaded, so a blinking caret costs a few
 * hundred bytes rather than a full screen.
 */
enum {
    GPU_FRAME_LAYER = 1,
    GPU_APP_LAYER_BASE = 2,
    GPU_APP_BACK_LAYER_BASE = GPU_APP_LAYER_BASE + MAX_GUI_APPS,
    GPU_OVERLAY_LAYER = GPU_APP_BACK_LAYER_BASE + MAX_GUI_APPS,
    GPU_SCENE_LAYER,
    GPU_BLUR_PING_LAYER,
    GPU_BLUR_PONG_LAYER,
};

_Static_assert(GPU_BLUR_PONG_LAYER < GPUCOMP_MAX_LAYERS,
               "GPU compositor layer table is too small");

static int gpu_app_layer_bank(int slot, int bank) {
    return (bank ? GPU_APP_BACK_LAYER_BASE : GPU_APP_LAYER_BASE) + slot;
}

static int gpu_app_layer(int slot) {
    return gpu_app_layer_bank(slot, app_sessions[slot].gpu_front_bank);
}

static int gpu_app_back_layer(int slot) {
    return gpu_app_layer_bank(slot, !app_sessions[slot].gpu_front_bank);
}

static void gpu_app_texture_release(int slot) {
    if (slot < 0 || slot >= MAX_GUI_APPS)
        return;
    (void)gpucomp_layer_release(gpu_app_layer_bank(slot, 0), 0);
    (void)gpucomp_layer_release(gpu_app_layer_bank(slot, 1), 0);
    app_sessions[slot].gpu_resource = 0;
    app_sessions[slot].gpu_pixels = 0;
    app_sessions[slot].gpu_resource_w = 0;
    app_sessions[slot].gpu_resource_h = 0;
    app_sessions[slot].gpu_content_w = 0;
    app_sessions[slot].gpu_content_h = 0;
    app_sessions[slot].gpu_resource_canvas = 0;
    app_sessions[slot].gpu_front_bank = 0;
}

/* Translate one validated application display list into an offscreen virgl
 * render target.  Command encoding is small; all pixel coverage, rounded
 * edges and glyph sampling happen on the host GPU. */
static int gpu_canvas_render(int slot) {
    struct app_session *session;
    int layer, result = 0;
    int glyph_budget = GUIAPP_CANVAS_STRING_BYTES;
    if (!gpu_present_ready || slot < 0 || slot >= MAX_GUI_APPS)
        return -1;
    session = &app_sessions[slot];
    layer = gpu_app_back_layer(slot);
    if (!session->used || !session->canvas_mode ||
        session->source_w <= 0 || session->source_h <= 0)
        return -1;
    {
        if (gpucomp_canvas_ensure(layer, session->source_w,
                                  session->source_h) < 0)
            return -1;
    }

    app_dirty_lock(slot);
    if (gpucomp_canvas_begin(layer) < 0) {
        app_dirty_unlock(slot);
        return -1;
    }
    for (uint16_t i = 0; i < session->canvas_count; i++) {
        const struct guiapp_canvas_command *command = &session->canvas[i];
        if (command->type == GUIAPP_CANVAS_RECT) {
            if (command->w <= 0 || command->h <= 0)
                continue;
            result = gpucomp_canvas_rect(layer, command->x, command->y,
                                         command->w, command->h,
                                         command->radius, command->color);
        } else if (command->type == GUIAPP_CANVAS_LINE) {
            if (command->radius <= 0)
                continue;
            result = gpucomp_canvas_line(layer, command->x, command->y,
                                         command->w, command->h,
                                         command->radius, command->color);
        } else if (command->type == GUIAPP_CANVAS_TEXT) {
            uint32_t end = (uint32_t)command->text_offset +
                           (uint32_t)command->text_length;
            if (command->w <= 0 || command->h <= 0 ||
                end > session->canvas_string_bytes)
                continue;
            int text_length = command->text_length;
            if (text_length > glyph_budget)
                text_length = glyph_budget;
            if (text_length <= 0)
                continue;
            glyph_budget -= text_length;
            result = gpucomp_canvas_text(
                layer, command->x, command->y, command->w, command->h,
                session->canvas_strings + command->text_offset,
                text_length, command->aux, command->color,
                command->flags & 3u,
                (command->flags & GUIAPP_CANVAS_TEXT_BOLD) != 0);
        }
        if (result < 0)
            break;
    }
    if (result == 0)
        result = gpucomp_canvas_end();
    /* Publish the texture geometry only after every render command has been
     * accepted.  gpu_draw_app therefore cannot combine a new application
     * size with the previous completed canvas. */
    if (result == 0) {
        session->gpu_front_bank = !session->gpu_front_bank;
        session->gpu_resource = gpucomp_layer_resource(layer);
        session->gpu_pixels = 0;
        (void)gpucomp_layer_capacity(layer, &session->gpu_resource_w,
                                    &session->gpu_resource_h);
        session->gpu_content_w = session->source_w;
        session->gpu_content_h = session->source_h;
        session->gpu_resource_canvas = 1;
    }
    app_dirty_unlock(slot);
    return result;
}

/* Snapshot a CPU-rendered app surface into compositor-owned GPU backing.
 *
 * Importing the writable SHM pages directly looked attractive, but a resize
 * changes their row stride while the app is publishing.  TRANSFER_TO_HOST_3D
 * can then consume half of one generation and half of the next; detecting the
 * sequence change afterwards is too late because the host texture has already
 * been modified.  A private texture makes the seqlock effective: copy first,
 * verify, and only then let the host read immutable backing. */
static int gpu_app_texture_sync(int slot, struct rect dirty) {
    struct app_session *session;
    int width, height, stride, layer, swap_bank;
    uint32_t sequence;
    uint32_t *destination;
    const uint32_t *source;
    if (!gpu_present_ready || slot < 0 || slot >= MAX_GUI_APPS ||
        !app_sessions[slot].used || !app_sessions[slot].shared)
        return -1;
    session = &app_sessions[slot];
    width = session->source_w;
    height = session->source_h;
    if (width <= 0 || height <= 0)
        return -1;
    if (session->canvas_mode)
        return gpu_canvas_render(slot);

    source = (const uint32_t *)((const uint8_t *)session->shared +
                                GUIAPP_SHARED_HEADER_SIZE);
    /* Only consume the exact generation whose frame metadata the reader
     * accepted.  Width is also the SHM row stride, so a mismatch must never
     * be interpreted using the current texture dimensions. */
    sequence = session->shared->sequence;
    if ((sequence & 1u) || sequence != session->last_sequence ||
        session->shared->width != (uint32_t)width ||
        session->shared->height != (uint32_t)height)
        return 1;
    __sync_synchronize();

    swap_bank = !session->gpu_resource || session->gpu_resource_canvas ||
                session->gpu_content_w != width ||
                session->gpu_content_h != height;
    layer = swap_bank ? gpu_app_back_layer(slot) : gpu_app_layer(slot);
    if (swap_bank) {
        if (gpucomp_layer_ensure(layer, width, height) < 0)
            return -1;
        destination = gpucomp_layer_pixels(layer, &stride);
        dirty = (struct rect){0, 0, width, height};
    } else {
        destination = session->gpu_pixels;
        stride = session->gpu_resource_w;
    }
    dirty = intersect_rect(dirty, (struct rect){0, 0, width, height});
    if (dirty.w <= 0 || dirty.h <= 0)
        return 0;

    if (!destination || stride < width) {
        return -1;
    }

    for (int row = 0; row < dirty.h; row++) {
        memcpy(destination + (size_t)(dirty.y + row) * (size_t)stride +
                                    (size_t)dirty.x,
               source + (size_t)(dirty.y + row) * (size_t)width +
                        (size_t)dirty.x,
               (size_t)dirty.w * sizeof(uint32_t));
    }
    __sync_synchronize();
    if (session->shared->sequence != sequence) {
        /* The private copy may be mixed, but it has not reached the host.
         * Recopy the whole stable generation because the newer dirty region
         * need not overlap the one we just attempted. */
        app_note_dirty(slot, (struct rect){0, 0, width, height});
        return 1;
    }

    if (gpucomp_layer_source_size(layer, width, height) < 0 ||
        gpucomp_upload_rect(layer, dirty.x, dirty.y, dirty.w, dirty.h) < 0)
        return -1;
    if (swap_bank) {
        session->gpu_front_bank = !session->gpu_front_bank;
        session->gpu_resource = gpucomp_layer_resource(layer);
        session->gpu_pixels = gpucomp_layer_pixels(layer, &stride);
        (void)gpucomp_layer_capacity(layer, &session->gpu_resource_w,
                                    &session->gpu_resource_h);
    }
    session->gpu_resource_canvas = 0;
    session->gpu_content_w = width;
    session->gpu_content_h = height;
    return 0;
}

static void gpu_canvas_capability_set(int enabled) {
    for (int slot = 0; slot < MAX_GUI_APPS; slot++) {
        if (!app_sessions[slot].used || !app_sessions[slot].shared)
            continue;
        uint32_t old = app_sessions[slot].shared->capabilities;
        uint32_t next = enabled ? old | GUIAPP_CAP_GPU_CANVAS
                                : old & ~GUIAPP_CAP_GPU_CANVAS;
        if (old == next)
            continue;
        app_sessions[slot].shared->capabilities = next;
        __sync_synchronize();
        if (!enabled && app_sessions[slot].canvas_mode) {
            /* Do not interpret a display list as a software pixel surface
             * while the app prepares its fallback frame. */
            app_sessions[slot].canvas_mode = 0;
            app_sessions[slot].surface_w = 0;
            app_sessions[slot].surface_h = 0;
            win_damage(WIN_APP_BASE + slot);
        }
        (void)app_send_event(slot, GUIAPP_EVT_CAPABILITIES, 0, 0,
                             (int)next, 0, 0);
    }
}

/* Build the ARGB cursor once.  With virtio-gpu cursorq active, subsequent
 * mouse packets contain only MOVE_CURSOR -- no shell damage and no texture
 * upload. */
static void init_hardware_cursor(void) {
    static const uint16_t arrow[16] = {
        0x8000,0xC000,0xE000,0xF000,0xF800,0xFC00,0xFE00,0xFF00,
        0xFF80,0xF800,0xDC00,0x8C00,0x0600,0x0600,0x0300,0x0300
    };
    uint32_t pixels[POINTER_W * POINTER_H];
    memset(pixels, 0, sizeof(pixels));
    for (int y = 0; y < POINTER_H; y++) {
        for (int x = 0; x < POINTER_W; x++) {
            if (!(arrow[y] & (0x8000u >> x)))
                continue;
            int edge = x == 0 || y == 0 ||
                       !(arrow[y] & (0x8000u >> (x + 1))) ||
                       (y + 1 < POINTER_H &&
                        !(arrow[y + 1] & (0x8000u >> x)));
            pixels[y * POINTER_W + x] =
                edge ? 0xFF000000u : 0xFFFFFFFFu;
        }
    }
    hardware_cursor_ready =
        gfx_cursor_define(pixels, POINTER_W, POINTER_H, 0, 0,
                          pointer_x, pointer_y) == 0;
    if (hardware_cursor_ready) {
        pointer_drawn_valid = 0;
        gui_log("[gui] virtio-gpu hardware cursor enabled");
    }
}

static void gpu_present_init(void) {
    int stride = 0, blur_w, blur_h;
    uint32_t *texture, *overlay;

    gpu_present_ready = 0;
    gpu_overlay_pixels = 0;
    gpu_overlay_stride = 0;
    gpu_blur_valid = 0;
    compose_pass = COMPOSE_ALL;
    if (gpucomp_init(sw, sh) < 0) {
        gpu_canvas_capability_set(0);
        return;
    }
    blur_w = (sw + 3) / 4;
    blur_h = (sh + 3) / 4;
    if (gpucomp_layer_ensure(GPU_FRAME_LAYER, sw, sh) < 0 ||
        gpucomp_layer_ensure_format(GPU_OVERLAY_LAYER, sw, sh,
                                    VIRGL_FORMAT_B8G8R8A8_UNORM) < 0 ||
        gpucomp_target_ensure(GPU_SCENE_LAYER, sw, sh) < 0 ||
        gpucomp_target_ensure(GPU_BLUR_PING_LAYER, blur_w, blur_h) < 0 ||
        gpucomp_target_ensure(GPU_BLUR_PONG_LAYER, blur_w, blur_h) < 0)
        goto fail;
    texture = gpucomp_layer_pixels(GPU_FRAME_LAYER, &stride);
    overlay = gpucomp_layer_pixels(GPU_OVERLAY_LAYER, &gpu_overlay_stride);
    if (!texture || stride <= 0 || !overlay || gpu_overlay_stride <= 0)
        goto fail;
    memset(overlay, 0, (size_t)gpu_overlay_stride * (size_t)sh *
                       sizeof(uint32_t));
    /* Base -> application surfaces -> GPU acrylic -> transparent overlay. */
    gpucomp_layer_place(GPU_FRAME_LAYER, 0, 0, sw, sh, 0, 255);
    gpucomp_layer_place(GPU_SCENE_LAYER, 0, 0, sw, sh, 0, 255);
    gpucomp_layer_place(GPU_OVERLAY_LAYER, 0, 0, sw, sh, 0, 255);
    if (gpu3d_scanout(1) < 0)
        goto fail;
    /* Compose directly into the texture's mapped backing.  Without this the
     * compositor would still be writing the zero-copy scanout that the GPU is
     * about to overwrite -- reading a surface while presenting onto it -- and
     * it also removes the intermediate copy entirely: the scene is built
     * straight into the memory the upload reads from. */
    fb = texture;
    fb_stride = stride;
    gpu_overlay_pixels = overlay;
    compose_pass = COMPOSE_GPU_BASE;
    scanout_direct = 0;
    gpu_present_ready = 1;
    gpu_canvas_capability_set(1);
    gui_log("[gui] virgl present + GPU acrylic enabled");
    return;

fail:
    gpucomp_shutdown();
    gpu_overlay_pixels = 0;
    gpu_overlay_stride = 0;
    gpu_blur_valid = 0;
    compose_pass = COMPOSE_ALL;
    gpu_canvas_capability_set(0);
}

/* Release GPU resources.  Does not rebind the compose buffer: the caller
 * decides what fb should point at next, since the two call sites want
 * different things (shutdown wants nothing, a mode change wants a rebind
 * against the new geometry). */
static void gpu_present_shutdown(void) {
    if (!gpu_present_ready)
        return;
    gpu_present_ready = 0;
    for (int slot = 0; slot < MAX_GUI_APPS; slot++)
        gpu_app_texture_release(slot);
    gpucomp_shutdown();
    gpu_overlay_pixels = 0;
    gpu_overlay_stride = 0;
    gpu_blur_valid = 0;
    compose_pass = COMPOSE_ALL;
}

enum { GPU_VISIBLE_RECTS_MAX = 96 };

/* Subtract one opaque screen rectangle from a list of visible rectangles.
 * Each overlap becomes at most four non-overlapping strips. */
static int gpu_visible_subtract(struct rect *rects, int count,
                                struct rect cut) {
    struct rect out[GPU_VISIBLE_RECTS_MAX];
    int out_count = 0;
    if (cut.w <= 0 || cut.h <= 0)
        return count;
    for (int i = 0; i < count; i++) {
        struct rect r = rects[i];
        struct rect hit = intersect_rect(r, cut);
        struct rect pieces[4] = {
            {r.x, r.y, r.w, hit.y - r.y},
            {r.x, hit.y + hit.h, r.w,
             r.y + r.h - (hit.y + hit.h)},
            {r.x, hit.y, hit.x - r.x, hit.h},
            {hit.x + hit.w, hit.y,
             r.x + r.w - (hit.x + hit.w), hit.h},
        };
        if (hit.w <= 0 || hit.h <= 0) {
            if (out_count >= GPU_VISIBLE_RECTS_MAX)
                return -1;
            out[out_count++] = r;
            continue;
        }
        for (int p = 0; p < 4; p++) {
            if (pieces[p].w <= 0 || pieces[p].h <= 0)
                continue;
            if (out_count >= GPU_VISIBLE_RECTS_MAX)
                return -1;
            out[out_count++] = pieces[p];
        }
    }
    for (int i = 0; i < out_count; i++)
        rects[i] = out[i];
    return out_count;
}

static int gpu_visible_cut(struct rect *rects, int count, struct rect cut) {
    return count < 0 ? count : gpu_visible_subtract(rects, count, cut);
}

/* Draw one imported application surface, clipped against all higher shell
 * geometry.  Scissoring keeps the original quad/UV transform intact. */
static void gpu_draw_app(int slot, struct rect damage) {
    struct app_session *session;
    struct rect content, destination, visible[GPU_VISIBLE_RECTS_MAX];
    int id, zpos = -1, count;
    if (slot < 0 || slot >= MAX_GUI_APPS)
        return;
    session = &app_sessions[slot];
    id = WIN_APP_BASE + slot;
    if (!session->used || !session->gpu_resource ||
        session->gpu_content_w <= 0 || session->gpu_content_h <= 0 ||
        !windows[id].visible || windows[id].minimized)
        return;
    content = motion_paint_rect(id, content_rect(id));
    if (session->scaled_surface) {
        destination = motion_paint_rect(id, scaled_view_rect_for(id, session->gpu_content_w,
                                           session->gpu_content_h));
    } else {
        /* Desktop UI is pixel-sized content, not a video surface.  Keep it
         * 1:1 while resize configures are in flight; stretching each lagging
         * intermediate frame to the newest window geometry makes glyphs
         * visibly pulse as the scale ratio repeatedly changes.  The opaque
         * shell layer already fills any newly exposed content margin. */
        destination = (struct rect){content.x, content.y,
                                    session->gpu_content_w,
                                    session->gpu_content_h};
    }
    visible[0] = intersect_rect(intersect_rect(destination, content), damage);
    if (visible[0].w <= 0 || visible[0].h <= 0)
        return;
    count = 1;

    for (int zi = 0; zi < WIN_COUNT; zi++)
        if (z_order[zi] == id) {
            zpos = zi;
            break;
        }
    for (int zi = zpos + 1; zi < WIN_COUNT && count > 0; zi++) {
        int above = z_order[zi];
        if (windows[above].visible && !windows[above].minimized)
            count = gpu_visible_cut(visible, count, motion_paint_rect(above, windows[above].r));
    }

    /* Taskbar, menus, IME, snap preview and the software-cursor fallback are
     * now a real alpha overlay drawn after every app, so app surfaces no
     * longer need to be fragmented around those rectangles. */
    if (count <= 0)
        return;

    gpucomp_layer_place(gpu_app_layer(slot), destination.x, destination.y,
                        destination.w, destination.h, 0, 255);
    for (int i = 0; i < count; i++)
        gpucomp_draw_layer_scissored(gpu_app_layer(slot), visible[i].x,
                                     visible[i].y, visible[i].w,
                                     visible[i].h);
}

struct gpu_acrylic_region {
    struct rect r;
    int radius;
    uint32_t tint;
    int alpha;
};

static int gpu_acrylic_regions(struct gpu_acrylic_region *out, int capacity) {
    int count = 0;
#define ADD_ACRYLIC(rect_value, rad_value, tint_value, alpha_value) do { \
        struct rect add_r = (rect_value); \
        if (add_r.w > 0 && add_r.h > 0 && count < capacity) { \
            out[count].r = add_r; \
            out[count].radius = (rad_value); \
            out[count].tint = (tint_value); \
            out[count].alpha = (alpha_value); \
            count++; \
        } \
    } while (0)
    if (taskbar_expanded) {
        int ids[MAX_GUI_APPS];
        int hidden = taskbar_hidden_windows(ids);
        if (hidden > 0)
            ADD_ACRYLIC(taskbar_overflow_panel(hidden), UI_RADIUS_OVERLAY,
                        UI_BG_ACRYLIC, 205);
    }
    ADD_ACRYLIC(ime_panel_rect(), UI_RADIUS_OVERLAY, UI_BG_ACRYLIC, 215);
    ADD_ACRYLIC(context_menu_rect(), UI_RADIUS_OVERLAY,
                UI_BG_ACRYLIC, 210);
#undef ADD_ACRYLIC
    return count;
}

static int gpu_acrylic_intersects(struct rect area) {
    struct gpu_acrylic_region regions[6];
    int count = gpu_acrylic_regions(regions, 6);
    for (int i = 0; i < count; i++) {
        struct rect hit = intersect_rect(area, regions[i].r);
        if (hit.w > 0 && hit.h > 0)
            return 1;
    }
    return 0;
}

static int gpu_draw_acrylic_regions(struct rect area) {
    struct gpu_acrylic_region regions[6];
    int count = gpu_acrylic_regions(regions, 6);
    for (int i = 0; i < count; i++) {
        struct gpu_acrylic_region *r = &regions[i];
        if (gpucomp_draw_acrylic(GPU_BLUR_PING_LAYER,
                                 r->r.x, r->r.y, r->r.w, r->r.h,
                                 r->radius, r->tint, r->alpha,
                                 area.x, area.y, area.w, area.h) < 0)
            return -1;
    }
    return 0;
}

/* Update the retained scene, refresh the cached GPU blur when something
 * beneath an acrylic region changed, then assemble the scanout. */
static int gpu_present_scene(struct rect area) {
    struct rect screen = {0, 0, sw, sh};
    if (!gpu_present_ready)
        return -1;
    area = intersect_rect(area, screen);
    if (area.w <= 0 || area.h <= 0)
        return 0;

    /* A mode switch recreates the compositor while apps keep their SHM.
     * Lazily restore those imports without waiting for another app frame. */
    for (int slot = 0; slot < MAX_GUI_APPS; slot++) {
        struct app_session *session = &app_sessions[slot];
        if (!session->used || session->gpu_resource ||
            session->source_w <= 0 || session->source_h <= 0)
            continue;
        int result = gpu_app_texture_sync(
            slot, (struct rect){0, 0, session->source_w, session->source_h});
        if (result < 0)
            return -1;
        if (result > 0)
            app_note_dirty(slot, (struct rect){0, 0, session->source_w,
                                               session->source_h});
    }

    if (gpucomp_target_begin(GPU_SCENE_LAYER, area.x, area.y,
                             area.w, area.h) < 0)
        return -1;
    gpucomp_draw_layer_scissored(GPU_FRAME_LAYER, area.x, area.y,
                                 area.w, area.h);
    for (int zi = 0; zi < WIN_COUNT; zi++) {
        int slot = app_slot_for_win(z_order[zi]);
        if (slot >= 0)
            gpu_draw_app(slot, area);
    }
    if (gpucomp_target_end() < 0)
        return -1;

    if (!gpu_blur_valid || gpu_acrylic_intersects(area)) {
        if (gpucomp_blur_rebuild(GPU_SCENE_LAYER, GPU_BLUR_PING_LAYER,
                                 GPU_BLUR_PONG_LAYER) < 0)
            return -1;
        gpu_blur_valid = 1;
    }

    gpucomp_begin();
    gpucomp_draw_layer_scissored(GPU_SCENE_LAYER, area.x, area.y,
                                 area.w, area.h);
    if (gpu_draw_acrylic_regions(area) < 0)
        return -1;
    gpucomp_draw_layer_scissored(GPU_OVERLAY_LAYER, area.x, area.y,
                                 area.w, area.h);
    return gpucomp_end(area.x, area.y, area.w, area.h);
}

static void gpu_overlay_clear(struct rect area) {
    area = intersect_rect(area, (struct rect){0, 0, sw, sh});
    if (!gpu_overlay_pixels || area.w <= 0 || area.h <= 0)
        return;
    for (int y = area.y; y < area.y + area.h; y++)
        memset(gpu_overlay_pixels + (size_t)y * gpu_overlay_stride + area.x,
               0, (size_t)area.w * sizeof(uint32_t));
}

/* Repaint the opaque base and transparent chrome overlay separately.
 * Application contents never enter either CPU buffer. */
static int gpu_shell_update(struct rect area) {
    int base_stride, result;
    uint32_t *base = gpucomp_layer_pixels(GPU_FRAME_LAYER, &base_stride);
    if (!base || base_stride <= 0 || !gpu_overlay_pixels)
        return -1;
    compose_skip_app_pixels = 1;
    compose_clip = area;
    compose_pass = COMPOSE_GPU_BASE;
    fb = base;
    fb_stride = base_stride;
    compose_scene();
    result = gpucomp_upload_rect(GPU_FRAME_LAYER, area.x, area.y,
                                 area.w, area.h);
    if (result < 0)
        goto done;

    gpu_overlay_clear(area);
    compose_pass = COMPOSE_GPU_OVERLAY;
    fb = gpu_overlay_pixels;
    fb_stride = gpu_overlay_stride;
    compose_scene();
    result = gpucomp_upload_rect(GPU_OVERLAY_LAYER, area.x, area.y,
                                 area.w, area.h);

done:
    fb = base;
    fb_stride = base_stride;
    compose_pass = COMPOSE_GPU_BASE;
    compose_clip = (struct rect){0, 0, sw, sh};
    compose_skip_app_pixels = 0;
    return result;
}

static void gpu_fallback_to_software(const char *reason) {
    gpu_present_shutdown();
    (void)bind_scanout();
    gpu_canvas_capability_set(0);
    desktop_dirty = 1;
    gui_log(reason ? reason :
            "[gui] virgl compositor failed; software fallback");
}

static int render_region(struct rect area) {
    area = intersect_rect(area, (struct rect){0, 0, sw, sh});
    if (area.w <= 0 || area.h <= 0)
        return 0;
    if (gpu_present_ready) {
        if (gpu_shell_update(area) < 0)
            gpu_fallback_to_software(
                "[gui] virgl shell upload failed; software fallback");
        else if (gpu_present_scene(area) < 0)
            gpu_fallback_to_software(
                "[gui] virgl scene/present failed; software fallback");
        else
            return 0;
        /* The old 2-D scanout may be stale after 3-D scanout was active. */
        area = (struct rect){0, 0, sw, sh};
    }
    compose_skip_app_pixels = 0;
    compose_clip = area;
    compose_scene();
    compose_clip = (struct rect){0, 0, sw, sh};
    if (scanout_direct)
        return gfx_present(area.x, area.y, area.w, area.h);
    /* Fallback: software backbuffer → kernel scanout copy. */
    return fb_blit_stride(area.x, area.y, area.w, area.h,
                          fb + area.y * fb_stride + area.x, fb_stride);
}

static void render(void) {
    (void)render_region((struct rect){0, 0, sw, sh});
}

static struct rect app_damage_to_screen(int slot, struct rect dirty) {
    int id = WIN_APP_BASE + slot;
    if (slot < 0 || slot >= MAX_GUI_APPS || !app_sessions[slot].used ||
        !windows[id].visible || windows[id].minimized)
        return (struct rect){0, 0, 0, 0};
    struct rect content = motion_paint_rect(id, content_rect(id));
    struct rect screen = (struct rect){0, 0, sw, sh};
    if (!app_sessions[slot].scaled_surface) {
        struct rect area = {
            content.x + dirty.x, content.y + dirty.y, dirty.w, dirty.h
        };
        return intersect_rect(area, intersect_rect(content, screen));
    }
    int source_w = app_sessions[slot].source_w;
    int source_h = app_sessions[slot].source_h;
    if (source_w <= 0 || source_h <= 0)
        return (struct rect){0, 0, 0, 0};
    dirty = intersect_rect(dirty, (struct rect){0, 0, source_w, source_h});
    if (dirty.w <= 0 || dirty.h <= 0)
        return (struct rect){0, 0, 0, 0};
    struct rect view = motion_paint_rect(id, scaled_view_rect(id, slot));
    int x1 = view.x + dirty.x * view.w / source_w;
    int y1 = view.y + dirty.y * view.h / source_h;
    int x2 = view.x +
        ((dirty.x + dirty.w) * view.w + source_w - 1) / source_w;
    int y2 = view.y +
        ((dirty.y + dirty.h) * view.h + source_h - 1) / source_h;
    struct rect area = {x1 - 1, y1 - 1, x2 - x1 + 2, y2 - y1 + 2};
    return intersect_rect(area, intersect_rect(view, screen));
}

static int top_window_at(int x, int y) {
    for (int zi = WIN_COUNT - 1; zi >= 0; zi--) {
        int i = z_order[zi];
        /* Start paints through the bottom band while its taskbar is hidden.
         * Its last visible tile/scrollbar pixels must remain interactive. */
        struct rect hit = i == WIN_LAUNCHER && metro_start_visible()
            ? (struct rect){0, 0, sw, sh} : windows[i].r;
        if (windows[i].visible && !windows[i].minimized && inside(x, y, hit))
            return i;
    }
    return -1;
}

static int hit_window_title(int x, int y) {
    int i = top_window_at(x, y);
    if (i < 0 || i == WIN_LAUNCHER)
        return -1;
    struct rect r = windows[i].r;
    struct rect title = {r.x, r.y, r.w, WINDOW_TITLE_H};
    return inside(x, y, title) ? i : -1;
}

static int hit_window(int x, int y) {
    return top_window_at(x, y);
}

static int hit_control(int x, int y, int *control_out) {
    int i = top_window_at(x, y);
    if (i < 0 || i == WIN_LAUNCHER)
        return -1;
    if (inside(x, y, control_hit_rect(i, 2))) {
        *control_out = 2;
        return i;
    }
    if (inside(x, y, control_hit_rect(i, 1))) {
        *control_out = 1;
        return i;
    }
    if (inside(x, y, control_hit_rect(i, 0))) {
        *control_out = 0;
        return i;
    }
    return -1;
}

static int hit_resize(int x, int y, int *edges_out) {
    for (int zi = WIN_COUNT - 1; zi >= 0; zi--) {
        int i = z_order[zi];
        if (i == WIN_LAUNCHER)
            continue;
        if (!windows[i].visible || windows[i].minimized)
            continue;
        struct rect r = windows[i].r;
        int side_pad = RESIZE_PAD + 2;
        int top_pad = 4;
        int bottom_pad = RESIZE_PAD + 6;
        int corner_pad = 24;
        int covered = inside(x, y, r);
        int in_title = covered && y >= r.y && y < r.y + WINDOW_TITLE_H;
        if (windows[i].maximized) {
            if (covered)
                return -1;
            continue;
        }
        if (x < r.x - side_pad || y < r.y - top_pad ||
            x >= r.x + r.w + side_pad || y >= r.y + r.h + bottom_pad) {
            if (covered)
                return -1;
            continue;
        }
        if (in_title && y >= r.y + top_pad)
            return -1;
        int edges = 0;
        if (x < r.x + side_pad)
            edges |= 1;
        if (x >= r.x + r.w - side_pad)
            edges |= 2;
        if (y < r.y + top_pad)
            edges |= 4;
        if (y >= r.y + r.h - bottom_pad)
            edges |= 8;
        if (x >= r.x + r.w - corner_pad && y >= r.y + r.h - corner_pad)
            edges |= 2 | 8;
        if (edges) {
            *edges_out = edges;
            return i;
        }
        if (covered)
            return -1;
    }
    return -1;
}

static void apply_resize(int id, int mx, int my) {
    struct rect r = windows[id].r;
    int min_w = window_min_width(id);
    int min_h = window_min_height(id);
    int dx = mx - resize_start_x;
    int dy = my - resize_start_y;
    if (dx == 0 && dy == 0)
        return;
    if (resize_edges & 1) {
        r.x += dx;
        r.w -= dx;
    }
    if (resize_edges & 2)
        r.w += dx;
    if (resize_edges & 4) {
        r.y += dy;
        r.h -= dy;
    }
    if (resize_edges & 8)
        r.h += dy;

    if (r.w < min_w) {
        if (resize_edges & 1)
            r.x -= min_w - r.w;
        r.w = min_w;
    }
    if (r.h < min_h) {
        if (resize_edges & 4)
            r.y -= min_h - r.h;
        r.h = min_h;
    }
    if (r.x < 0) {
        r.w += r.x;
        r.x = 0;
    }
    if (r.y < work_area().y) {
        r.h += r.y - work_area().y;
        r.y = work_area().y;
    }
    if (r.x + r.w > sw)
        r.w = sw - r.x;
    if (r.y + r.h > sh - TASKBAR_H)
        r.h = sh - TASKBAR_H - r.y;
    if (r.w < min_w)
        r.w = min_i(min_w, sw - r.x);
    if (r.h < min_h)
        r.h = min_i(min_h, sh - TASKBAR_H - r.y);

    windows[id].r = r;
    windows[id].restore = r;
    resize_start_x = mx;
    resize_start_y = my;
    resize_start_rect = r;
    int slot = app_slot_for_win(id);
    if (slot >= 0 && app_sessions[slot].used) {
        app_sessions[slot].resize_dirty = 1;
        publish_app_configure(slot);
    }
    clamp_scroll(id);
}

static void minimize_window(int id) {
    if (id < 0 || id >= WIN_COUNT || id == WIN_LAUNCHER)
        return;
    window_motion_cancel(id);
    windows[id].minimized = 1;
    windows[id].active = 0;
    for (int zi = WIN_COUNT - 1; zi >= 0; zi--) {
        int next = z_order[zi];
        if (windows[next].visible && !windows[next].minimized) {
            activate(next);
            return;
        }
    }
}

static void close_window(int id) {
    if (id < 0 || id >= WIN_COUNT || id == WIN_LAUNCHER)
        return;
    window_motion_cancel(id);
    for (int control = 0; control < 3; control++)
        ui_motion_reset(&caption_motion[id][control], 0);
    if (hover_app == id)
        hover_app = -1;
    int slot = app_slot_for_win(id);
    if (slot >= 0 && app_sessions[slot].used) {
        for (int k = 0; k < GUIAPP_KEY_COUNT; k++)
            if (key_owner[k] == slot + 1) key_owner[k] = 0;
        app_sessions[slot].closing = 1;
        (void)app_send_event(slot, GUIAPP_EVT_CLOSE, 0, 0, 0, 0, 0);
        close(app_sessions[slot].to_fd);
        if (app_sessions[slot].pid > 0) {
            int status;
            int reaped = 0;
            for (int attempt = 0; attempt < 50; attempt++) {
                int waited = waitpid(app_sessions[slot].pid, &status, WNOHANG);
                if (waited == app_sessions[slot].pid || waited < 0) {
                    reaped = 1;
                    break;
                }
                sleep_ms(2);
            }
            if (!reaped) {
                (void)kill(app_sessions[slot].pid);
                (void)waitpid(app_sessions[slot].pid, &status, 0);
            }
        }
        if (app_sessions[slot].reader_tid > 0)
            (void)join(app_sessions[slot].reader_tid);
        close(app_sessions[slot].from_fd);
        gpu_app_texture_release(slot);
        if (app_sessions[slot].shm_token)
            (void)shm_unmap(app_sessions[slot].shm_token);
        app_sessions[slot].used = 0;
        app_sessions[slot].pid = 0;
        app_sessions[slot].to_fd = -1;
        app_sessions[slot].from_fd = -1;
        app_sessions[slot].reader_tid = -1;
        app_sessions[slot].reader_dead = 0;
        app_sessions[slot].closing = 0;
        app_sessions[slot].wants_tick = 0;
        app_sessions[slot].shm_token = 0;
        app_sessions[slot].shared = 0;
    }
    windows[id].visible = 0;
    windows[id].minimized = 0;
    windows[id].active = 0;
    for (int zi = WIN_COUNT - 1; zi >= 0; zi--) {
        int next = z_order[zi];
        if (windows[next].visible && !windows[next].minimized) {
            activate(next);
            return;
        }
    }
}

/* User exits keep their app pixels alive until the visual deadline. Failure
 * cleanup, reader reaping and shutdown still use the immediate close path. */
static void request_window_exit(int id, int action) {
    if (id <= WIN_LAUNCHER || id >= WIN_COUNT || !windows[id].visible ||
        windows[id].minimized || window_exit[id]) return;
    window_motion_cancel(id);
    window_exit[id] = action;
    ui_motion_to(&window_motion[id], action == WINDOW_EXIT_CLOSE ? 32 : 48,
                 action == WINDOW_EXIT_CLOSE ? 150 : 180, monotonic_ms());
    win_damage(id);
}

static void finish_window_exits(void) {
    for (int id = WIN_STATUS; id < WIN_COUNT; id++) {
        int action = window_exit[id];
        if (!action) continue;
        window_motion_cancel(id);
        if (action == WINDOW_EXIT_CLOSE) close_window(id);
        else minimize_window(id);
        desktop_dirty = 1;
    }
}

static void reap_dead_apps(void) {
    for (int slot = 0; slot < MAX_GUI_APPS; slot++) {
        if (app_sessions[slot].used && app_sessions[slot].reader_dead) {
            gui_log("[gui] app protocol ended");
            close_window(WIN_APP_BASE + slot);
        }
    }
}

static void toggle_maximize(int id) {
    if (id < 0 || id >= WIN_COUNT || id == WIN_LAUNCHER)
        return;
    window_motion_cancel(id);
    if (windows[id].maximized) {
        windows[id].r = windows[id].restore;
        windows[id].maximized = 0;
    } else {
        windows[id].restore = windows[id].r;
        /* Fill the work area; the taskbar is reserved, everything else is
         * available. */
        windows[id].r = work_area();
        windows[id].maximized = 1;
    }
    clamp_scroll(id);
    int slot = app_slot_for_win(id);
    if (slot >= 0 && app_sessions[slot].used) {
        app_sessions[slot].resize_dirty = 1;
        (void)sync_app_size(id, 1);
    }
    ui_motion_reset(&window_motion[id], windows[id].maximized ? 16 : 24);
    window_motion_pending[id] = 1;
    win_damage(id);
}

static int hit_scrollbar(int x, int y, int *axis_out) {
    int i = top_window_at(x, y);
    if (i < 0)
        return -1;
    if (inside(x, y, vscroll_track(i)) && max_scroll_y(i) > 0) {
        *axis_out = 1;
        return i;
    }
    if (inside(x, y, hscroll_track(i)) && max_scroll_x(i) > 0) {
        *axis_out = 0;
        return i;
    }
    return -1;
}

/* Non-window taskbar hits, kept above the window-id range so callers can keep
 * testing `>= 0` for "a window was clicked". */
enum {
    TB_HIT_OVERFLOW = WIN_COUNT,
    TB_HIT_START,
    TB_HIT_IME,
    TB_HIT_CLOCK,
};

/* ---- Edge snap ---------------------------------------------------------
 *
 * the theme snaps a window when its title bar is dragged into a screen edge:
 * the top maximises, the left and right halve.  The zone is keyed off the
 * pointer rather than the window bounds so a window that is already wide
 * does not snap merely by being dragged slightly left.
 */

static int snap_zone_at(int x, int y) {
    struct rect work = work_area();
    if (y <= work.y + SNAP_EDGE)
        return SNAP_MAX;
    if (x <= work.x + SNAP_EDGE)
        return SNAP_LEFT;
    if (x >= work.x + work.w - 1 - SNAP_EDGE)
        return SNAP_RIGHT;
    return SNAP_NONE;
}

static struct rect snap_target_rect(int zone) {
    struct rect work = work_area();
    switch (zone) {
    case SNAP_LEFT:
        return (struct rect){work.x, work.y, work.w / 2, work.h};
    case SNAP_RIGHT:
        return (struct rect){work.x + work.w / 2, work.y,
                             work.w - work.w / 2, work.h};
    case SNAP_MAX:
        return work;
    default:
        return work;
    }
}

/* Commit the snap for a window whose drag just ended. */
static void snap_window(int id, int zone) {
    struct rect target;
    if (zone == SNAP_NONE || id <= WIN_LAUNCHER || id >= WIN_COUNT)
        return;
    target = snap_target_rect(zone);
    /* Preserve the pre-snap bounds so the maximise button and a later drag
     * can restore them, exactly as an explicit maximise does. */
    if (!windows[id].maximized)
        windows[id].restore = windows[id].r;
    queue_damage(shadow_bounds(windows[id].r));
    windows[id].r = target;
    windows[id].maximized = (zone == SNAP_MAX);
    clamp_scroll(id);
    win_damage(id);
    if (id >= WIN_APP_BASE) {
        int slot = app_slot_for_win(id);
        if (slot >= 0 && app_sessions[slot].used) {
            app_sessions[slot].resize_dirty = 1;
            (void)sync_app_size(id, 1);
        }
    }
}

static void apply_snap(int id) {
    snap_window(id, snap_zone_at(pointer_x, pointer_y));
}

/* Translucent preview of where the window will land, drawn during the drag. */
static void draw_snap_preview(void) {
    struct ui_surface s;
    int zone;
    struct rect target;
    struct ui_rect t;

    if (drag_win < 0)
        return;
    zone = snap_zone_at(pointer_x, pointer_y);
    if (zone == SNAP_NONE)
        return;
    s = ui_target();
    target = snap_target_rect(zone);
    t = ui_of(target);
    ui_fill_round_a(&s, t, UI_RADIUS_WINDOW, UI_ACCENT_FILL, 40);
    ui_stroke_round(&s, t, UI_RADIUS_WINDOW, 2, UI_ACCENT_FILL, 190);
}

static int hit_start_menu(int x, int y);

static int hit_taskbar(int x, int y) {
    if (metro_start_visible()) return -1;
    struct tb_item items[TB_MAX_ITEMS];
    int count;

    if (taskbar_expanded) {
        int ids[MAX_GUI_APPS];
        int hidden = taskbar_hidden_windows(ids);
        if (hidden > 0) {
            struct rect panel = taskbar_overflow_panel(hidden);
            for (int i = 0; i < hidden; i++) {
                struct rect row = {panel.x + 6, panel.y + 6 + i * 40,
                                   panel.w - 12, 36};
                if (inside(x, y, row))
                    return ids[i];
            }
        }
    }

    count = taskbar_items(items);
    for (int i = 0; i < count; i++) {
        if (!inside(x, y, items[i].r))
            continue;
        switch (items[i].kind) {
        case TB_START:     return TB_HIT_START;
        case TB_WINDOW:    return items[i].value;
        case TB_OVERFLOW:  return TB_HIT_OVERFLOW;
        case TB_TRAY_IME:  return TB_HIT_IME;
        case TB_TRAY_CLOCK:return TB_HIT_CLOCK;
        default:           return -1;
        }
    }
    return -1;
}

static void send_mouse_to_app(int id, int buttons, int wheel) {
    int slot = app_slot_for_win(id);
    if (slot < 0 || !app_sessions[slot].used || window_exit[id])
        return;
    struct rect c = content_rect(id);
    int x = pointer_x - c.x;
    int y = pointer_y - c.y;
    (void)app_send_event(slot, GUIAPP_EVT_MOUSE, x, y, 0, buttons, wheel);
}

static void send_mouse_leave_to_app(int id) {
    int slot = app_slot_for_win(id);
    if (slot < 0 || !app_sessions[slot].used)
        return;
    /* A negative coordinate is outside every app control and avoids treating
     * a leave as a hover over the old window's overlapping geometry. */
    (void)app_send_event(slot, GUIAPP_EVT_MOUSE, -1, -1, 0, 0, 0);
}

static void update_hover_app(int force) {
    int next_hover_app = -1;
    /* The context menu owns the pointer while open; do not light up controls
     * in the application underneath it. */
    if (!context_open && !(start_open && inside(pointer_x, pointer_y, start_menu_rect()))) {
        int hovered_window = hit_window(pointer_x, pointer_y);
        int hovered_slot = app_slot_for_win(hovered_window);
        if (hovered_slot >= 0 && app_sessions[hovered_slot].used &&
            inside(pointer_x, pointer_y, content_rect(hovered_window)))
            next_hover_app = hovered_window;
    }
    if (hover_app >= 0 && hover_app != next_hover_app)
        send_mouse_leave_to_app(hover_app);
    if (next_hover_app >= 0 &&
        (force || next_hover_app != hover_app))
        send_mouse_to_app(next_hover_app, 0, 0);
    hover_app = next_hover_app;
}

static void flush_pending_app_resizes(void) {
    /* Modern live resize: publish configure every frame and issue RESIZE
     * when the previous present completed (resize_inflight).  Stretch
     * covers the gap; guiapp_read_event overlays latest configure size. */
    for (int slot = 0; slot < MAX_GUI_APPS; slot++) {
        if (!app_sessions[slot].used || !app_sessions[slot].resize_dirty)
            continue;
        (void)sync_app_size(WIN_APP_BASE + slot, 0);
    }
}

static void send_app_ticks(void) {
    uint32_t now = monotonic_ms();
    if ((uint32_t)(now - last_app_tick_ms) < 500u)
        return;
    last_app_tick_ms = now;
    for (int slot = 0; slot < MAX_GUI_APPS; slot++) {
        if (!app_sessions[slot].used || !app_sessions[slot].wants_tick)
            continue;
        if (app_send_event(slot, GUIAPP_EVT_TICK, 0, 0, 0, 0, 0) < 0)
            app_sessions[slot].reader_dead = 1;
    }
}

static int has_app_tick_clients(void) {
    for (int slot = 0; slot < MAX_GUI_APPS; slot++)
        if (app_sessions[slot].used && app_sessions[slot].wants_tick)
            return 1;
    return 0;
}

static int shell_motion_active(void) {
    if (home_scroll_motion[0].active || home_scroll_motion[1].active ||
        result_scroll_motion.active) return 1;
    for (int i = 0; i < WIN_COUNT + 4; i++)
        if (dock_hover_motion[i].active || dock_press_motion[i].active ||
            dock_active_motion[i].active) return 1;
    for (int id = WIN_STATUS; id < WIN_COUNT; id++)
        for (int control = 0; control < 3; control++)
            if (caption_motion[id][control].active) return 1;
    if (search_motion.active || home_motion_active) return 1;
    for (int id = WIN_STATUS; id < WIN_COUNT; id++)
        if (window_motion[id].active || window_motion_pending[id]) return 1;
    for (int i = 0; i < app_count; i++)
        if (tile_hover_motion[i].active || tile_press_motion[i].active) return 1;
    return 0;
}

static void refresh_shell_motion(uint32_t now) {
    for (int id = WIN_STATUS; id < WIN_COUNT; id++) {
        if (window_exit[id] && !window_motion[id].active) {
            int action = window_exit[id];
            window_motion_cancel(id);
            if (action == WINDOW_EXIT_CLOSE) close_window(id);
            else minimize_window(id);
            desktop_dirty = 1;
        }
        for (int control = 0; control < 3; control++) {
            int hovered = windows[id].visible && !windows[id].minimized &&
                          control_hovered(id, control);
            ui_motion_to(&caption_motion[id][control], hovered ? 255 : 0, 120, now);
        }
    }
    int dock_hover[WIN_COUNT + 4] = {0}, dock_active[WIN_COUNT + 4] = {0};
    struct tb_item items[TB_MAX_ITEMS];
    int count = metro_start_visible() ? 0 : taskbar_items(items);
    for (int i = 0; i < count; i++) {
        struct tb_item *it = &items[i];
        int channel = it->kind == TB_WINDOW ? it->value :
            it->kind == TB_START ? WIN_COUNT :
            it->kind == TB_OVERFLOW ? WIN_COUNT + 1 :
            it->kind == TB_TRAY_IME ? WIN_COUNT + 2 : WIN_COUNT + 3;
        dock_hover[channel] = inside(pointer_x, pointer_y, it->r);
        dock_active[channel] = it->kind == TB_WINDOW ? windows[it->value].active :
            it->kind == TB_START ? start_open :
            it->kind == TB_OVERFLOW ? taskbar_expanded : 0;
    }
    for (int i = 0; i < WIN_COUNT + 4; i++) {
        ui_motion_to(&dock_hover_motion[i], dock_hover[i] ? 255 : 0, 120, now);
        ui_motion_to(&dock_press_motion[i], dock_hover[i] && motion_pointer_down ? 255 : 0, 80, now);
        ui_motion_to(&dock_active_motion[i], dock_active[i] ? 255 : 0, 180, now);
    }
    for (int id = WIN_STATUS; id < WIN_COUNT; id++) {
        if (!window_motion_pending[id]) continue;
        window_motion_pending[id] = 0;
        ui_motion_to(&window_motion[id], 0, 240, now);
    }
    int home = metro_start_visible();
    if (home && !motion_home_visible) {
        home_motion_start = now;
        home_motion_active = 1;
        motion_frame_ms = now - MOTION_FRAME_MS;
    }
    if (!home && home_motion_active) {
        home_motion_active = 0;
        queue_damage(content_rect(WIN_LAUNCHER));
    }
    motion_home_visible = home;
    int hovered = home && !start_open ? hit_launcher_row_at(pointer_x, pointer_y) : -1;
    for (int i = 0; i < app_count; i++) {
        ui_motion_to(&tile_hover_motion[i], i == hovered ? 255 : 0, 120, now);
        ui_motion_to(&tile_press_motion[i], i == launcher_press && i == hovered
                     && motion_pointer_down ? 255 : 0, 90, now);
    }
    if ((uint32_t)(now - motion_frame_ms) < MOTION_FRAME_MS) return;
    motion_frame_ms = now;
    for (int axis = 0; axis < 2; axis++) {
        if (!ui_motion_step(&home_scroll_motion[axis], now)) continue;
        if (axis) scroll_y[WIN_LAUNCHER] = home_scroll_motion[axis].value;
        else scroll_x[WIN_LAUNCHER] = home_scroll_motion[axis].value;
        clamp_scroll(WIN_LAUNCHER);
        /* Include the group headings above the tile body and scrollbars. */
        win_damage(WIN_LAUNCHER);
        hover_launcher_row = -1;
        refresh_pointer_hover_damage();
    }
    if (ui_motion_step(&result_scroll_motion, now)) {
        start_scroll = result_scroll_motion.value;
        hover_start_tile = -1;
        start_damage();
    }
    int dock_changed = 0;
    for (int i = 0; i < WIN_COUNT + 4; i++) {
        dock_changed |= ui_motion_step(&dock_hover_motion[i], now);
        dock_changed |= ui_motion_step(&dock_press_motion[i], now);
        dock_changed |= ui_motion_step(&dock_active_motion[i], now);
    }
    if (dock_changed) taskbar_damage();
    for (int id = WIN_STATUS; id < WIN_COUNT; id++)
        for (int control = 0; control < 3; control++)
            if (ui_motion_step(&caption_motion[id][control], now))
                queue_damage(motion_paint_rect(id, caption_rect(id, control)));
    if (home_motion_active) {
        struct rect c = content_rect(WIN_LAUNCHER);
        queue_damage(c);
        uint32_t end = HOME_MOTION_MS + (uint32_t)min_i(max_i(0, app_count - 1), 10) * HOME_STAGGER_MS;
        if ((uint32_t)(now - home_motion_start) >= end) home_motion_active = 0;
    }
    if (ui_motion_step(&search_motion, now)) start_damage();
    for (int id = WIN_STATUS; id < WIN_COUNT; id++) {
        struct rect old = motion_paint_rect(id, windows[id].r);
        if (ui_motion_step(&window_motion[id], now))
            queue_damage(union_rect(shadow_bounds(old),
                                     shadow_bounds(motion_paint_rect(id, windows[id].r))));
        if (window_exit[id] && !window_motion[id].active) {
            int action = window_exit[id];
            window_motion_cancel(id);
            if (action == WINDOW_EXIT_CLOSE) close_window(id);
            else minimize_window(id);
            desktop_dirty = 1;
        }
    }
    for (int i = 0; i < app_count; i++) {
        int changed = ui_motion_step(&tile_hover_motion[i], now);
        changed |= ui_motion_step(&tile_press_motion[i], now);
        if (changed) {
            struct rect r = launcher_row_paint_rect(i);
            r.x -= 4; r.y -= 4; r.w += 8; r.h += 32;
            queue_damage(intersect_rect(r, content_rect(WIN_LAUNCHER)));
        }
    }
}

static void refresh_timed_shell(uint32_t now) {
    /* A notification threshold, not an execution deadline. The shell owns
     * the title and can paint it while the page's main thread is busy. */
    for (int slot = 0; slot < MAX_GUI_APPS; slot++) {
        struct app_session *app = &app_sessions[slot];
        struct guiapp_shared_surface *shared = app->shared;
        int busy = app->used && shared && shared->script_control_enabled &&
            __atomic_load_n(&shared->script_running, __ATOMIC_ACQUIRE) &&
            (uint32_t)(now - shared->script_started_ms) >= 2000;
        if (busy != app->script_busy_visible) {
            app->script_busy_visible = busy;
            win_damage(WIN_APP_BASE + slot);
        }
        if (app->used && app->input_overflow && app_event_has_capacity(slot)) {
            app->input_overflow = 0;
            app_send_event(slot, GUIAPP_EVT_INPUT_RESET, 0, 0, 0, 0, 0);
        }
    }
    /* Keep the old approximately-60-Hz time unit without requiring a 60-Hz
     * polling loop.  Only actual deadlines cause damage. */
    tick = now / 16u;
    uint32_t second = now / 1000u;
    if (second != last_clock_second) {
        last_clock_second = second;
        if (!metro_start_visible()) taskbar_clock_damage();
    }
    if (mode_error_until && tick >= mode_error_until) {
        mode_error_until = 0;
        win_damage(WIN_STATUS);
    }
}

static unsigned int gui_idle_timeout(uint32_t now) {
    unsigned int timeout = 1000u - now % 1000u;
    if (shell_motion_active()) {
        uint32_t elapsed = now - motion_frame_ms;
        unsigned int next = elapsed >= MOTION_FRAME_MS ? 1u : MOTION_FRAME_MS - elapsed;
        if (next < timeout) timeout = next;
    }
    if (has_app_tick_clients()) {
        uint32_t elapsed = now - last_app_tick_ms;
        unsigned int app_timeout = elapsed >= 500u ? 1u : 500u - elapsed;
        if (app_timeout < timeout)
            timeout = app_timeout;
    }
    if (mode_error_until && mode_error_until > tick) {
        unsigned int error_timeout = (mode_error_until - tick) * 16u;
        if (error_timeout < timeout)
            timeout = error_timeout;
    }
    return timeout ? timeout : 1u;
}

static int ime_target_active(void) {
    int slot = app_slot_for_win(focus);
    return slot >= 0 && app_sessions[slot].used;
}

static void ime_submit(const char *value) {
    if (!value || !value[0]) return;
    int slot = app_slot_for_win(focus);
    while (slot >= 0 && app_sessions[slot].used && *value) {
        int n = (int)strlen(value);
        if (n > GUIAPP_TEXT_MAX - 1) n = GUIAPP_TEXT_MAX - 1;
        while (n > 0 && ((uint8_t)value[n] & 0xC0u) == 0x80u) n--;
        if (n <= 0) n = GUIAPP_TEXT_MAX - 1;
        char chunk[GUIAPP_TEXT_MAX];
        for (int i = 0; i < n; i++) chunk[i] = value[i];
        chunk[n] = 0;
        if (app_send_text(slot, chunk) < 0)
            break;
        value += n;
    }
}

static void clipboard_command(int command) {
    if (context_target < 0) return;
    activate(context_target);
    if (command == GUIAPP_CMD_PASTE) {
        if (clipboard[0]) ime_submit(clipboard);
        return;
    }
    int slot = app_slot_for_win(context_target);
    if (slot >= 0 && app_sessions[slot].used &&
        app_send_event(slot, GUIAPP_EVT_COMMAND, 0, 0, command, 0, 0) < 0)
        app_sessions[slot].reader_dead = 1;
}

static void ime_clear(void) {
    ime_length = 0;
    ime_buffer[0] = 0;
    ime_page = 0;
    ime_cand_count = 0;
}

static void ime_consume_prefix(int n) {
    if (n <= 0)
        return;
    if (n >= ime_length) {
        ime_clear();
        return;
    }
    for (int i = 0; i <= ime_length - n; i++)
        ime_buffer[i] = ime_buffer[i + n];
    ime_length -= n;
    ime_rebuild_candidates();
}

static void ime_commit_candidate(int page_index) {
    char item[GUIAPP_TEXT_MAX];
    if (ime_candidate_at(page_index, item)) {
        int consume = ime_candidate_consume(page_index);
        ime_submit(item);
        ime_consume_prefix(consume);
    } else if (ime_length > 0) {
        /* No dictionary hit: pass through the raw pinyin so the user is
         * never stuck with a dead composition. */
        ime_submit(ime_buffer);
        ime_clear();
    }
}

static const char *ime_punctuation(int k) {
    /* While composing, -/[=] are claimed earlier for candidate paging. */
    switch (k) {
    case ',': return "，"; case '.': return "。"; case '?': return "？";
    case '!': return "！"; case ':': return "："; case ';': return "；";
    case '(': return "（"; case ')': return "）";
    case '<': return "《"; case '>': return "》";
    case '[': return "【"; case ']': return "】";
    case '\'': return "‘";
    case '"': return "“";
    case '\\': return "、";
    case '`': return "·";
    case '~': return "～";
    case '$': return "￥";
    case '^': return "……";
    case '_': return "——";
    case '{': return "「"; case '}': return "」";
    case '|': return "｜";
    default: return 0;
    }
}

/* Returns non-zero when the desktop IME consumed the key. */
static int ime_handle_key(int k) {
    if (k == 0x1F) { /* Ctrl+Space from the keyboard driver */
        ime_enabled = !ime_enabled;
        ime_clear();
        ime_damage();
        return 1;
    }
    if (!ime_enabled || !ime_target_active())
        return 0;
    if (k == KEY_ESC && ime_length > 0) {
        ime_clear();
        ime_damage();
        return 1;
    }
    if ((k == KEY_BACKSPACE || k == 127) && ime_length > 0) {
        ime_buffer[--ime_length] = 0;
        ime_buffer[ime_length] = 0;
        ime_rebuild_candidates();
        ime_damage();
        return 1;
    }
    if ((k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z')) {
        if (ime_length + 1 < IME_BUF_CAP) {
            if (k >= 'A' && k <= 'Z')
                k += 'a' - 'A';
            /* ü is typed as v (standard mainland IME convention). */
            ime_buffer[ime_length++] = (char)k;
            ime_buffer[ime_length] = 0;
            ime_rebuild_candidates();
        }
        ime_damage();
        return 1;
    }
    if (ime_length > 0) {
        /* Page candidates: -/[ previous, =/] next (also +/- on some layouts). */
        if (k == '-' || k == '[' || k == KEY_LEFT) {
            if (ime_page > 0)
                ime_page--;
            ime_clamp_page();
            ime_damage();
            return 1;
        }
        if (k == '=' || k == ']' || k == KEY_RIGHT) {
            if (ime_page + 1 < ime_page_count())
                ime_page++;
            ime_clamp_page();
            ime_damage();
            return 1;
        }
        if (k >= '1' && k <= '9') {
            ime_commit_candidate(k - '1');
            ime_damage();
            return 1;
        }
        if (k == ' ' || k == '\r' || k == '\n') {
            ime_commit_candidate(0);
            ime_damage();
            return 1;
        }
        /* Commit the best candidate (or raw pinyin) before punctuation. */
        ime_commit_candidate(0);
    }
    const char *punct = ime_punctuation(k);
    if (punct) {
        ime_submit(punct);
        ime_damage();
        return 1;
    }
    return 0;
}

static void activate_next_visible(void) {
    for (int step = 1; step <= WIN_COUNT; step++) {
        int id = (focus + step) % WIN_COUNT;
        if (id == WIN_LAUNCHER)
            continue;
        if (!windows[id].visible || windows[id].minimized)
            continue;
        if (id >= WIN_APP_BASE && !app_sessions[id - WIN_APP_BASE].used)
            continue;
        activate(id);
        return;
    }
}

static void show_workspace(void) {
    for (int id = WIN_STATUS; id < WIN_COUNT; id++)
        if (windows[id].visible) windows[id].minimized = 1;
    start_set_open(0);
    activate(WIN_LAUNCHER);
    desktop_dirty = 1;
}

static void switch_task(void) {
    for (int step = 1; step <= WIN_COUNT; step++) {
        int id = (focus + step) % WIN_COUNT;
        if (id == WIN_LAUNCHER || !windows[id].visible) continue;
        if (id >= WIN_APP_BASE && !app_sessions[id - WIN_APP_BASE].used)
            continue;
        activate(id);
        desktop_dirty = 1;
        return;
    }
}

static void handle_key(int k) {
    if (k == 299) return;
    finish_window_exits();
    ui_motion_reset(&home_scroll_motion[0], scroll_x[WIN_LAUNCHER]);
    ui_motion_reset(&home_scroll_motion[1], scroll_y[WIN_LAUNCHER]);
    ui_motion_reset(&result_scroll_motion, start_scroll);
    if (k >= KEY_WINDOW_CLOSE && k <= KEY_SNAP_LEFT) {
        start_set_open(0);
        if (k == KEY_WORKSPACE) show_workspace();
        else if (k == KEY_TASK_SWITCH) switch_task();
        else if (focus != WIN_LAUNCHER) {
            if (k == KEY_WINDOW_CLOSE) request_window_exit(focus, WINDOW_EXIT_CLOSE);
            if (k == KEY_MINIMIZE) request_window_exit(focus, WINDOW_EXIT_MINIMIZE);
            if (k == KEY_MAXIMIZE) toggle_maximize(focus);
            if (k == KEY_SNAP_LEFT) snap_window(focus, SNAP_LEFT);
            if (k == KEY_SNAP_RIGHT) snap_window(focus, SNAP_RIGHT);
            desktop_dirty = 1;
        }
        return;
    }
    if (k == 16) { /* Ctrl+P: application palette */
        start_set_open(!start_open);
        return;
    }
    if (start_open) {
        start_handle_key(k);
        return;
    }
    if (ime_handle_key(k))
        return;
    if (k == 23 && focus != WIN_LAUNCHER) { /* Ctrl+W */
        request_window_exit(focus, WINDOW_EXIT_CLOSE);
        desktop_dirty = 1;
        return;
    }
    if (k == KEY_ESC) {
        if (app_handle_escape(focus)) return;
        running = 0;
        return;
    }
    if (k == '\t' && focus < WIN_APP_BASE) {
        activate_next_visible();
        desktop_dirty = 1;
        return;
    }
    if (focus == WIN_STATUS) {
        /* Digits pick the first nine listed modes; [ ] cycle all ratios. */
        if (k >= '1' && k <= '9' && (k - '1') < DISPLAY_MODE_COUNT) {
            (void)switch_display_mode(k - '1');
            return;
        }
        if (k == '[' || k == KEY_LEFT) {
            int cur = current_display_mode();
            int next = cur < 0 ? 0 : (cur + DISPLAY_MODE_COUNT - 1) %
                                     DISPLAY_MODE_COUNT;
            (void)switch_display_mode(next);
            return;
        }
        if (k == ']' || k == KEY_RIGHT) {
            int cur = current_display_mode();
            int next = cur < 0 ? 0 : (cur + 1) % DISPLAY_MODE_COUNT;
            (void)switch_display_mode(next);
            return;
        }
    }
    if (focus == WIN_LAUNCHER) {
        if (k == KEY_UP || k == KEY_DOWN || k == KEY_LEFT || k == KEY_RIGHT ||
            k == KEY_HOME || k == KEY_END) {
            home_motion_active = 0;
            motion_home_visible = 1;
            queue_damage(content_rect(WIN_LAUNCHER));
        }
        if (k == 's' || k == 'S') {
            activate(WIN_STATUS);
            desktop_dirty = 1;
            return;
        }
        int old_selected = app_selected;
        struct rect body = content_rect(WIN_LAUNCHER);
        if (k == KEY_UP || k == KEY_DOWN || k == KEY_LEFT || k == KEY_RIGHT)
            app_selected = launcher_neighbor(k);
        else if (k == KEY_HOME) app_selected = 0;
        else if (k == KEY_END) app_selected = max_i(0, app_count - 1);
        else if ((k == '\n' || k == '\r') && app_count > 0) {
            run_app(apps[app_selected].path);
            return;
        } else if (k == 'r' || k == 'R') {
            scan_apps();
            desktop_dirty = 1;
            return;
        } else if (k >= 32 && k < 127) {
            start_set_open(1);
            start_handle_key(k);
            return;
        }
        if (app_selected != old_selected) {
            struct rect tile = launcher_row_paint_rect(app_selected);
            if (tile.x < body.x) scroll_x[WIN_LAUNCHER] -= body.x - tile.x;
            else if (tile.x + tile.w > body.x + body.w)
                scroll_x[WIN_LAUNCHER] += tile.x + tile.w - body.x - body.w;
            if (tile.y < body.y) scroll_y[WIN_LAUNCHER] -= body.y - tile.y;
            else if (tile.y + tile.h > body.y + body.h)
                scroll_y[WIN_LAUNCHER] += tile.y + tile.h - body.y - body.h;
            clamp_scroll(WIN_LAUNCHER);
            win_damage(WIN_LAUNCHER);
        }
        return;
    }
    int slot = app_slot_for_win(focus);
    if (slot >= 0 && app_sessions[slot].used) {
        if (app_send_event(slot, GUIAPP_EVT_KEY, 0, 0, k, 1, 0) < 0)
            app_sessions[slot].reader_dead = 1;
        else if (k >= 0 && k < GUIAPP_KEY_COUNT)
            key_owner[k] = (uint8_t)(slot + 1);
    }
}

static void forward_key_releases(void) {
    if (keyevent_fd < 0)
        return;
    uint16_t event;
    while (read(keyevent_fd, &event, sizeof(event)) == (int)sizeof(event)) {
        if (event & 0x8000u)
            continue;
        int key = event & 0x7FFFu;
        int slot = key < GUIAPP_KEY_COUNT ? (int)key_owner[key] - 1 : -1;
        if (key < GUIAPP_KEY_COUNT) key_owner[key] = 0;
        if (slot < 0) continue;
        if (slot >= 0 && app_sessions[slot].used &&
            app_send_event(slot, GUIAPP_EVT_KEY, 0, 0,
                           key, 0, 0) < 0)
            app_sessions[slot].reader_dead = 1;
    }
}

static void damage_widget(struct rect r) {
    if (r.w <= 0 || r.h <= 0)
        return;
    /* 1px pad covers rounded-corner AA that extends past the hit rect. */
    queue_damage((struct rect){r.x - 1, r.y - 1, r.w + 2, r.h + 2});
}

static struct rect pointer_damage_rect(int x, int y) {
    return (struct rect){
        x - POINTER_DAMAGE_PAD,
        y - POINTER_DAMAGE_PAD,
        POINTER_W + 2 * POINTER_DAMAGE_PAD,
        POINTER_H + 2 * POINTER_DAMAGE_PAD
    };
}

/* Expand a dirty region so a partial compose both erases the last scanned-out
 * cursor and redraws it at the live position. */
static struct rect damage_with_pointer(struct rect damage) {
    if (hardware_cursor_ready)
        return damage;
    if (pointer_drawn_valid)
        damage = union_rect(damage, pointer_damage_rect(pointer_drawn_x,
                                                        pointer_drawn_y));
    damage = union_rect(damage, pointer_damage_rect(pointer_x, pointer_y));
    return damage;
}

static void note_pointer_drawn(void) {
    if (hardware_cursor_ready) {
        pointer_drawn_valid = 0;
        return;
    }
    pointer_drawn_x = pointer_x;
    pointer_drawn_y = pointer_y;
    pointer_drawn_valid = 1;
}

static int hit_status_mode_at(int x, int y) {
    if (!windows[WIN_STATUS].visible || windows[WIN_STATUS].minimized)
        return -1;
    if (top_window_at(x, y) != WIN_STATUS)
        return -1;
    struct rect body = content_rect(WIN_STATUS);
    if (!inside(x, y, body))
        return -1;
    for (int mode = 0; mode < DISPLAY_MODE_COUNT; mode++) {
        if (inside(x, y, status_mode_rect(mode)))
            return mode;
    }
    return -1;
}

/* Geometry shared by paint, hit-test, and damage.  Width is capped to the
 * visible content so dirty rects match what fill_round actually painted. */
static struct rect launcher_row_paint_rect(int index) {
    if (sw < 1000) {
        struct rect c = content_rect(WIN_LAUNCHER);
        int cols = compact_columns(c), w = compact_tile_width(c), h = compact_tile_height(c);
        return (struct rect){compact_grid_x(c) + (index % cols) * (w + 12),
                             c.y - scroll_y[WIN_LAUNCHER] + (index / cols) * (h + 12), w, h};
    }
    return metro_tile_rect(metro_tile_for(apps[index].name, index));
}

static struct rect launcher_row_screen_rect(int index) {
    return intersect_rect(launcher_row_paint_rect(index),
                          content_rect(WIN_LAUNCHER));
}

static int hit_launcher_row_at(int x, int y) {
    if (!windows[WIN_LAUNCHER].visible || windows[WIN_LAUNCHER].minimized)
        return -1;
    if (top_window_at(x, y) != WIN_LAUNCHER)
        return -1;
    struct rect c = content_rect(WIN_LAUNCHER);
    if (!inside(x, y, c))
        return -1;
    for (int index = 0; index < app_count; index++)
        if (inside(x, y, launcher_row_paint_rect(index)))
            return index;
    return -1;
}

/* Recompute pointer-driven hover and damage full widget bounds on change. */
static void refresh_pointer_hover_damage(void) {
    static int previous_action = -1;
    int action = -1;
    if (metro_start_visible() && !start_open)
        for (int i = 0; i < 5; i++)
            if (inside(pointer_x, pointer_y, metro_action_rect(i))) action = i;
    if (action != previous_action) {
        if (previous_action >= 0) damage_widget(metro_action_rect(previous_action));
        if (action >= 0) damage_widget(metro_action_rect(action));
        previous_action = action;
    }
    int mode = hit_status_mode_at(pointer_x, pointer_y);
    if (mode != hover_status_mode) {
        if (hover_status_mode >= 0)
            damage_widget(status_mode_rect(hover_status_mode));
        if (mode >= 0)
            damage_widget(status_mode_rect(mode));
        hover_status_mode = mode;
    }

    int chrome_ctl = -1;
    int chrome_win = hit_control(pointer_x, pointer_y, &chrome_ctl);
    if (chrome_win != hover_chrome_win || chrome_ctl != hover_chrome_ctl) {
        if (hover_chrome_win >= 0 && hover_chrome_ctl >= 0)
            damage_widget(control_hit_rect(hover_chrome_win, hover_chrome_ctl));
        if (chrome_win >= 0 && chrome_ctl >= 0)
            damage_widget(control_hit_rect(chrome_win, chrome_ctl));
        hover_chrome_win = chrome_win;
        hover_chrome_ctl = chrome_ctl;
    }

    /* Overlays own the pointer while open.  Without this the launcher below
     * the Start menu keeps taking hover -- and its rows light up through the
     * menu -- because a flyout is not a window and so is invisible to
     * top_window_at(). */
    int overlay = start_open &&
                  inside(pointer_x, pointer_y, start_menu_rect());

    /* Tracked even while the menu is shut, so closing and reopening it under
     * a stationary pointer still repaints the tile: leaving a stale index
     * here would make the reopened highlight compare equal and never damage. */
    int tile = overlay ? hit_start_menu(pointer_x, pointer_y) : -1;
    if (tile != hover_start_tile) {
        if (hover_start_tile >= 0)
            damage_widget(start_app_tile_rect(hover_start_tile));
        if (tile >= 0)
            damage_widget(start_app_tile_rect(tile));
        hover_start_tile = tile;
    }

    int row = overlay ? -1 : hit_launcher_row_at(pointer_x, pointer_y);
    if (row != hover_launcher_row) {
        if (hover_launcher_row >= 0)
            damage_widget(launcher_row_screen_rect(hover_launcher_row));
        if (row >= 0)
            damage_widget(launcher_row_screen_rect(row));
        hover_launcher_row = row;
    }
}

static void handle_mouse(void) {
    struct mouse_state ms;
    if (mouse_get(&ms) < 0)
        return;
    motion_pointer_down = ms.buttons & 1;
    if (ms.buttons != prev_buttons && (ms.buttons & 1)) {
        finish_window_exits();
        ui_motion_reset(&home_scroll_motion[0], scroll_x[WIN_LAUNCHER]);
        ui_motion_reset(&home_scroll_motion[1], scroll_y[WIN_LAUNCHER]);
        ui_motion_reset(&result_scroll_motion, start_scroll);
        /* Input immediately owns final window geometry; arrival motion must
         * not move a title/button away from a click, resize or drag. */
        for (int id = WIN_STATUS; id < WIN_COUNT; id++)
            if (window_motion[id].active || window_motion_pending[id]) window_motion_cancel(id);
    }
    int old_pointer_x = pointer_x;
    int old_pointer_y = pointer_y;
    int old_dock_hover = taskbar_hover;
    int pointer_moved = ms.x != pointer_x || ms.y != pointer_y;
    if (ms.buttons != prev_buttons)
        desktop_dirty = 1;
    pointer_x = ms.x;
    pointer_y = ms.y;
    if (pointer_moved) {
        if (hardware_cursor_ready) {
            if (gfx_cursor_move(pointer_x, pointer_y, 1) < 0) {
                hardware_cursor_ready = 0;
                queue_damage(pointer_damage_rect(old_pointer_x,
                                                 old_pointer_y));
                queue_damage(pointer_damage_rect(pointer_x, pointer_y));
            }
        } else {
            queue_damage(pointer_damage_rect(old_pointer_x, old_pointer_y));
            queue_damage(pointer_damage_rect(pointer_x, pointer_y));
        }
        refresh_pointer_hover_damage();
        if (context_open)
            queue_damage((struct rect){context_x, context_y,
                                       CONTEXT_MENU_W, CONTEXT_MENU_H});
        if (!(ms.buttons & 1) && app_mouse_capture < 0) {
            update_hover_app(1);
        }
    }
    int left = ms.buttons & 1;
    int right = ms.buttons & 2;
    taskbar_hover = hit_taskbar(pointer_x, pointer_y);
    if (taskbar_hover != old_dock_hover)
        taskbar_damage();

    if (right && !(prev_buttons & 2)) {
        int target = hit_window(pointer_x, pointer_y);
        if (target >= WIN_APP_BASE &&
            inside(pointer_x, pointer_y, content_rect(target))) {
            context_open = 1;
            context_x = pointer_x;
            context_y = pointer_y;
            context_target = target;
            activate(target);
        } else {
            context_open = 0;
        }
        prev_buttons = ms.buttons;
        return;
    }

    if (left && !(prev_buttons & 1) && context_open) {
        int item = (pointer_y - context_y - 4) / CONTEXT_ITEM_STEP;
        if (pointer_x >= context_x + 4 &&
            pointer_x < context_x + CONTEXT_MENU_W - 4 &&
            pointer_y >= context_y + 4 && item >= 0 && item < 3) {
            static const int commands[] = {GUIAPP_CMD_COPY, GUIAPP_CMD_PASTE, GUIAPP_CMD_CUT};
            if (item != 1 || clipboard[0])
                clipboard_command(commands[item]);
        }
        context_open = 0;
        prev_buttons = ms.buttons;
        return;
    }

    if (ms.wheel_seq != last_wheel_seq) {
        int wheel_delta = ms.wheel - last_wheel_value;
        /* An open flyout absorbs the wheel; scrolling the window underneath
         * it would move content the pointer is not actually over. */
        int over_search = start_open && inside(pointer_x, pointer_y, start_menu_rect());
        /* A captured thumb owns scrolling until release. Consume wheel
         * reports without starting motion that can overwrite the drag. */
        int h = scroll_drag_win >= 0 || over_search ? -1 : hit_window(pointer_x, pointer_y);
        if (scroll_drag_win < 0 && over_search) {
            int indices[MAX_APPS];
            int count = start_matches(indices);
            if (!result_scroll_motion.active)
                ui_motion_reset(&result_scroll_motion, start_scroll);
            ui_motion_scroll(&result_scroll_motion, -wheel_delta * START_ROW_H,
                max_i(0, count * START_ROW_H - start_results_rect().h), 160, monotonic_ms());
            /* Retarget may sample directly onto the target and become idle;
             * publish that sample even when no later step will report change. */
            start_scroll = result_scroll_motion.value;
            hover_start_tile = -1;
            start_damage();
        }
        if (h >= 0) {
            int slot = app_slot_for_win(h);
            if (slot >= 0 && app_sessions[slot].used && inside(pointer_x, pointer_y, content_rect(h)))
                send_mouse_to_app(h, ms.buttons, wheel_delta);
            else {
                if (h == WIN_LAUNCHER) {
                    int axis = sw < 1000;
                    struct ui_motion *m = &home_scroll_motion[axis];
                    if (!m->active) ui_motion_reset(m, axis ? scroll_y[h] : scroll_x[h]);
                    ui_motion_scroll(m, -wheel_delta * (axis ? 44 : 80),
                                     axis ? max_scroll_y(h) : max_scroll_x(h), 180, monotonic_ms());
                    if (axis) scroll_y[h] = m->value;
                    else scroll_x[h] = m->value;
                    home_motion_active = 0;
                } else {
                    scroll_y[h] -= wheel_delta * 44;
                    clamp_scroll(h);
                }
                /* Scroll changes widget geometry under a stationary pointer. */
                hover_status_mode = -1;
                hover_launcher_row = -1;
                refresh_pointer_hover_damage();
            }
            win_damage(h);
        }
        last_wheel_seq = ms.wheel_seq;
        last_wheel_value = ms.wheel;
    }

    if (left && !prev_buttons) {
        if (metro_start_visible() && !start_open) {
            for (int action = 0; action < 5; action++) {
                struct rect hit = metro_action_rect(action);
                if (action < 2) hit = intersect_rect(hit, content_rect(WIN_LAUNCHER));
                if (!inside(pointer_x, pointer_y, hit)) continue;
                if (action == 0) { activate(WIN_STATUS); desktop_dirty = 1; }
                else if (action == 4) running = 0;
                else start_set_open(1);
                prev_buttons = ms.buttons;
                return;
            }
        }
        int start_pick = hit_start_menu(pointer_x, pointer_y);
        if (start_open && start_pick >= 0) {
            /* Launching closes the menu, as on any modern desktop. */
            start_set_open(0);
            run_app(apps[start_pick].path);
            prev_buttons = ms.buttons;
            return;
        }
        if (start_open && start_pick == -2) {
            start_set_open(0);
            prev_buttons = ms.buttons;
            return;
        }
        if (start_open && inside(pointer_x, pointer_y, start_menu_rect())) {
            /* A click inside the menu chrome is absorbed, not passed through
             * to whatever window happens to be underneath. */
            prev_buttons = ms.buttons;
            return;
        }
        if (taskbar_hover == TB_HIT_START) {
            start_set_open(!start_open);
            taskbar_expanded = 0;
        } else if (taskbar_hover == TB_HIT_IME) {
            ime_enabled = !ime_enabled;
            ime_clear();
            ime_damage();
        } else if (taskbar_hover == TB_HIT_CLOCK) {
            /* No flyout yet; swallow the click so it does not fall through
             * to the desktop and deactivate the focused window. */
        } else if (taskbar_hover == TB_HIT_OVERFLOW) {
            taskbar_expanded = !taskbar_expanded;
            desktop_dirty = 1;
        } else if (taskbar_hover >= 0) {
            if (taskbar_hover == WIN_LAUNCHER) show_workspace();
            else activate(taskbar_hover);
            taskbar_expanded = 0;
            start_set_open(0);
        } else {
            start_set_open(0);
            int control = -1;
            int ctl_win = hit_control(pointer_x, pointer_y, &control);
            if (ctl_win >= 0) {
                activate(ctl_win);
                if (control == 0)
                    request_window_exit(ctl_win, WINDOW_EXIT_MINIMIZE);
                else if (control == 1)
                    toggle_maximize(ctl_win);
                else
                    request_window_exit(ctl_win, WINDOW_EXIT_CLOSE);
                prev_buttons = ms.buttons;
                return;
            }

            int axis = -1;
            int sb = hit_scrollbar(pointer_x, pointer_y, &axis);
            if (sb >= 0) {
                activate(sb);
                if (axis) {
                    struct rect track = vscroll_track(sb);
                    struct rect thumb = vscroll_thumb(sb);
                    if (!inside(pointer_x, pointer_y, thumb)) {
                        int span = max_i(1, track.h - thumb.h);
                        scroll_y[sb] = (pointer_y - track.y - thumb.h / 2) *
                                       max_scroll_y(sb) / span;
                        clamp_scroll(sb);
                    }
                } else {
                    struct rect track = hscroll_track(sb);
                    struct rect thumb = hscroll_thumb(sb);
                    if (!inside(pointer_x, pointer_y, thumb)) {
                        int span = max_i(1, track.w - thumb.w);
                        scroll_x[sb] = (pointer_x - track.x - thumb.w / 2) *
                                       max_scroll_x(sb) / span;
                        clamp_scroll(sb);
                    }
                }
                scroll_drag_win = sb;
                scroll_drag_axis = axis;
                scroll_drag_mouse = axis ? pointer_y : pointer_x;
                scroll_drag_value = axis ? scroll_y[sb] : scroll_x[sb];
                prev_buttons = ms.buttons;
                return;
            }

            int edges = 0;
            int rz = hit_resize(pointer_x, pointer_y, &edges);
            if (rz >= 0) {
                activate(rz);
                resize_win = rz;
                resize_edges = edges;
                resize_start_x = pointer_x;
                resize_start_y = pointer_y;
                resize_start_rect = windows[rz].r;
                prev_buttons = ms.buttons;
                return;
            }

            int h = hit_window(pointer_x, pointer_y);
            if (h >= 0)
                activate(h);
            if (h == WIN_STATUS) {
                /* Same boundary as paint: only the visible content body is
                 * interactive.  Scrolled-away cells stay addressable only
                 * after the user scrolls them back into content_rect. */
                struct rect body = content_rect(WIN_STATUS);
                if (inside(pointer_x, pointer_y, body)) {
                    for (int mode = 0; mode < DISPLAY_MODE_COUNT; mode++) {
                        if (inside(pointer_x, pointer_y,
                                   status_mode_rect(mode))) {
                            (void)switch_display_mode(mode);
                            prev_buttons = ms.buttons;
                            return;
                        }
                    }
                }
            }
            int t = hit_window_title(pointer_x, pointer_y);
            if (t >= 0) {
                drag_win = t;
                drag_dx = pointer_x - windows[t].r.x;
                drag_dy = pointer_y - windows[t].r.y;
            }
            /* Route launcher clicks through the same hit test the hover
             * highlight uses.  This block previously derived the row from
             * pointer_y alone, gated only on focus == WIN_LAUNCHER, so a
             * click anywhere on screen -- over another window, or on bare
             * desktop below the launcher -- selected and could double-click
             * launch whichever row that y happened to line up with.
             * hit_launcher_row_at() checks the top window, the content rect
             * and the row band, so paint and hit agree. */
            int launcher_row = hit_launcher_row_at(pointer_x, pointer_y);
            if (launcher_row >= 0) {
                app_selected = launcher_row;
                launcher_press = launcher_row;
                prev_buttons = ms.buttons;
                return;
            } else if (focus != WIN_LAUNCHER) {
                int slot = app_slot_for_win(focus);
                if (slot >= 0 && inside(pointer_x, pointer_y, content_rect(focus))) {
                    app_mouse_capture = focus;
                    send_mouse_to_app(focus, ms.buttons, 0);
                }
            }
        }
    }
    if (!left) {
        if (launcher_press >= 0) {
            int pressed = launcher_press;
            launcher_press = -1;
            if (metro_start_visible() && !start_open &&
                hit_launcher_row_at(pointer_x, pointer_y) == pressed) {
                run_app(apps[pressed].path);
                prev_buttons = ms.buttons;
                return;
            }
        }
        if (app_mouse_capture >= 0)
            send_mouse_to_app(app_mouse_capture, ms.buttons, 0);
        int finished_resize = resize_win;
        int finished_drag = drag_win;
        app_mouse_capture = -1;
        drag_win = -1;
        scroll_drag_win = -1;
        resize_win = -1;
        if (finished_drag >= 0)
            apply_snap(finished_drag);
        if (finished_resize >= WIN_APP_BASE)
            (void)sync_app_size(finished_resize, 1);
        update_hover_app(0);
    }
    if (left && resize_win >= 0) {
        struct rect old = windows[resize_win].r;
        apply_resize(resize_win, pointer_x, pointer_y);
        struct rect now = windows[resize_win].r;
        queue_damage(union_rect(shadow_bounds(old), shadow_bounds(now)));
        prev_buttons = ms.buttons;
        return;
    }
    if (left && scroll_drag_win >= 0) {
        int id = scroll_drag_win;
        if (scroll_drag_axis) {
            struct rect track = vscroll_track(id);
            struct rect thumb = vscroll_thumb(id);
            int span = max_i(1, track.h - thumb.h);
            int delta = pointer_y - scroll_drag_mouse;
            scroll_y[id] = scroll_drag_value + delta * max_scroll_y(id) / span;
        } else {
            struct rect track = hscroll_track(id);
            struct rect thumb = hscroll_thumb(id);
            int span = max_i(1, track.w - thumb.w);
            int delta = pointer_x - scroll_drag_mouse;
            scroll_x[id] = scroll_drag_value + delta * max_scroll_x(id) / span;
        }
        clamp_scroll(id);
        if (id == WIN_LAUNCHER) {
            ui_motion_reset(&home_scroll_motion[0], scroll_x[id]);
            ui_motion_reset(&home_scroll_motion[1], scroll_y[id]);
        }
        /* Compact Start content extends 16px below the app work area. */
        if (id == WIN_LAUNCHER) queue_damage((struct rect){0, 0, sw, sh});
        else win_damage(id);
        prev_buttons = ms.buttons;
        return;
    }
    if (left && drag_win >= 0) {
        struct rect *r = &windows[drag_win].r;
        struct rect old = *r;
        if (windows[drag_win].maximized) {
            struct rect restored = windows[drag_win].restore;
            drag_dx = old.w > 0 ? drag_dx * restored.w / old.w : 0;
            drag_dy = clamp_i(drag_dy, 0, WINDOW_TITLE_H - 1);
            *r = restored;
            windows[drag_win].maximized = 0;
            int slot = app_slot_for_win(drag_win);
            if (slot >= 0 && app_sessions[slot].used)
                app_sessions[slot].resize_dirty = 1;
        }
        r->x = pointer_x - drag_dx;
        r->y = pointer_y - drag_dy;
        if (r->x < 0) r->x = 0;
        if (r->y < work_area().y) r->y = work_area().y;
        if (r->x + r->w > sw) r->x = sw - r->w;
        if (r->y + r->h > sh - TASKBAR_H) r->y = sh - TASKBAR_H - r->h;
        queue_damage(union_rect(shadow_bounds(old), shadow_bounds(*r)));
        /* The snap preview is drawn outside the window, so it needs its own
         * damage or it leaves an outline behind when the zone changes. */
        {
            static int last_snap_zone;
            int zone = snap_zone_at(pointer_x, pointer_y);
            if (zone != last_snap_zone) {
                if (last_snap_zone != SNAP_NONE)
                    queue_damage(snap_target_rect(last_snap_zone));
                if (zone != SNAP_NONE)
                    queue_damage(snap_target_rect(zone));
                last_snap_zone = zone;
            }
        }
        prev_buttons = ms.buttons;
        return;
    }
    if (left && app_mouse_capture >= 0) {
        send_mouse_to_app(app_mouse_capture, ms.buttons, 0);
        prev_buttons = ms.buttons;
        return;
    }
    prev_buttons = ms.buttons;
}

static void init_desktop(void) {
    struct gfx_info info;
    if (gfx_info(&info) < 0 || info.width == 0 || info.height == 0) {
        sw = 1024;
        sh = 768;
        display_backend = GFX_BACKEND_FRAMEBUFFER;
    } else {
        sw = (int)info.width;
        sh = (int)info.height;
        display_backend = info.backend;
    }
    if (sw > MAX_SW)
        sw = MAX_SW;
    if (sh > MAX_SH)
        sh = MAX_SH;
    compose_clip = (struct rect){0, 0, sw, sh};
    if (bind_scanout() == 0)
        gui_log(scanout_direct
                    ? "[gui] zero-copy virtio-gpu scanout"
                    : "[gui] buffered linear framebuffer");
    else
        gui_log("[gui] software backbuffer (scanout map failed)");
    gfx_set_origin(0, 0);
    pointer_x = sw / 2;
    pointer_y = sh / 2;
    init_hardware_cursor();
    /* Try the GPU present path after the scanout is bound: it retargets the
     * compose buffer, and needs the software path as a working fallback. */
    gpu_present_init();
    status_refresh_caps();
    scan_apps();
    keyevent_fd = open("/dev/keyevent", O_RDONLY);
    layout();
    uint32_t data[4];
    int fd = open("/fs/desktop.settings", O_RDONLY);
    if (fd >= 0) {
        int valid = read_full(fd, data, sizeof(data)) == 0;
        char extra;
        int trailing = read(fd, &extra, 1);
        close(fd);
        if (valid && trailing == 0 &&
            data[0] == 0x425A5531u) {
            for (int i = 0; i < DISPLAY_MODE_COUNT; i++) {
                if (data[1] == (uint32_t)display_modes[i].width &&
                    data[2] == (uint32_t)display_modes[i].height) {
                    if (data[3] < (uint32_t)app_count)
                        app_selected = (int)data[3];
                    if (switch_display_mode(i) == 0) {
                        preferences_saved = 1;
                        gui_log("[gui] restored desktop settings");
                    }
                    break;
                }
            }
        }
    }
    motion_home_visible = 0;
}

static void shutdown_desktop(void) {
    save_preferences();
    /* Point the compose buffer away from the GPU texture before freeing it;
     * anything that paints during teardown would otherwise write into a
     * destroyed resource. */
    if (hardware_cursor_ready)
        (void)gfx_cursor_move(pointer_x, pointer_y, 0);
    hardware_cursor_ready = 0;
    gpu_present_shutdown();
    (void)bind_scanout();
    if (keyevent_fd >= 0) {
        close(keyevent_fd);
        keyevent_fd = -1;
    }
    for (int slot = 0; slot < MAX_GUI_APPS; slot++) {
        if (app_sessions[slot].used)
            close_window(WIN_APP_BASE + slot);
    }
}

static void release_display(void) {
    if (!display_acquired)
        return;
    display_acquired = 0;
    (void)gfx_release_display();
}

static void redirect_logs_to_serial(void) {
    int serial = open("/dev/serial", O_WRONLY);
    if (serial < 0)
        return;
    (void)dup2(serial, 1);
    (void)dup2(serial, 2);
    if (serial > 2)
        close(serial);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (gfx_acquire_display() < 0) {
        puts("gui: display is already in use");
        return 1;
    }
    display_acquired = 1;
    (void)atexit(release_display);
    redirect_logs_to_serial();
    init_desktop();
    while (running) {
        /* Capture before consuming any source.  An IRQ or app-reader publish
         * during this iteration changes the sequence and makes the wait at
         * the bottom return immediately instead of losing the wakeup. */
        uint32_t event_sequence = gui_event_sequence();
        uint32_t loop_now = monotonic_ms();
        refresh_timed_shell(loop_now);
        reap_dead_apps();
        int key;
        while ((key = read_key_poll()) >= 0)
            handle_key(key);
        forward_key_releases();
        handle_mouse();
        refresh_shell_motion(monotonic_ms());
        flush_pending_app_resizes();
        send_app_ticks();
        uint32_t app_dirty = __sync_lock_test_and_set(&app_frame_dirty_mask, 0);
        struct rect shell_damage = {0, 0, 0, 0};
        struct rect damage = {0, 0, 0, 0};
        int have_shell_damage = take_damage(&shell_damage);
        int have_damage = have_shell_damage;
        if (have_shell_damage)
            damage = shell_damage;
        for (int slot = 0; slot < MAX_GUI_APPS; slot++) {
            if (!(app_dirty & (1u << slot)))
                continue;
            struct rect dirty;
            if (!app_take_dirty(slot, &dirty))
                continue;
            if (gpu_present_ready) {
                int sync = gpu_app_texture_sync(slot, dirty);
                if (sync > 0) {
                    app_note_dirty(slot, dirty);
                    continue;
                }
                if (sync < 0) {
                    /* A client back-buffer failure is not a compositor
                     * failure.  Keep its last complete front buffer and wait
                     * for a later frame instead of disabling VirGL globally. */
                    if (!app_sessions[slot].gpu_sync_warned) {
                        gui_log("[gui] app GPU frame dropped; retaining front buffer");
                        app_sessions[slot].gpu_sync_warned = 1;
                    }
                    continue;
                }
                app_sessions[slot].gpu_sync_warned = 0;
            }
            struct rect area = app_damage_to_screen(slot, dirty);
            if (area.w <= 0 || area.h <= 0)
                continue;
            damage = have_damage ? union_rect(damage, area) : area;
            have_damage = 1;
        }
        int full_dirty = __sync_lock_test_and_set(&desktop_dirty, 0);
        if (full_dirty) {
            render();
            note_pointer_drawn();
        } else if (have_damage) {
            /* App-list hover dirties whole rows frequently.  Those rects can
             * omit the previous cursor splat if it sat just outside a row
             * gap; always re-erase the last composed pointer. */
            if (gpu_present_ready) {
                if (have_shell_damage) {
                    shell_damage = damage_with_pointer(shell_damage);
                    damage = union_rect(damage, shell_damage);
                    if (gpu_shell_update(shell_damage) < 0) {
                        gpu_fallback_to_software(
                            "[gui] virgl shell upload failed; software fallback");
                        render();
                        note_pointer_drawn();
                        goto frame_done;
                    }
                }
                if (gpu_present_scene(damage) < 0) {
                    gpu_fallback_to_software(
                        "[gui] virgl scene/present failed; software fallback");
                    render();
                }
            } else {
                damage = damage_with_pointer(damage);
                (void)render_region(damage);
            }
            note_pointer_drawn();
        }
frame_done:
        loop_now = monotonic_ms();
        (void)gui_event_wait(event_sequence, gui_idle_timeout(loop_now));
    }
    shutdown_desktop();
    release_display();
    return 0;
}
