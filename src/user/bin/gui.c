#include "libc.h"
#include "guiapp.h"
#include "palette.h"
#include "uikit.h"
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

    WIN_LAUNCHER = 0,
    WIN_STATUS = 1,
    WIN_APP_BASE = 2,
    MAX_GUI_APPS = 10,
    WIN_COUNT = WIN_APP_BASE + MAX_GUI_APPS,

    MAX_APPS = 16,
    APP_DEFAULT_W = 560,
    APP_DEFAULT_H = 360,
    APP_SURFACE_MAX_W = GUIAPP_MAX_W,
    APP_SURFACE_MAX_H = GUIAPP_MAX_H,
    MAX_SW = GUIAPP_MAX_W,
    MAX_SH = GUIAPP_MAX_H,
    DISPLAY_MODE_COUNT = 11,
    DISPLAY_MODE_COLS = 2,
    DISPLAY_BTN_H = 34,
    DISPLAY_BTN_GAP = 6,
    DISPLAY_GROUP_LABEL_H = 22,
    DISPLAY_GROUP_GAP = 14,
    /* Content Y of the "Resolution" heading inside the System window. */
    STATUS_RES_HEAD_Y = 236,
    STATUS_RES_BODY_Y = 264,
    /* modern desktop layout: no top bar, so the work area starts at the screen top and
     * ends at the taskbar.  WORK_TOP is kept as a named zero because window
     * clamping, damage and maximise all measure from it. */
    WORK_TOP = 0,
    WINDOW_TITLE_H = UI_TITLEBAR_H,
    TASKBAR_H = UI_TASKBAR_H,
    LAUNCHER_HEADER_H = 36,
    LAUNCHER_ROW_STEP = 42,
    LAUNCHER_ROW_H = 36,
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
    START_COLS = 4,
    START_TILE = 96,
    START_TILE_GAP = 8,
    START_PAD = 20,
    START_FOOTER_H = 56,
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
    int reader_tid;
    volatile int reader_dead;
    volatile int closing;
    /* Opt-in desktop heartbeat (System Monitor). Broadcasting TICK to every
     * app forced full redraws every 500 ms even when idle. */
    int wants_tick;
    uint32_t shm_token;
    struct guiapp_shared_surface *shared;
    uint32_t gpu_resource;
    int gpu_resource_w;
    int gpu_resource_h;
    int gpu_resource_canvas;
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
/* Local fallback when scanout cannot be mapped into user space. */
static uint32_t fb_local[MAX_SW * MAX_SH];
/* Compose target: GPU/LFB scanout (zero-copy) or fb_local. */
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
static int app_last_click = -1;
static unsigned int app_last_click_tick;
static int taskbar_hover = -1;
static int taskbar_expanded;
static int start_open;
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
static unsigned int tick;
static unsigned int last_render_tick;
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
static void win_damage(int id) {
    if (id < 0 || id >= WIN_COUNT)
        return;
    queue_damage(shadow_bounds(windows[id].r));
}

/* Region a window may occupy: the screen minus the taskbar.  With the top bar
 * gone this starts at y=0, and a maximised window fills it edge to edge
 * rather than sitting inside a margin as it did around the old
 * floating dock. */
static struct rect work_area(void) {
    return (struct rect){0, WORK_TOP, sw, sh - WORK_TOP - TASKBAR_H};
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

static int gui_text_width(const char *s) {
    int width = 0;
    while (s && *s) {
        uint32_t cp = gui_utf8_next(&s);
        if (cp == '\n') break;
        width += gui_codepoint_advance(cp, width);
    }
    return width;
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
    default: return KEY_ESC;
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

static int app_send_event(int slot, int type, int x, int y, int key, int buttons, int wheel) {
    if (slot < 0 || slot >= MAX_GUI_APPS || !app_sessions[slot].used)
        return -1;
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
    return write_full(app_sessions[slot].to_fd, &ev, (int)sizeof(ev));
}

static int app_send_text(int slot, const char *value) {
    if (slot < 0 || slot >= MAX_GUI_APPS || !app_sessions[slot].used || !value)
        return -1;
    struct guiapp_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.magic = GUIAPP_MAGIC;
    ev.type = GUIAPP_EVT_TEXT;
    ev.width = app_sessions[slot].want_w;
    ev.height = app_sessions[slot].want_h;
    copy_text(ev.text, value, sizeof(ev.text));
    return write_full(app_sessions[slot].to_fd, &ev, (int)sizeof(ev));
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
    windows[id].r = (struct rect){
        80 + slot * 36, 74 + slot * 34,
        min_i(default_w, sw - 120),
        min_i(default_h, sh - 150)
    };
    windows[id].restore = windows[id].r;
    windows[id].visible = 1;
    windows[id].minimized = 0;
    windows[id].maximized = 0;

    app_sessions[slot].used = 1;
    app_sessions[slot].pid = pid;
    app_sessions[slot].to_fd = ev_pipe[1];
    app_sessions[slot].from_fd = frame_pipe[0];
    app_target_size(id, &app_sessions[slot].want_w, &app_sessions[slot].want_h);
    app_sessions[slot].surface_w = 0;
    app_sessions[slot].surface_h = 0;
    app_sessions[slot].resize_dirty = 0;
    app_sessions[slot].resize_inflight = 0;
    app_sessions[slot].reader_dead = 0;
    app_sessions[slot].closing = 0;
    app_sessions[slot].wants_tick = path && strstr(path, "taskmanager") != 0;
    app_sessions[slot].shm_token = mapping.token;
    app_sessions[slot].shared = shared;
    app_sessions[slot].gpu_resource = 0;
    app_sessions[slot].gpu_resource_w = 0;
    app_sessions[slot].gpu_resource_h = 0;
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
}

static void run_app(const char *path) {
    run_app_with_arg(path, 0);
}

static void activate(int id) {
    if (id < 0 || id >= WIN_COUNT)
        return;
    int old_focus = focus;
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
    if (pos >= 0) {
        for (int i = pos; i < WIN_COUNT - 1; i++)
            z_order[i] = z_order[i + 1];
        z_order[WIN_COUNT - 1] = id;
    }
    focus = id;
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
    int top = WORK_TOP;
    int dock = TASKBAR_H;
    int content_h = sh - top - dock - margin * 2;
    int left_w = min_i(max_i(360, sw / 3), 620);
    int right_w = min_i(max_i(400, sw / 3), 620);
    int launcher_h = min_i(content_h,
                           max_i(300, 126 + app_count * LAUNCHER_ROW_STEP));

    windows[WIN_LAUNCHER].title = "Applications";
    windows[WIN_LAUNCHER].dock_label = "Apps";
    windows[WIN_LAUNCHER].r = (struct rect){margin, top + margin,
                                            left_w, launcher_h};
    windows[WIN_LAUNCHER].restore = windows[WIN_LAUNCHER].r;
    windows[WIN_LAUNCHER].visible = 1;

    windows[WIN_STATUS].title = "System";
    windows[WIN_STATUS].dock_label = "Sys";
    windows[WIN_STATUS].r = (struct rect){
        sw - right_w - margin,
        top + margin,
        right_w,
        min_i(content_h, max_i(460, (content_h * 2) / 3))
    };
    windows[WIN_STATUS].restore = windows[WIN_STATUS].r;
    windows[WIN_STATUS].visible = 1;

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

/* Desktop wallpaper: a blue bloom over near-black, a calm desktop backdrop.
 *
 * The glow is separable -- a horizontal falloff times a vertical one -- which
 * makes it an axis-aligned elliptical gradient computable from two 1-D terms
 * per pixel.  A true radial bloom would need a square root per pixel, and the
 * background is repainted on every window drag, so the cost would be paid
 * continuously for a difference nobody can see through a blur this soft. */
static int wall_falloff(int pos, int centre, int spread) {
    int d = pos - centre;
    int t;
    if (d < 0)
        d = -d;
    if (spread <= 0 || d >= spread)
        return 0;
    /* Smoothstep, so the bloom has no visible edge where it fades out. */
    t = 255 - d * 255 / spread;
    return t * t / 255;
}

static void draw_background(void) {
    struct ui_surface s = ui_target();
    struct ui_rect c = ui_clipped(&s, ui_rect_make(0, 0, sw, sh));
    int cx = sw / 2;
    int cy = sh / 3;
    int spread_x = sw * 3 / 4;
    int spread_y = sh * 2 / 3;

    if (ui_rect_empty(c))
        return;
    for (int y = c.y; y < c.y + c.h; y++) {
        int vy = wall_falloff(y, cy, spread_y);
        uint32_t base = ui_lerp(UI_WALL_BASE, UI_WALL_MID,
                                sh > 1 ? y * 255 / (sh - 1) : 0);
        uint32_t *row = s.px + (size_t)y * s.stride;
        for (int x = c.x; x < c.x + c.w; x++) {
            int glow = wall_falloff(x, cx, spread_x) * vy / 255;
            row[x] = glow > 0 ? ui_blend(UI_WALL_GLOW, base, (uint32_t)glow)
                              : base;
        }
    }
}

static struct rect caption_rect(int id, int control);
static struct rect control_hit_rect(int id, int control);
static int control_hovered(int id, int control);
static int top_window_at(int x, int y);

/* One caption button: subtle fill on hover, red on close, and the outer
 * corner rounded so it sits flush inside the window frame. */
static void draw_caption_button(struct ui_surface *s, int id, int control) {
    struct rect b = caption_rect(id, control);
    struct ui_rect box = ui_of(b);
    int hovered = control_hovered(id, control);
    int pressed = hovered && (prev_buttons & 1);
    int is_close = control == 2;
    int corners = is_close ? UI_CORNER_TR : 0;
    uint32_t glyph = windows[id].active ? UI_TEXT_PRIMARY : UI_TEXT_TERTIARY;
    int icon;

    if (hovered) {
        uint32_t fill = is_close
            ? (pressed ? UI_CAPTION_CLOSE_PRESS : UI_CAPTION_CLOSE)
            : (pressed ? UI_SUBTLE_PRESSED : UI_SUBTLE_HOVER);
        ui_fill_round_mask(s, box, UI_RADIUS_WINDOW, corners, fill, 255);
        glyph = UI_TEXT_PRIMARY;
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
    frame = ui_of(w->r);
    /* A maximised window has no wallpaper around it to round against, so it
     * squares off as it does on any modern desktop. */
    radius = w->maximized ? 0 : UI_RADIUS_WINDOW;

    if (!w->maximized)
        ui_shadow(&s, frame, radius, UI_ELEV_FLYOUT_R,
                  w->active ? UI_ELEV_DIALOG_A : UI_ELEV_CARD_A, 4);
    ui_fill_round(&s, frame, radius, UI_BG_SOLID);
    title = ui_rect_make(frame.x, frame.y, frame.w, WINDOW_TITLE_H);
    ui_fill_round_mask(&s, title, radius, UI_CORNER_TOP,
                       w->active ? UI_BG_LAYER : UI_BG_SOLID, 255);
    ui_fill_a(&s, ui_rect_make(frame.x, frame.y + WINDOW_TITLE_H, frame.w, 1),
              UI_STROKE_DIVIDER, w->active ? 255 : 160);

    ts = ui_style(UI_FONT_BODY,
                  w->active ? UI_TEXT_PRIMARY : UI_TEXT_TERTIARY);
    ui_text_in(&s, ui_rect_make(frame.x + 12, frame.y,
                                frame.w - 12 - 3 * UI_CAPTION_BTN_W - 8,
                                WINDOW_TITLE_H),
               w->title, ts);

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
                      ui_rect_make(w->r.x + w->r.w - 6 - i * 4,
                                   w->r.y + w->r.h - 14 + i * 4, 4, 1),
                      UI_TEXT_TERTIARY, 200);
    }
}

static struct rect content_rect(int id) {
    struct rect r = windows[id].r;
    return (struct rect){r.x + 12, r.y + WINDOW_TITLE_H + 12,
                         r.w - 30, r.h - WINDOW_TITLE_H - 44};
}

/* Fit the source aspect ratio into the current content rect (letterboxed). */
static struct rect scaled_view_rect(int id, int slot) {
    struct rect c = content_rect(id);
    int source_w = app_sessions[slot].source_w;
    int source_h = app_sessions[slot].source_h;
    if (!app_sessions[slot].scaled_surface || source_w <= 0 || source_h <= 0)
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
        struct rect c = content_rect(id);
        int width = 120;
        for (int i = 0; i < app_count; i++)
            width = max_i(width, 42 + gui_text_width(apps[i].name));
        return max_i(c.w, width);
    }
    if (id == WIN_STATUS)
        return content_rect(id).w;
    return 520;
}

static int content_height(int id) {
    int slot = app_slot_for_win(id);
    if (slot >= 0 && app_sessions[slot].used)
        return content_rect(id).h;
    if (id == WIN_LAUNCHER)
        return LAUNCHER_HEADER_H +
               max_i(app_count, 1) * LAUNCHER_ROW_STEP + 12;
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

/* Content-local layout of mode button `index` inside a panel of width `inner_w`. */
static void display_mode_cell(int index, int inner_w,
                              int *x_out, int *y_out, int *w_out, int *h_out) {
    int y = STATUS_RES_BODY_Y;
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
    *y_out = STATUS_RES_BODY_Y;
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
    int y = STATUS_RES_BODY_Y;
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
    return (struct rect){ox, oy + STATUS_RES_BODY_Y, c.w, DISPLAY_GROUP_LABEL_H};
}

static struct rect fit_window_rect(struct rect r, int old_sw, int old_sh,
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
    int max_h = max_i(WIN_MIN_H, dock_y - WORK_TOP - 10);
    if (r.w > max_w) r.w = max_w;
    if (r.h > max_h) r.h = max_h;
    if (r.x < 8) r.x = 8;
    if (r.y < WORK_TOP + 4) r.y = WORK_TOP + 4;
    if (r.x + r.w > sw - 8) r.x = sw - 8 - r.w;
    if (r.y + r.h > dock_y - 6) r.y = dock_y - 6 - r.h;
    return r;
}

static void relayout_after_mode_change(int old_sw, int old_sh) {
    int margin = max_i(18, sw / 48);
    int content_h = sh - WORK_TOP - TASKBAR_H - margin * 2;
    int left_w = min_i(max_i(360, sw / 3), 620);
    int right_w = min_i(max_i(400, sw / 3), 620);
    int launcher_h = min_i(content_h,
                           max_i(300, 126 + app_count * LAUNCHER_ROW_STEP));
    struct rect launcher = {margin, WORK_TOP + margin, left_w, launcher_h};
    struct rect status = {
        sw - right_w - margin, WORK_TOP + margin, right_w,
        min_i(content_h, max_i(460, (content_h * 2) / 3))
    };
    int dock_y = sh - TASKBAR_H;
    struct rect work = work_area();

    windows[WIN_LAUNCHER].restore = launcher;
    windows[WIN_LAUNCHER].r = windows[WIN_LAUNCHER].maximized ? work : launcher;
    windows[WIN_STATUS].restore = status;
    windows[WIN_STATUS].r = windows[WIN_STATUS].maximized ? work : status;

    for (int slot = 0; slot < MAX_GUI_APPS; slot++) {
        int id = WIN_APP_BASE + slot;
        struct rect restore = windows[id].maximized ? windows[id].restore
                                                     : windows[id].r;
        restore = fit_window_rect(restore, old_sw, old_sh, dock_y);
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

static int switch_display_mode(int index) {
    if (index < 0 || index >= DISPLAY_MODE_COUNT)
        return -1;
    if (current_display_mode() == index)
        return 0;
    int old_sw = sw;
    int old_sh = sh;
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
    relayout_after_mode_change(old_sw, old_sh);
    pointer_x = clamp_i(pointer_x, 0, sw - 1);
    pointer_y = clamp_i(pointer_y, 0, sh - 1);
    if (hardware_cursor_ready &&
        gfx_cursor_move(pointer_x, pointer_y, 1) < 0)
        hardware_cursor_ready = 0;
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
    vt = vscroll_track(id);
    ht = hscroll_track(id);
    /* Tracks live just outside the content clip.  Clear them on every
     * redraw so a window that shrinks from scrollable to non-scrollable does
     * not retain the previous track/thumb pixels. */
    fill(vt, UI_BG_SOLID);
    fill(ht, UI_BG_SOLID);
    if (max_scroll_y(id) > 0) {
        struct rect th = vscroll_thumb(id);
        int hot = inside(pointer_x, pointer_y, vt) || scroll_drag_win == id;
        if (hot)
            ui_fill_round(&s, ui_of(vt), vt.w / 2, UI_BG_MICA_ALT);
        ui_fill_round(&s,
                      ui_rect_make(th.x + (hot ? 2 : 3), th.y + 2,
                                   th.w - (hot ? 4 : 6), th.h - 4),
                      3, hot ? UI_TEXT_SECONDARY : UI_TEXT_TERTIARY);
    }
    if (max_scroll_x(id) > 0) {
        struct rect th = hscroll_thumb(id);
        int hot = inside(pointer_x, pointer_y, ht) || scroll_drag_win == id;
        if (hot)
            ui_fill_round(&s, ui_of(ht), ht.h / 2, UI_BG_MICA_ALT);
        ui_fill_round(&s,
                      ui_rect_make(th.x + 2, th.y + (hot ? 2 : 3),
                                   th.w - 4, th.h - (hot ? 4 : 6)),
                      3, hot ? UI_TEXT_SECONDARY : UI_TEXT_TERTIARY);
    }
}

static void draw_launcher(void) {
    struct ui_surface s;
    struct rect c, saved_clip, clip;
    struct ui_text_style head;
    int ox, oy, y, hover_row;

    if (!windows[WIN_LAUNCHER].visible || windows[WIN_LAUNCHER].minimized)
        return;
    draw_window_frame(WIN_LAUNCHER);
    s = ui_target();
    c = content_rect(WIN_LAUNCHER);
    fill(c, UI_BG_SOLID);
    /* Same content clip as System: row fills must not paint past the body. */
    saved_clip = compose_clip_push(c);
    clip = c;
    /* ui_target() snapshots compose_clip, so take it after the push. */
    s = ui_target();
    ox = c.x - scroll_x[WIN_LAUNCHER];
    oy = c.y - scroll_y[WIN_LAUNCHER];

    head = ui_style(UI_FONT_BODY, UI_TEXT_SECONDARY);
    head.bold = 1;
    ui_text_in(&s, ui_rect_make(ox, oy, c.w, LAUNCHER_HEADER_H - 8),
               "Installed", head);
    y = oy + LAUNCHER_HEADER_H;
    if (app_count == 0) {
        ui_text_in(&s, ui_rect_make(ox, y, c.w, LAUNCHER_ROW_H),
                   "No apps in /fs/apps",
                   ui_style(UI_FONT_BODY, UI_TEXT_TERTIARY));
        compose_clip_pop(saved_clip);
        draw_scrollbars(WIN_LAUNCHER);
        return;
    }
    hover_row = hit_launcher_row_at(pointer_x, pointer_y);
    for (int i = 0; i < app_count; i++) {
        struct rect row = launcher_row_paint_rect(i);
        struct rect visible = intersect_rect(row, clip);
        struct ui_rect fill_r;
        int selected = i == app_selected;
        int hovered = i == hover_row;
        if (visible.w <= 0 || visible.h <= 0)
            continue;
        fill_r = ui_rect_make(row.x, row.y, row.w - 6, row.h);
        if (selected)
            ui_fill_round(&s, fill_r, UI_RADIUS_CONTROL, UI_SUBTLE_HOVER);
        else if (hovered)
            ui_fill_round(&s, fill_r, UI_RADIUS_CONTROL, UI_SUBTLE_PRESSED);
        /* the theme marks the selected list row with an accent bar on its
         * leading edge rather than by filling the whole row. */
        if (selected)
            ui_fill_round(&s, ui_rect_make(row.x + 1, row.y + row.h / 4, 3,
                                           row.h / 2),
                          1, UI_ACCENT_FILL);
        ui_icon(&s, app_icon_for(apps[i].name), row.x + 14,
                row.y + (row.h - 18) / 2, 18,
                selected ? UI_ACCENT_FILL : UI_TEXT_SECONDARY, 255);
        ui_text_in(&s, ui_rect_make(row.x + 42, row.y, row.w - 50, row.h),
                   apps[i].name,
                   ui_style(UI_FONT_BODY,
                            selected || hovered ? UI_TEXT_PRIMARY
                                                : UI_TEXT_SECONDARY));
    }
    compose_clip_pop(saved_clip);
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
    c = content_rect(WIN_STATUS);
    fill(c, UI_BG_SOLID);
    /* Restrict paint to the scrollable body.  The button and rounded-fill
     * helpers only honour compose_clip, so without this push the resolution
     * grid paints through the frame onto the desktop whenever content_height
     * exceeds the window. */
    saved_clip = compose_clip_push(c);
    s = ui_target();
    ox = c.x - scroll_x[WIN_STATUS];
    oy = c.y - scroll_y[WIN_STATUS];
    step = ui_line_height(UI_FONT_BODY) + 6;

    head = ui_style(UI_FONT_BODY_LG, UI_TEXT_PRIMARY);
    head.bold = 1;
    body = ui_style(UI_FONT_BODY, UI_TEXT_SECONDARY);
    faint = ui_style(UI_FONT_BODY, UI_TEXT_TERTIARY);

#define STATUS_ROW(n) ui_rect_make(ox, oy + (n) * step, c.w, step)
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
    {
        struct gpu3d_caps caps;
        if (gpu3d_info(&caps) == 0 && caps.available)
            ui_text_in(&s, STATUS_ROW(row++), "virgl 3D available", faint);
    }
    copy_text(line, "Applications ", sizeof(line));
    append_uint(line, (unsigned int)app_count, sizeof(line));
    ui_text_in(&s, STATUS_ROW(row++), line, body);

    row++;
    ui_text_in(&s, STATUS_ROW(row++), "Controls", head);
    ui_text_in(&s, STATUS_ROW(row++), "Ctrl+Space toggles IME", body);
    ui_text_in(&s, STATUS_ROW(row++), "[ ] cycle resolution", body);
    ui_text_in(&s, STATUS_ROW(row++), "Esc returns to shell", body);
#undef STATUS_ROW

    mode_error = mode_error_until && tick < mode_error_until;
    {
        struct ui_text_style st = head;
        if (mode_error)
            st.color = UI_SYS_CRITICAL;
        ui_text_in(&s, ui_rect_make(ox, oy + STATUS_RES_HEAD_Y, c.w,
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
        struct rect label_r = status_group_label_rect(group);
        ui_text_in(&s, ui_of(label_r), display_modes[start].ratio, faint);
        for (int i = 0; i < count; i++) {
            int mode = start + i;
            struct rect btn = status_mode_rect(mode);
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
    struct rect c = content_rect(id);
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
        struct rect view = scaled_view_rect(id, slot);
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
                           int active, int open, int hovered, int pressed) {
    struct ui_rect box = ui_of(r);
    uint32_t tint = UI_TEXT_PRIMARY;
    if (pressed)
        ui_fill_round(s, box, UI_RADIUS_CONTROL, UI_SUBTLE_PRESSED);
    else if (active || hovered)
        ui_fill_round(s, box, UI_RADIUS_CONTROL,
                      active ? UI_SUBTLE_HOVER : UI_SUBTLE_PRESSED);
    ui_icon_in(s, icon, box, TB_ICON, tint, active ? 255 : 225);
    if (open) {
        int w = active ? 16 : 6;
        ui_fill_round(s, ui_rect_make(r.x + (r.w - w) / 2, r.y + r.h - 2, w, 3),
                      1, active ? UI_ACCENT_FILL : UI_TEXT_TERTIARY);
    }
}

static void draw_taskbar(void) {
    struct ui_surface s = ui_target();
    struct tb_item items[TB_MAX_ITEMS];
    int count = taskbar_items(items);
    struct rect bar = taskbar_rect();
    char clock[16];

    /* Acrylic: the wallpaper and any window edge under the bar show through
     * blurred, which is the single strongest the theme cue. */
    shell_acrylic(&s, ui_of(bar), 0, UI_BG_ACRYLIC_THIN, 190);
    ui_fill_a(&s, ui_rect_make(bar.x, bar.y, bar.w, 1), UI_STROKE_SURFACE,
              150);

    for (int i = 0; i < count; i++) {
        struct tb_item *it = &items[i];
        int hovered = inside(pointer_x, pointer_y, it->r);
        int pressed = hovered && (prev_buttons & 1);
        switch (it->kind) {
        case TB_START:
            draw_tb_button(&s, it->r, UI_ICON_START, start_open, 0, hovered,
                           pressed);
            break;
        case TB_WINDOW: {
            int id = it->value;
            draw_tb_button(&s, it->r, window_icon(id), windows[id].active,
                           windows[id].visible, hovered, pressed);
            break;
        }
        case TB_OVERFLOW:
            draw_tb_button(&s, it->r, UI_ICON_MORE, taskbar_expanded, 0,
                           hovered, pressed);
            break;
        case TB_TRAY_IME: {
            struct ui_rect box = ui_of(it->r);
            if (hovered)
                ui_fill_round(&s, box, UI_RADIUS_CONTROL, UI_SUBTLE_HOVER);
            ui_icon_in(&s, UI_ICON_KEYBOARD, box, 20,
                       ime_enabled ? UI_ACCENT_FILL : UI_TEXT_SECONDARY,
                       255);
            break;
        }
        case TB_TRAY_CLOCK: {
            struct ui_rect box = ui_of(it->r);
            struct ui_text_style st = ui_style(UI_FONT_CAPTION,
                                               UI_TEXT_SECONDARY);
            if (hovered)
                ui_fill_round(&s, box, UI_RADIUS_CONTROL, UI_SUBTLE_HOVER);
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
 * Same discipline as the taskbar: the tile grid is described once and both
 * the painter and the hit tester read it.
 */

static int start_rows(void) {
    int rows = (app_count + START_COLS - 1) / START_COLS;
    return rows < 1 ? 1 : rows;
}

static struct rect start_menu_rect(void) {
    int w = min_i(START_W, sw - 24);
    int h = START_PAD * 2 + ui_line_height(UI_FONT_BODY) + 8 +
            start_rows() * (START_TILE + START_TILE_GAP) + START_FOOTER_H;
    int y = sh - TASKBAR_H - h - 12;
    if (h > sh - TASKBAR_H - 24) {
        h = sh - TASKBAR_H - 24;
        y = 12;
    }
    return (struct rect){(sw - w) / 2, y, w, h};
}

static struct rect start_tile_rect(int index) {
    struct rect panel = start_menu_rect();
    int grid_w = START_COLS * START_TILE + (START_COLS - 1) * START_TILE_GAP;
    int gx = panel.x + (panel.w - grid_w) / 2;
    int gy = panel.y + START_PAD + ui_line_height(UI_FONT_BODY) + 8;
    int col = index % START_COLS;
    int row = index / START_COLS;
    return (struct rect){gx + col * (START_TILE + START_TILE_GAP),
                         gy + row * (START_TILE + START_TILE_GAP),
                         START_TILE, START_TILE};
}

static struct rect start_power_rect(void) {
    struct rect panel = start_menu_rect();
    return (struct rect){panel.x + panel.w - START_PAD - 40,
                         panel.y + panel.h - START_PAD - 36, 40, 36};
}

static void draw_start_menu(void) {
    struct ui_surface s;
    struct rect panel;
    struct ui_rect p;
    struct ui_text_style head;
    struct rect power;
    struct ui_rect saved;

    if (!start_open)
        return;
    s = ui_target();
    panel = start_menu_rect();
    p = ui_of(panel);

    ui_shadow(&s, p, UI_RADIUS_OVERLAY, UI_ELEV_DIALOG_R, UI_ELEV_DIALOG_A,
              6);
    shell_acrylic(&s, p, UI_RADIUS_OVERLAY, UI_BG_ACRYLIC, 215);
    ui_stroke_round(&s, p, UI_RADIUS_OVERLAY, 1, UI_STROKE_SURFACE, 255);

    head = ui_style(UI_FONT_BODY, UI_TEXT_PRIMARY);
    head.bold = 1;
    ui_text_in(&s, ui_rect_make(panel.x + START_PAD, panel.y + START_PAD,
                                panel.w - START_PAD * 2,
                                ui_line_height(UI_FONT_BODY)),
               "Pinned", head);

    saved = ui_clip_push(&s, ui_rect_make(panel.x, panel.y, panel.w,
                                          panel.h - START_FOOTER_H));
    for (int i = 0; i < app_count; i++) {
        struct rect tile = start_tile_rect(i);
        struct ui_rect t = ui_of(tile);
        struct ui_text_style label = ui_style(UI_FONT_CAPTION,
                                              UI_TEXT_PRIMARY);
        int hovered = inside(pointer_x, pointer_y, tile);
        int pressed = hovered && (prev_buttons & 1);
        if (pressed)
            ui_fill_round(&s, t, UI_RADIUS_CONTROL, UI_SUBTLE_PRESSED);
        else if (hovered)
            ui_fill_round(&s, t, UI_RADIUS_CONTROL, UI_SUBTLE_HOVER);
        ui_icon(&s, app_icon_for(apps[i].name), tile.x + (tile.w - 32) / 2,
                tile.y + 16, 32, UI_TEXT_PRIMARY, 255);
        label.align = UI_ALIGN_CENTER;
        ui_text_in(&s, ui_rect_make(tile.x + 4, tile.y + tile.h - 26,
                                    tile.w - 8, 20),
                   apps[i].name, label);
    }
    ui_clip_pop(&s, saved);

    ui_fill_a(&s, ui_rect_make(panel.x + 1,
                               panel.y + panel.h - START_FOOTER_H, panel.w - 2,
                               1),
              UI_STROKE_DIVIDER, 200);
    ui_text_in(&s, ui_rect_make(panel.x + START_PAD,
                                panel.y + panel.h - START_FOOTER_H,
                                panel.w / 2, START_FOOTER_H),
               "BuzzOS", ui_style(UI_FONT_BODY, UI_TEXT_SECONDARY));

    power = start_power_rect();
    {
        struct ui_rect pw = ui_of(power);
        int hovered = inside(pointer_x, pointer_y, power);
        if (hovered)
            ui_fill_round(&s, pw, UI_RADIUS_CONTROL, UI_SUBTLE_HOVER);
        ui_icon_in(&s, UI_ICON_POWER, pw, 20, UI_TEXT_SECONDARY, 255);
    }
}

/* Returns the app index to launch, -2 for the power button, or -1 for a
 * click that the menu swallows without acting on. */
static int hit_start_menu(int x, int y) {
    struct rect panel;
    if (!start_open)
        return -1;
    panel = start_menu_rect();
    if (!inside(x, y, panel))
        return -1;
    if (inside(x, y, start_power_rect()))
        return -2;
    for (int i = 0; i < app_count; i++)
        if (inside(x, y, start_tile_rect(i)))
            return i;
    return -1;
}

/* Damage the Start menu plus its shadow. */
static void start_damage(void) {
    struct rect panel = start_menu_rect();
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
    hover_start_tile = -1;
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
    if (panel_y < WORK_TOP + 4)
        panel_y = WORK_TOP + 4;
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
    if (gfx_map_surface(&map) == 0 && map.pixels &&
        map.width >= (uint32_t)sw && map.height >= (uint32_t)sh &&
        map.stride_pixels >= (uint32_t)sw) {
        fb = map.pixels;
        fb_stride = (int)map.stride_pixels;
        scanout_direct = 1;
        display_backend = map.backend;
        return 0;
    }
    fb = fb_local;
    fb_stride = MAX_SW;
    scanout_direct = 0;
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
    GPU_OVERLAY_LAYER = GPU_APP_LAYER_BASE + MAX_GUI_APPS,
    GPU_SCENE_LAYER,
    GPU_BLUR_PING_LAYER,
    GPU_BLUR_PONG_LAYER,
};

_Static_assert(GPU_BLUR_PONG_LAYER < GPUCOMP_MAX_LAYERS,
               "GPU compositor layer table is too small");

static int gpu_app_layer(int slot) {
    return GPU_APP_LAYER_BASE + slot;
}

static void gpu_app_texture_release(int slot) {
    if (slot < 0 || slot >= MAX_GUI_APPS)
        return;
    if (app_sessions[slot].gpu_resource) {
        (void)gpucomp_layer_release(gpu_app_layer(slot), 0);
        if (!app_sessions[slot].gpu_resource_canvas)
            (void)gpu3d_resource_destroy(app_sessions[slot].gpu_resource);
    }
    app_sessions[slot].gpu_resource = 0;
    app_sessions[slot].gpu_resource_w = 0;
    app_sessions[slot].gpu_resource_h = 0;
    app_sessions[slot].gpu_resource_canvas = 0;
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
    layer = gpu_app_layer(slot);
    if (!session->used || !session->canvas_mode ||
        session->source_w <= 0 || session->source_h <= 0)
        return -1;
    if (!session->gpu_resource_canvas ||
        session->gpu_resource_w != session->source_w ||
        session->gpu_resource_h != session->source_h) {
        if (session->gpu_resource && !session->gpu_resource_canvas)
            gpu_app_texture_release(slot);
        if (gpucomp_canvas_ensure(layer, session->source_w,
                                  session->source_h) < 0)
            return -1;
        session->gpu_resource = gpucomp_layer_resource(layer);
        session->gpu_resource_w = session->source_w;
        session->gpu_resource_h = session->source_h;
        session->gpu_resource_canvas = 1;
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
    app_dirty_unlock(slot);
    return result;
}

/* Bind the app's existing shared pixel pages directly as a virgl texture.
 * There is no GUI-side memcpy: TRANSFER_TO_HOST_3D reads the pages the app
 * published through guiapp. */
static int gpu_app_texture_sync(int slot, struct rect dirty) {
    struct app_session *session;
    int width, height;
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

    if (session->gpu_resource_canvas || !session->gpu_resource ||
        session->gpu_resource_w != width ||
        session->gpu_resource_h != height) {
        uint32_t resource = 0;
        gpu_app_texture_release(slot);
        if (gpu3d_resource_import_shm(
                session->shm_token, GUIAPP_SHARED_HEADER_SIZE,
                VIRGL_TARGET_TEXTURE_2D, VIRGL_FORMAT_B8G8R8X8_UNORM,
                VIRGL_BIND_SAMPLER_VIEW, (uint32_t)width, (uint32_t)height,
                &resource) < 0)
            return -1;
        session->gpu_resource = resource;
        session->gpu_resource_w = width;
        session->gpu_resource_h = height;
        if (gpucomp_layer_import(gpu_app_layer(slot), resource, width, height,
                                 VIRGL_FORMAT_B8G8R8X8_UNORM) < 0) {
            gpu_app_texture_release(slot);
            return -1;
        }
        dirty = (struct rect){0, 0, width, height};
    }

    dirty = intersect_rect(dirty, (struct rect){0, 0, width, height});
    if (dirty.w <= 0 || dirty.h <= 0)
        return 0;
    /* Do not issue DMA while the application is in the middle of publishing
     * a new dirty rectangle.  Its reader thread will wake us for the newer
     * sequence, so yielding here coalesces instead of copying torn pixels. */
    uint32_t sequence = session->shared->sequence;
    if (sequence & 1u)
        return 1;
    __sync_synchronize();
    int result = gpu3d_upload(session->gpu_resource, dirty.x, dirty.y,
                              dirty.w, dirty.h);
    __sync_synchronize();
    if (result == 0 && session->shared->sequence != sequence) {
        /* The app started a newer publish while TRANSFER_TO_HOST_3D was
         * reading.  Queue that generation instead of presenting a tear. */
        app_note_dirty(slot, (struct rect){0, 0, width, height});
        return 1;
    }
    return result;
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
        !windows[id].visible || windows[id].minimized)
        return;
    content = content_rect(id);
    destination = session->scaled_surface
        ? scaled_view_rect(id, slot)
        : (struct rect){content.x, content.y,
                        session->source_w, session->source_h};
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
            count = gpu_visible_cut(visible, count, windows[above].r);
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
    ADD_ACRYLIC(taskbar_rect(), 0, UI_BG_ACRYLIC_THIN, 190);
    if (taskbar_expanded) {
        int ids[MAX_GUI_APPS];
        int hidden = taskbar_hidden_windows(ids);
        if (hidden > 0)
            ADD_ACRYLIC(taskbar_overflow_panel(hidden), UI_RADIUS_OVERLAY,
                        UI_BG_ACRYLIC, 205);
    }
    if (start_open)
        ADD_ACRYLIC(start_menu_rect(), UI_RADIUS_OVERLAY,
                    UI_BG_ACRYLIC, 215);
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

static void gpu_fallback_to_software(void) {
    gpu_present_shutdown();
    (void)bind_scanout();
    gpu_canvas_capability_set(0);
    desktop_dirty = 1;
    gui_log("[gui] virgl compositor failed; software fallback");
}

static int render_region(struct rect area) {
    area = intersect_rect(area, (struct rect){0, 0, sw, sh});
    if (area.w <= 0 || area.h <= 0)
        return 0;
    if (gpu_present_ready) {
        if (gpu_shell_update(area) == 0 && gpu_present_scene(area) == 0)
            return 0;
        gpu_fallback_to_software();
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
    struct rect content = content_rect(id);
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
    struct rect view = scaled_view_rect(id, slot);
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
        if (windows[i].visible && !windows[i].minimized && inside(x, y, windows[i].r))
            return i;
    }
    return -1;
}

static int hit_window_title(int x, int y) {
    int i = top_window_at(x, y);
    if (i < 0)
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
    if (i < 0)
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

    if (r.w < WIN_MIN_W) {
        if (resize_edges & 1)
            r.x -= WIN_MIN_W - r.w;
        r.w = WIN_MIN_W;
    }
    if (r.h < WIN_MIN_H) {
        if (resize_edges & 4)
            r.y -= WIN_MIN_H - r.h;
        r.h = WIN_MIN_H;
    }
    if (r.x < 0) {
        r.w += r.x;
        r.x = 0;
    }
    if (r.y < WORK_TOP) {
        r.h += r.y - WORK_TOP;
        r.y = WORK_TOP;
    }
    if (r.x + r.w > sw)
        r.w = sw - r.x;
    if (r.y + r.h > sh - 12)
        r.h = sh - 12 - r.y;
    if (r.w < WIN_MIN_W)
        r.w = min_i(WIN_MIN_W, sw - r.x);
    if (r.h < WIN_MIN_H)
        r.h = min_i(WIN_MIN_H, sh - 12 - r.y);

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
    if (id < 0 || id >= WIN_COUNT)
        return;
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
    if (id < 0 || id >= WIN_COUNT)
        return;
    if (hover_app == id)
        hover_app = -1;
    int slot = app_slot_for_win(id);
    if (slot >= 0 && app_sessions[slot].used) {
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

static void reap_dead_apps(void) {
    for (int slot = 0; slot < MAX_GUI_APPS; slot++) {
        if (app_sessions[slot].used && app_sessions[slot].reader_dead) {
            gui_log("[gui] app protocol ended");
            close_window(WIN_APP_BASE + slot);
        }
    }
}

static void toggle_maximize(int id) {
    if (id < 0 || id >= WIN_COUNT)
        return;
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
static void apply_snap(int id) {
    int zone = snap_zone_at(pointer_x, pointer_y);
    struct rect target;
    if (zone == SNAP_NONE || id < 0 || id >= WIN_COUNT)
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
    if (slot < 0 || !app_sessions[slot].used)
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
    if (!context_open) {
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

static void refresh_timed_shell(uint32_t now) {
    /* Keep the old approximately-60-Hz time unit without requiring a 60-Hz
     * polling loop.  Only actual deadlines cause damage. */
    tick = now / 16u;
    uint32_t second = now / 1000u;
    if (second != last_clock_second) {
        last_clock_second = second;
        taskbar_clock_damage();
    }
    if (mode_error_until && tick >= mode_error_until) {
        mode_error_until = 0;
        win_damage(WIN_STATUS);
    }
}

static unsigned int gui_idle_timeout(uint32_t now) {
    unsigned int timeout = 1000u - now % 1000u;
    if (has_app_tick_clients()) {
        uint32_t elapsed = now - last_app_tick_ms;
        unsigned int app_timeout = elapsed >= 500u ? 1u : 500u - elapsed;
        if (app_timeout < timeout)
            timeout = app_timeout;
    }
    if (!gpu_present_ready) {
        unsigned int elapsed_ticks = tick - last_render_tick;
        unsigned int redraw_timeout = elapsed_ticks >= 60u
            ? 1u : (60u - elapsed_ticks) * 16u;
        if (redraw_timeout < timeout)
            timeout = redraw_timeout;
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
        if (!windows[id].visible || windows[id].minimized)
            continue;
        if (id >= WIN_APP_BASE && !app_sessions[id - WIN_APP_BASE].used)
            continue;
        activate(id);
        return;
    }
}

static void handle_key(int k) {
    if (ime_handle_key(k))
        return;
    if (k == KEY_ESC) {
        running = 0;
        return;
    }
    if (k == '\t') {
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
        int old_selected = app_selected;
        if (k == KEY_UP && app_selected > 0)
            app_selected--;
        else if (k == KEY_DOWN && app_selected + 1 < app_count)
            app_selected++;
        else if ((k == '\n' || k == '\r') && app_count > 0) {
            run_app(apps[app_selected].path);
            return;
        } else if (k == 'r' || k == 'R') {
            scan_apps();
            desktop_dirty = 1;
            return;
        }
        if (app_selected != old_selected)
            win_damage(WIN_LAUNCHER);
        return;
    }
    int slot = app_slot_for_win(focus);
    if (slot >= 0 && app_sessions[slot].used) {
        if (app_send_event(slot, GUIAPP_EVT_KEY, 0, 0, k, 1, 0) < 0)
            app_sessions[slot].reader_dead = 1;
    }
}

static void forward_key_releases(void) {
    if (keyevent_fd < 0)
        return;
    uint16_t event;
    while (read(keyevent_fd, &event, sizeof(event)) == (int)sizeof(event)) {
        if (event & 0x8000u)
            continue;
        int slot = app_slot_for_win(focus);
        if (slot >= 0 && app_sessions[slot].used &&
            app_send_event(slot, GUIAPP_EVT_KEY, 0, 0,
                           event & 0x7FFFu, 0, 0) < 0)
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
    struct rect c = content_rect(WIN_LAUNCHER);
    int ox = c.x - scroll_x[WIN_LAUNCHER];
    int oy = c.y - scroll_y[WIN_LAUNCHER];
    int row_w = content_width(WIN_LAUNCHER) - 20;
    if (row_w > c.w)
        row_w = c.w;
    if (row_w < 1)
        row_w = 1;
    struct rect row = {
        ox,
        oy + LAUNCHER_HEADER_H + index * LAUNCHER_ROW_STEP,
        row_w,
        LAUNCHER_ROW_H
    };
    return row;
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
    int rel = y - (c.y + LAUNCHER_HEADER_H) + scroll_y[WIN_LAUNCHER];
    if (rel < 0)
        return -1;
    int idx = rel / LAUNCHER_ROW_STEP;
    if (idx < 0 || idx >= app_count)
        return -1;
    /* Ignore the inter-row gap (STEP - ROW_H); it is not painted as hover. */
    int row_top = idx * LAUNCHER_ROW_STEP;
    if (rel < row_top || rel >= row_top + LAUNCHER_ROW_H)
        return -1;
    return idx;
}

/* Recompute pointer-driven hover and damage full widget bounds on change. */
static void refresh_pointer_hover_damage(void) {
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
            damage_widget(start_tile_rect(hover_start_tile));
        if (tile >= 0)
            damage_widget(start_tile_rect(tile));
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
        int h = (start_open &&
                 inside(pointer_x, pointer_y, start_menu_rect()))
            ? -1 : hit_window(pointer_x, pointer_y);
        if (h >= 0) {
            int slot = app_slot_for_win(h);
            if (slot >= 0 && app_sessions[slot].used && inside(pointer_x, pointer_y, content_rect(h)))
                send_mouse_to_app(h, ms.buttons, wheel_delta);
            else {
                scroll_y[h] -= wheel_delta * 44;
                clamp_scroll(h);
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
            running = 0;
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
            activate(taskbar_hover);
            taskbar_expanded = 0;
            start_set_open(0);
        } else {
            start_set_open(0);
            int control = -1;
            int ctl_win = hit_control(pointer_x, pointer_y, &control);
            if (ctl_win >= 0) {
                activate(ctl_win);
                if (control == 0)
                    minimize_window(ctl_win);
                else if (control == 1)
                    toggle_maximize(ctl_win);
                else
                    close_window(ctl_win);
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
                if (launcher_row == app_last_click &&
                    tick - app_last_click_tick <= 25u) {
                    app_selected = launcher_row;
                    app_last_click = -1;
                    run_app(apps[launcher_row].path);
                    prev_buttons = ms.buttons;
                    return;
                }
                app_selected = launcher_row;
                app_last_click = launcher_row;
                app_last_click_tick = tick;
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
        update_hover_app(1);
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
        queue_damage(windows[id].r);
        prev_buttons = ms.buttons;
        return;
    }
    if (left && drag_win >= 0) {
        struct rect *r = &windows[drag_win].r;
        struct rect old = *r;
        if (windows[drag_win].maximized) {
            windows[drag_win].maximized = 0;
            windows[drag_win].restore = *r;
        }
        r->x = pointer_x - drag_dx;
        r->y = pointer_y - drag_dy;
        if (r->x < 0) r->x = 0;
        if (r->y < WORK_TOP) r->y = WORK_TOP;
        if (r->x + r->w > sw) r->x = sw - r->w;
        if (r->y + r->h > sh - 12) r->y = sh - 12 - r->h;
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
        gui_log(display_backend == GFX_BACKEND_VIRTIO_GPU_2D
                    ? "[gui] zero-copy virtio-gpu scanout"
                    : "[gui] zero-copy linear framebuffer");
    else
        gui_log("[gui] software backbuffer (scanout map failed)");
    gfx_set_origin(0, 0);
    pointer_x = sw / 2;
    pointer_y = sh / 2;
    init_hardware_cursor();
    /* Try the GPU present path after the scanout is bound: it retargets the
     * compose buffer, and needs the software path as a working fallback. */
    gpu_present_init();
    scan_apps();
    keyevent_fd = open("/dev/keyevent", O_RDONLY);
    layout();
}

static void shutdown_desktop(void) {
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
        flush_pending_app_resizes();
        send_app_ticks();
        uint32_t app_dirty = __sync_lock_test_and_set(&app_frame_dirty_mask, 0);
        struct rect shell_damage = {0, 0, 0, 0};
        struct rect damage = {0, 0, 0, 0};
        int have_shell_damage = take_damage(&shell_damage);
        int have_damage = have_shell_damage;
        int gpu_sync_failed = 0;
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
                    gpu_sync_failed = 1;
                    break;
                }
            }
            struct rect area = app_damage_to_screen(slot, dirty);
            if (area.w <= 0 || area.h <= 0)
                continue;
            damage = have_damage ? union_rect(damage, area) : area;
            have_damage = 1;
        }
        int full_dirty = __sync_lock_test_and_set(&desktop_dirty, 0);
        if (gpu_sync_failed) {
            gpu_fallback_to_software();
            full_dirty = 1;
        }
        if (full_dirty || (!gpu_present_ready &&
                           tick - last_render_tick >= 60u)) {
            render();
            last_render_tick = tick;
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
                        gpu_fallback_to_software();
                        render();
                        last_render_tick = tick;
                        note_pointer_drawn();
                        goto frame_done;
                    }
                }
                if (gpu_present_scene(damage) < 0) {
                    gpu_fallback_to_software();
                    render();
                    last_render_tick = tick;
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
