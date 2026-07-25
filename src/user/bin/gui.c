#include "libc.h"
#include "guiapp.h"
#include "palette.h"
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
    STATUS_RES_HEAD_Y = 264,
    STATUS_RES_BODY_Y = 294,
    TOPBAR_H = 36,
    WINDOW_TITLE_H = 34,
    DOCK_RESERVED_H = 82,
    LAUNCHER_HEADER_H = 44,
    LAUNCHER_ROW_STEP = 62,
    LAUNCHER_ROW_H = 58,
    DOCK_SYSTEM_STEP = 54,
    DOCK_SYSTEM_W = 46,
    DOCK_TASK_STEP = 54,
    DOCK_TASK_W = 46,
    DOCK_MORE_W = 46,
    DOCK_TOOLTIP_DELAY = 24,
    CONTEXT_MENU_W = 184,
    CONTEXT_ITEM_STEP = 38,
    CONTEXT_ITEM_H = 34,
    CONTEXT_MENU_H = 126,
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
    struct rect r;
    struct rect restore;
    int visible;
    int active;
    int minimized;
    int maximized;
};

struct app_entry {
    char file[24];
    char name[32];
    char summary[64];
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
static int dock_hover = -1;
static int dock_expanded;
static unsigned int dock_hover_since;
static int dock_tooltip_visible;
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
/* Last position actually composed into the backbuffer / scanned out.  Any
 * dirty-rect render must re-erase this spot; otherwise a later partial
 * update (common when skimming the app list's per-row hover damage) leaves
 * a ghost cursor. */
static int pointer_drawn_x;
static int pointer_drawn_y;
static int pointer_drawn_valid;
static int keyevent_fd = -1;
static unsigned int tick;
static unsigned int last_render_tick;
static uint32_t last_app_tick_ms;
static volatile int desktop_dirty = 1;
static volatile uint32_t app_frame_dirty_mask;

static int find_caret_window(void);
static struct rect get_caret_area(void);
static void draw_ime(void);
static int bind_scanout(void);

static uint32_t scaled_scanline[APP_SURFACE_MAX_W];
static struct rect compose_clip = {0, 0, MAX_SW, MAX_SH};
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

static int gray(int n) {
    if (n < 0) n = 0;
    if (n > 14) n = 14;
    return 25 + n;
}

/* Rounded-corner inset tables: outer ring radius ~4px, inner (1px
 * inset) follows the same arc so a fill_round pair makes a 1px ring. */
static const uint8_t corner_outer[4] = {3, 2, 1, 1};
static const uint8_t corner_inner[4] = {2, 1, 1, 0};

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

/* Damage a window including its drop shadow. */
static void win_damage(int id) {
    if (id < 0 || id >= WIN_COUNT)
        return;
    struct rect r = windows[id].r;
    queue_damage((struct rect){r.x, r.y, r.w + 6, r.h + 6});
}

static void dock_geometry(int *x, int *y, int *dock_w, int *task_cap);

/* Damage the dock plus the tooltip/expanded-panel area above it. */
static void dock_damage(void) {
    int x, y, dock_w, task_cap;
    dock_geometry(&x, &y, &dock_w, &task_cap);
    int top = y - 42;
    /* App reader threads can call this concurrently.  Use the maximum
     * possible panel height instead of shared cached geometry. */
    if (dock_expanded)
        top = y - (16 + MAX_GUI_APPS * 42) - 8;
    /* The dock recenters as apps open/close, so damage the complete bottom
     * band instead of only the new bounds.  This also clears old tooltips. */
    queue_damage((struct rect){0, top, sw, y + 68 - top});
}

/* Damage the IME badge (top bar) and the candidate panel area. */
static void ime_damage(void) {
    queue_damage((struct rect){0, 0, sw, TOPBAR_H});
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

static void topbar_damage(void) {
    queue_damage((struct rect){0, 0, sw, TOPBAR_H});
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

static void fill(struct rect r, int color) {
    uint32_t c = (uint32_t)color & 0x00FFFFFFu;
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
        fb[y * fb_stride + x] = (uint32_t)color & 0x00FFFFFFu;
}

static void pixel_clip(int x, int y, int color, struct rect clip) {
    if (inside(x, y, clip))
        pixel(x, y, color);
}

static void text_clip(int x, int y, const char *s, int fg, int bg, struct rect clip);

static void text(int x, int y, const char *s, int fg, int bg) {
    struct rect clip = {0, 0, sw, sh};
    text_clip(x, y, s, fg, bg, clip);
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

static void text_clip(int x, int y, const char *s, int fg, int bg, struct rect clip) {
    int line_origin = x;
    y -= PLT_FONT_Y_SHIFT;
    while (s && *s) {
        uint32_t cp = gui_utf8_next(&s);
        if (cp == '\n') {
            y += KFONT_HEIGHT;
            x = line_origin;
            continue;
        }
        if (cp == '\t') {
            x += gui_tab_advance(x - line_origin);
            continue;
        }
        if (cp == '\r')
            continue;
        if (x >= clip.x + clip.w)
            return;
        uint8_t bits[FONT_GLYPH_BYTES];
        const uint8_t *alpha = 0;
        int glyph_w = KFONT_WIDTH;
        if (cp >= KFONT_FIRST && cp < KFONT_FIRST + KFONT_COUNT) {
            alpha = &kfont_alpha[cp - KFONT_FIRST][0][0];
        } else if (cp >= 0x80u) {
            glyph_w = font_glyph(cp, bits, sizeof(bits));
            if (glyph_w <= 0) {
                cp = '?'; glyph_w = KFONT_WIDTH;
                alpha = &kfont_alpha[cp - KFONT_FIRST][0][0];
            }
        } else {
            cp = '?'; alpha = &kfont_alpha[cp - KFONT_FIRST][0][0];
        }
        if (x + glyph_w > clip.x && y + KFONT_HEIGHT > clip.y &&
            x < clip.x + clip.w && y < clip.y + clip.h) {
        for (int py = 0; py < KFONT_HEIGHT; py++) {
            for (int px = 0; px < glyph_w; px++) {
                int coverage = alpha ? alpha[py * KFONT_WIDTH + px] :
                    (((bits[py * FONT_GLYPH_STRIDE + px / 8] &
                       (uint8_t)(0x80u >> (px & 7))) != 0) ? 255 : 0);
                int tx = x + px;
                int ty = y + py;
                if (coverage >= 255) {
                    pixel_clip(tx, ty, fg, clip);
                } else if (coverage <= 0) {
                    if (bg >= 0)
                        pixel_clip(tx, ty, bg, clip);
                } else if (inside(tx, ty, clip) && tx >= 0 && ty >= 0 &&
                           tx < sw && ty < sh &&
                           inside(tx, ty, compose_clip)) {
                    int under = bg >= 0 ? bg : (int)fb[ty * fb_stride + tx];
                    pixel(tx, ty, plt_blend(fg, under, coverage));
                }
            }
        }
        }
        x += glyph_w;
    }
}

static void line_h(int x, int y, int w, int color) {
    fill((struct rect){x, y, w, 1}, color);
}

static void line_v(int x, int y, int h, int color) {
    fill((struct rect){x, y, 1, h}, color);
}

static void border(struct rect r, int hi, int lo) {
    line_h(r.x, r.y, r.w, hi);
    line_v(r.x, r.y, r.h, hi);
    line_h(r.x, r.y + r.h - 1, r.w, lo);
    line_v(r.x + r.w - 1, r.y, r.h, lo);
}

/* Fill a rect with rounded corners; insets picks the corner arc. */
static void fill_round_t(struct rect r, const uint8_t *insets, int color) {
    fill((struct rect){r.x + insets[0], r.y, r.w - 2 * insets[0], r.h},
         color);
    for (int i = 0; i < 4; i++) {
        int rw = r.w - 2 * insets[i];
        fill((struct rect){r.x + insets[i], r.y + i, rw, 1}, color);
        fill((struct rect){r.x + insets[i], r.y + r.h - 1 - i, rw, 1},
             color);
    }
    fill((struct rect){r.x, r.y + 4, r.w, r.h - 8}, color);
}

static void fill_round(struct rect r, int color) {
    fill_round_t(r, corner_outer, color);
}

/* Only the top corners are rounded (window title bars). */
static void fill_round_top(struct rect r, const uint8_t *insets, int color) {
    fill((struct rect){r.x, r.y + 4, r.w, r.h - 4}, color);
    for (int i = 0; i < 4; i++)
        fill((struct rect){r.x + insets[i], r.y + i, r.w - 2 * insets[i], 1},
             color);
}

/* Blend RGB fg over the current backbuffer contents. */
static void fill_blend(struct rect r, int fg, int alpha) {
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
            row[xx] = plt_blend((uint32_t)fg, row[xx], alpha);
    }
}

static void fill_circle(int cx, int cy, int rad, int color) {
    for (int dy = -rad; dy <= rad; dy++) {
        for (int dx = -rad; dx <= rad; dx++) {
            if (dx * dx + dy * dy <= rad * rad)
                pixel(cx + dx, cy + dy, color);
        }
    }
}

/* Soft drop shadow: a quiet near edge plus a wider bottom/right falloff.
 * The six-pixel extent still matches the move/resize damage margin. */
static void shadow(struct rect r) {
    fill_blend((struct rect){r.x + 5, r.y + r.h, r.w, 5}, 0, 44);
    fill_blend((struct rect){r.x + r.w, r.y + 5, 5, r.h}, 0, 44);
    fill_blend((struct rect){r.x + 2, r.y + r.h, r.w, 2}, 0, 76);
    fill_blend((struct rect){r.x + r.w, r.y + 2, 2, r.h}, 0, 76);
}

static void button_state(struct rect r, const char *label, int active,
                         int disabled) {
    int hovered = !disabled && inside(pointer_x, pointer_y, r);
    int pressed = hovered && (prev_buttons & 1);
    int bg = active ? THEME_ACCENT_DIM :
             hovered ? THEME_WIN_HOVER : THEME_WIN_CONTROL;
    int edge = active ? THEME_ACCENT :
               hovered ? THEME_ACCENT_DIM : THEME_WIN_BORDER_INACT;
    int fg = active ? THEME_TEXT_ON_LIGHT : THEME_TEXT;
    if (pressed)
        bg = active ? THEME_ACCENT_DIM : THEME_WIN_PRESSED;
    if (disabled) {
        bg = THEME_PANEL_RAISED;
        edge = THEME_DIVIDER;
        fg = THEME_TEXT_FAINT;
    }
    fill_round_t(r, corner_outer, edge);
    fill_round_t((struct rect){r.x + 1, r.y + 1, r.w - 2, r.h - 2},
                 corner_inner, bg);
    int label_w = gui_text_width(label);
    int label_x = label_w <= r.w - 8 ? r.x + (r.w - label_w) / 2 : r.x + 8;
    int label_y = r.y + (r.h - KFONT_HEIGHT) / 2 + PLT_FONT_Y_SHIFT +
                  (pressed ? 1 : 0);
    text_clip(label_x, label_y, label, fg, -1,
              (struct rect){r.x + 4, r.y + 2, r.w - 8, r.h - 4});
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

static int manifest_key_length(const char *line, int line_len,
                               const char *key) {
    int i = 0;
    while (key[i]) {
        if (i >= line_len || line[i] != key[i])
            return 0;
        i++;
    }
    return i < line_len && line[i] == '=' ? i : 0;
}

static int copy_manifest_value(char *dst, size_t cap, const char *line,
                               int line_len, const char *key) {
    int key_len = manifest_key_length(line, line_len, key);
    if (!key_len || cap == 0)
        return 0;
    int src = key_len + 1;
    size_t out = 0;
    while (src < line_len && out + 1 < cap)
        dst[out++] = line[src++];
    dst[out] = 0;
    return 1;
}

static void load_launcher_manifest(struct app_entry *app,
                                   const char *manifest_path) {
    char buf[384];
    int fd = open(manifest_path, O_RDONLY);
    if (fd < 0)
        return;
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = 0;
    int start = 0;
    for (int i = 0; i <= n; i++) {
        if (buf[i] != '\n' && buf[i] != 0)
            continue;
        int len = i - start;
        if (len > 0 && buf[start + len - 1] == '\r')
            len--;
        if (len > 0) {
            if (!copy_manifest_value(app->name, sizeof(app->name),
                                     buf + start, len, "name"))
                (void)copy_manifest_value(app->summary, sizeof(app->summary),
                                          buf + start, len, "summary");
        }
        start = i + 1;
    }
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
            struct app_entry *app = &apps[app_count];
            copy_text(app->file, ents[i].d_name, sizeof(app->file));
            copy_text(app->name, ents[i].d_name, sizeof(app->name));
            copy_text(app->summary, "GUI application", sizeof(app->summary));
            copy_text(app->path, app_path, sizeof(app->path));
            app->size = ents[i].d_size;
            load_launcher_manifest(app, manifest_path);
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
            continue;
        }
        if (frame.type == GUIAPP_FRAME_EXEC) {
            if (exec_target_allowed(frame.target))
                run_app_with_arg("/fs/apps/terminal", frame.target);
            else
                gui_log("[gui] exec request rejected");
            continue;
        }
        if (frame.type != GUIAPP_FRAME_LAUNCH)
            break;
        if (app_target_allowed(frame.target))
            run_app_with_arg(frame.target, frame.argument[0] ? frame.argument : 0);
        else
            gui_log("[gui] launch request rejected");
    }
    if (frame.width <= 0 || frame.height <= 0 ||
        frame.width > APP_SURFACE_MAX_W || frame.height > APP_SURFACE_MAX_H)
        return -1;
    if ((frame.type != GUIAPP_FRAME_FULL &&
         frame.type != GUIAPP_FRAME_DIRTY &&
         frame.type != GUIAPP_FRAME_SCALED) ||
        !app_sessions[slot].shared)
        return -1;
    frame.title[GUIAPP_TITLE_MAX - 1] = 0;
    if ((frame.type == GUIAPP_FRAME_SCALED ||
         frame.type == GUIAPP_FRAME_DIRTY) &&
        (frame.dirty_w <= 0 || frame.dirty_h <= 0 ||
         frame.dirty_w > APP_SURFACE_MAX_W || frame.dirty_h > APP_SURFACE_MAX_H))
        return -1;
    int scaled = frame.type == GUIAPP_FRAME_SCALED;
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
        app_sessions[slot].source_w != source_w ||
        app_sessions[slot].source_h != source_h ||
        title_changed;
    app_sessions[slot].surface_w = frame.width;
    app_sessions[slot].surface_h = frame.height;
    app_sessions[slot].scaled_surface = scaled;
    app_sessions[slot].source_w = source_w;
    app_sessions[slot].source_h = source_h;
    app_sessions[slot].last_sequence = frame.sequence;
    /* App presented — allow the next live-resize configure. */
    app_sessions[slot].resize_inflight = 0;
    copy_text(app_sessions[slot].title, frame.title, sizeof(app_sessions[slot].title));
    windows[WIN_APP_BASE + slot].title = app_sessions[slot].title[0]
        ? app_sessions[slot].title : "Application";
    if (title_changed && focus == WIN_APP_BASE + slot)
        topbar_damage();
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
            dock_damage();
        if (resize_win == WIN_APP_BASE + slot)
            app_note_dirty(slot, (struct rect){0, 0, source_w, source_h});
        else
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
    }
    if (!app_sessions[slot].closing) {
        app_sessions[slot].reader_dead = 1;
        win_damage(WIN_APP_BASE + slot);
        dock_damage();
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
        topbar_damage();
        win_damage(old_focus);
        win_damage(id);
        dock_damage();
    }
    /* Focus can change without a pointer move (keyboard navigation, launch,
     * minimize/close).  Reconcile application hover state immediately so a
     * button under the stationary pointer gets the same feedback as one
     * reached by moving the pointer into it. */
    update_hover_app(1);
}

static void layout(void) {
    int margin = max_i(18, sw / 48);
    int top = TOPBAR_H;
    int dock = DOCK_RESERVED_H;
    int content_h = sh - top - dock - margin * 2;
    int left_w = min_i(max_i(360, sw / 3), 620);
    int right_w = min_i(max_i(400, sw / 3), 620);
    int launcher_h = min_i(content_h,
                           max_i(300, 134 + app_count * LAUNCHER_ROW_STEP));

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
    windows[WIN_STATUS].minimized = 1;

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

static void draw_background(void) {
    fill((struct rect){0, 0, sw, sh}, THEME_DESKTOP_BASE);
    /* A sparse grid gives the wallpaper depth without competing with
     * windows or relying on gradients unavailable in the 8-bit palette. */
    for (int x = 0; x < sw; x += 96)
        line_v(x, TOPBAR_H, sh - TOPBAR_H, THEME_DESKTOP_DEEP);
    for (int y = TOPBAR_H; y < sh; y += 96)
        line_h(0, y, sw, THEME_DESKTOP_DEEP);
}

static struct rect ime_badge_rect(void) {
    return (struct rect){sw - 54, 4, 42, 28};
}

static void draw_topbar(void) {
    fill((struct rect){0, 0, sw, TOPBAR_H}, THEME_TOPBAR);
    fill((struct rect){0, TOPBAR_H - 1, sw, 1}, THEME_TOPBAR_BORDER);
    int text_y = (TOPBAR_H - KFONT_HEIGHT) / 2 + PLT_FONT_Y_SHIFT;
    fill_round((struct rect){14, 12, 10, 10}, THEME_ACCENT_DIM);
    fill((struct rect){17, 15, 4, 4}, THEME_ACCENT);
    text(32, text_y, "BuzzOS", THEME_ACCENT, -1);
    const char *title = (focus >= 0 && focus < WIN_COUNT &&
                         windows[focus].visible &&
                         !windows[focus].minimized && windows[focus].title)
        ? windows[focus].title : "Desktop";
    struct rect badge = ime_badge_rect();
    line_v(124, 9, TOPBAR_H - 18, THEME_DIVIDER);
    text_clip(140, text_y, title, THEME_TEXT, -1,
              (struct rect){140, 3, max_i(1, badge.x - 152),
                            TOPBAR_H - 6});
}

static struct rect close_rect(int id);
static struct rect max_rect(int id);
static struct rect min_rect(int id);
static struct rect control_hit_rect(int id, int control);
static int control_hovered(int id, int control);
static int top_window_at(int x, int y);

static void draw_window_frame(int id) {
    struct window *w = &windows[id];
    if (!w->visible || w->minimized)
        return;
    struct rect r = w->r;
    shadow(r);
    fill_round_t(r, corner_outer,
                 w->active ? THEME_WIN_HOVER : THEME_WIN_CONTROL);
    fill_round_t((struct rect){r.x + 1, r.y + 1, r.w - 2, r.h - 2},
                 corner_inner, THEME_WIN_BODY);
    fill_round_top((struct rect){r.x + 1, r.y + 1, r.w - 2,
                                 WINDOW_TITLE_H - 1},
                   corner_inner,
                   w->active ? THEME_TITLE_ACT : THEME_TITLE_INACT);
    line_h(r.x + 1, r.y + WINDOW_TITLE_H - 1, r.w - 2,
           THEME_WIN_BORDER_INACT);
    if (w->active)
        line_h(r.x + 1, r.y + WINDOW_TITLE_H - 1,
               min_i(112, r.w - 2), THEME_ACCENT_DIM);
    int title_y = r.y + (WINDOW_TITLE_H - KFONT_HEIGHT) / 2 +
                  PLT_FONT_Y_SHIFT;
    text_clip(r.x + 12, title_y, w->title,
              w->active ? THEME_TEXT : THEME_TEXT_FAINT, -1,
              (struct rect){r.x + 10, r.y + 3, r.w - 88,
                            WINDOW_TITLE_H - 6});
    struct rect mn = min_rect(id);
    struct rect mx = max_rect(id);
    struct rect cl = close_rect(id);
    int mn_hover = control_hovered(id, 0);
    int mx_hover = control_hovered(id, 1);
    int cl_hover = control_hovered(id, 2);
    int neutral = w->active ? THEME_WIN_CONTROL : THEME_TITLE_INACT;
    int mn_bg = mn_hover ? THEME_WIN_HOVER : neutral;
    int mx_bg = mx_hover ? THEME_WIN_HOVER : neutral;
    int cl_bg = cl_hover ? THEME_DANGER_DIM : neutral;
    if (mn_hover && (prev_buttons & 1)) mn_bg = THEME_WIN_PRESSED;
    if (mx_hover && (prev_buttons & 1)) mx_bg = THEME_WIN_PRESSED;
    if (cl_hover && (prev_buttons & 1)) cl_bg = THEME_DANGER;
    fill_circle(mn.x + 6, mn.y + 6, 5, mn_bg);
    fill_circle(mx.x + 6, mx.y + 6, 5, mx_bg);
    fill_circle(cl.x + 6, cl.y + 6, 5, cl_bg);
    int mn_glyph = mn_hover ? THEME_TEXT : THEME_TEXT_FAINT;
    int mx_glyph = mx_hover ? THEME_TEXT : THEME_TEXT_FAINT;
    int cl_glyph = cl_hover ? THEME_TEXT : THEME_TEXT_FAINT;
    line_h(mn.x + 4, mn.y + 6, 5, mn_glyph);
    border((struct rect){mx.x + 4, mx.y + 4, 5, 5},
           mx_glyph, mx_glyph);
    for (int i = 0; i < 5; i++) {
        pixel(cl.x + 4 + i, cl.y + 4 + i, cl_glyph);
        pixel(cl.x + 8 - i, cl.y + 4 + i, cl_glyph);
    }
    if (!w->maximized) {
        for (int i = 0; i < 3; i++) {
            line_h(r.x + r.w - 18 + i * 5, r.y + r.h - 6 - i * 5,
                   12 - i * 4, gray(4));
        }
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

static struct rect close_rect(int id) {
    struct rect r = windows[id].r;
    return (struct rect){r.x + r.w - 27,
                         r.y + (WINDOW_TITLE_H - 12) / 2, 12, 12};
}

static struct rect max_rect(int id) {
    struct rect r = windows[id].r;
    return (struct rect){r.x + r.w - 47,
                         r.y + (WINDOW_TITLE_H - 12) / 2, 12, 12};
}

static struct rect min_rect(int id) {
    struct rect r = windows[id].r;
    return (struct rect){r.x + r.w - 67,
                         r.y + (WINDOW_TITLE_H - 12) / 2, 12, 12};
}

static struct rect control_hit_rect(int id, int control) {
    struct rect visual = control == 2 ? close_rect(id) :
                         control == 1 ? max_rect(id) : min_rect(id);
    return (struct rect){visual.x - 4, visual.y - 5,
                         visual.w + 8, visual.h + 10};
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
        /* Keep primary labels revealable in narrow windows.  Secondary
         * summaries clip instead of forcing a scrollbar at normal widths. */
        for (int i = 0; i < app_count; i++)
            width = max_i(width, 86 + gui_text_width(apps[i].name));
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
    int max_h = max_i(WIN_MIN_H, dock_y - TOPBAR_H - 10);
    if (r.w > max_w) r.w = max_w;
    if (r.h > max_h) r.h = max_h;
    if (r.x < 8) r.x = 8;
    if (r.y < TOPBAR_H + 4) r.y = TOPBAR_H + 4;
    if (r.x + r.w > sw - 8) r.x = sw - 8 - r.w;
    if (r.y + r.h > dock_y - 6) r.y = dock_y - 6 - r.h;
    return r;
}

static void relayout_after_mode_change(int old_sw, int old_sh) {
    int margin = max_i(18, sw / 48);
    int content_h = sh - TOPBAR_H - DOCK_RESERVED_H - margin * 2;
    int left_w = min_i(max_i(360, sw / 3), 620);
    int right_w = min_i(max_i(400, sw / 3), 620);
    int launcher_h = min_i(content_h,
                           max_i(300, 134 + app_count * LAUNCHER_ROW_STEP));
    struct rect launcher = {margin, TOPBAR_H + margin, left_w, launcher_h};
    struct rect status = {
        sw - right_w - margin, TOPBAR_H + margin, right_w,
        min_i(content_h, max_i(460, (content_h * 2) / 3))
    };
    int dock_x, dock_y, dock_w, task_cap;
    dock_geometry(&dock_x, &dock_y, &dock_w, &task_cap);
    (void)dock_x; (void)dock_w; (void)task_cap;
    struct rect work = {8, TOPBAR_H + 4, sw - 16,
                        dock_y - TOPBAR_H - 10};

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
    dock_expanded = 0;
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
    if (gfx_set_mode(display_modes[index].width,
                     display_modes[index].height) < 0) {
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
    (void)bind_scanout();
    relayout_after_mode_change(old_sw, old_sh);
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

static void draw_scrollbars(int id) {
    clamp_scroll(id);
    struct rect vt = vscroll_track(id);
    struct rect ht = hscroll_track(id);
    /* Tracks live just outside the content clip.  Clear them on every
     * redraw so a window that shrinks from scrollable to non-scrollable does
     * not retain the previous track/thumb pixels. */
    fill(vt, THEME_WIN_BODY);
    fill(ht, THEME_WIN_BODY);
    if (max_scroll_y(id) > 0) {
        struct rect vth = vscroll_thumb(id);
        fill_round((struct rect){vth.x + 1, vth.y + 1,
                                 vth.w - 2, vth.h - 2}, THEME_WIN_HOVER);
    }
    if (max_scroll_x(id) > 0) {
        struct rect hth = hscroll_thumb(id);
        fill_round((struct rect){hth.x + 1, hth.y + 1,
                                 hth.w - 2, hth.h - 2}, THEME_WIN_HOVER);
    }
}

static int launcher_icon_color(const char *name) {
    uint32_t hash = 2166136261u;
    while (name && *name) {
        hash ^= (uint8_t)*name++;
        hash *= 16777619u;
    }
    switch (hash % 6u) {
    case 0: return THEME_WIN_BORDER_ACT;
    case 1: return THEME_ACCENT_DIM;
    case 2: return 21;
    case 3: return THEME_MAX_GREEN;
    case 4: return 23;
    default: return THEME_DANGER_DIM;
    }
}

static void draw_launcher(void) {
    if (!windows[WIN_LAUNCHER].visible || windows[WIN_LAUNCHER].minimized)
        return;
    draw_window_frame(WIN_LAUNCHER);
    struct rect c = content_rect(WIN_LAUNCHER);
    fill(c, THEME_WIN_BODY);
    /* Same content clip as System: row fills must not paint past the body. */
    struct rect saved_clip = compose_clip_push(c);
    struct rect clip = c;
    int ox = c.x - scroll_x[WIN_LAUNCHER];
    int oy = c.y - scroll_y[WIN_LAUNCHER];
    text_clip(ox, oy + 2, "All apps", THEME_ACCENT, -1, clip);
    char count_label[24];
    count_label[0] = 0;
    append_uint(count_label, (unsigned int)app_count, sizeof(count_label));
    append_text(count_label, app_count == 1 ? " app" : " apps",
                sizeof(count_label));
    int badge_w = gui_text_width(count_label) + 18;
    struct rect badge = {c.x + c.w - badge_w - 4, oy - 1,
                         badge_w, 32};
    if (ox + gui_text_width("All apps") + 12 <= badge.x) {
        fill_round(badge, THEME_DIVIDER);
        fill_round((struct rect){badge.x + 1, badge.y + 1,
                                 badge.w - 2, badge.h - 2},
                   THEME_PANEL_RAISED);
        text_clip(badge.x + 9, badge.y + 2 + PLT_FONT_Y_SHIFT,
                  count_label, THEME_TEXT_DIM, -1, clip);
    }
    int y = oy + LAUNCHER_HEADER_H;
    if (app_count == 0) {
        text_clip(ox, y, "No apps in /fs/apps", THEME_TEXT_DIM, -1, clip);
        compose_clip_pop(saved_clip);
        draw_scrollbars(WIN_LAUNCHER);
        return;
    }
    int hover_row = hit_launcher_row_at(pointer_x, pointer_y);
    for (int i = 0; i < app_count; i++) {
        struct rect row = launcher_row_paint_rect(i);
        struct rect visible = intersect_rect(row, clip);
        if (visible.w <= 0 || visible.h <= 0)
            continue;
        int selected = i == app_selected;
        int hovered = i == hover_row;
        if (selected) {
            struct rect sel = intersect_rect(
                (struct rect){row.x, row.y, row.w - 6, row.h}, clip);
            if (sel.w > 0 && sel.h > 0) {
                fill_round(sel, THEME_SELECTION_SOFT);
                fill((struct rect){row.x, row.y + 8, 3, row.h - 16},
                     THEME_ACCENT);
            }
        } else if (hovered) {
            struct rect hover = intersect_rect(
                (struct rect){row.x, row.y, row.w - 6, row.h}, clip);
            if (hover.w > 0 && hover.h > 0)
                fill_round(hover, THEME_PANEL_RAISED);
        }
        struct rect icon = {row.x + 10, row.y + 13, 32, 32};
        fill_round(icon, selected ? THEME_ACCENT : THEME_FIELD_BORDER);
        fill_round((struct rect){icon.x + 2, icon.y + 2,
                                 icon.w - 4, icon.h - 4},
                   launcher_icon_color(apps[i].path));
        char monogram[2] = {apps[i].name[0] ? apps[i].name[0] : '?', 0};
        int mono_w = gui_text_width(monogram);
        text_clip(icon.x + (icon.w - mono_w) / 2,
                  icon.y + (icon.h - KFONT_HEIGHT) / 2 + PLT_FONT_Y_SHIFT,
                  monogram, THEME_TEXT, -1, clip);
        int tx = row.x + 54;
        text_clip(tx, row.y + 4 + PLT_FONT_Y_SHIFT, apps[i].name,
                  selected || hovered ? THEME_TEXT : THEME_TEXT_DIM, -1,
                  (struct rect){tx, row.y, max_i(1, row.w - 66), row.h});
        text_clip(tx, row.y + 30 + PLT_FONT_Y_SHIFT, apps[i].summary,
                  selected ? THEME_TEXT_DIM : THEME_TEXT_FAINT, -1,
                  (struct rect){tx, row.y, max_i(1, row.w - 66), row.h});
    }
    compose_clip_pop(saved_clip);
    draw_scrollbars(WIN_LAUNCHER);
}

static void draw_status_shortcut(int x, int y, int available_w,
                                 const char *key, const char *label,
                                 struct rect clip) {
    int key_w = min_i(available_w, max_i(60, gui_text_width(key) + 18));
    int label_w = gui_text_width(label);
    int show_label = key_w + 12 + label_w <= available_w;
    struct rect cap = {x, y, key_w, 28};
    fill_round(cap, THEME_DIVIDER);
    fill_round((struct rect){cap.x + 1, cap.y + 1,
                             cap.w - 2, cap.h - 2},
               THEME_WIN_CONTROL);
    int key_x = cap.x + max_i(6, (cap.w - gui_text_width(key)) / 2);
    text_clip(key_x, cap.y + PLT_FONT_Y_SHIFT, key, THEME_TEXT, -1,
              intersect_rect(cap, clip));
    if (show_label)
        text_clip(cap.x + cap.w + 12, cap.y + PLT_FONT_Y_SHIFT, label,
                  THEME_TEXT_DIM, -1,
                  (struct rect){cap.x + cap.w + 12, cap.y,
                                available_w - cap.w - 12, cap.h});
}

static void draw_status(void) {
    if (!windows[WIN_STATUS].visible || windows[WIN_STATUS].minimized)
        return;
    draw_window_frame(WIN_STATUS);
    struct rect c = content_rect(WIN_STATUS);
    fill(c, THEME_WIN_BODY);
    /* Restrict paint to the scrollable body.  button()/fill_round only honor
     * compose_clip (not the text clip rect), so without this push the
     * resolution grid paints through the frame onto the desktop whenever
     * content_height exceeds the window. */
    struct rect saved_clip = compose_clip_push(c);
    struct rect clip = c;
    int ox = c.x - scroll_x[WIN_STATUS];
    int oy = c.y - scroll_y[WIN_STATUS];
    char line[96];
    text_clip(ox, oy, "System overview", THEME_ACCENT, -1, clip);
    struct rect display_card = {ox, oy + 34, c.w - 4, 88};
    fill_round(display_card, THEME_DIVIDER);
    fill_round((struct rect){display_card.x + 1, display_card.y + 1,
                             display_card.w - 2, display_card.h - 2},
               THEME_PANEL_RAISED);
    text_clip(display_card.x + 12, display_card.y + 4 + PLT_FONT_Y_SHIFT,
              "Display", THEME_TEXT_FAINT, -1, clip);
    char app_label[32];
    app_label[0] = 0;
    append_uint(app_label, (unsigned int)app_count, sizeof(app_label));
    append_text(app_label, app_count == 1 ? " app" : " apps",
                sizeof(app_label));
    int app_label_w = gui_text_width(app_label);
    int app_label_x = display_card.x + display_card.w - app_label_w - 12;
    if (display_card.x + 12 + gui_text_width("Display") + 12 <= app_label_x)
        text_clip(app_label_x, display_card.y + 4 + PLT_FONT_Y_SHIFT,
                  app_label, THEME_TEXT_DIM, -1, clip);
    copy_text(line, "resolution ", sizeof(line));
    append_uint(line, (unsigned int)sw, sizeof(line));
    append_text(line, " x ", sizeof(line));
    append_uint(line, (unsigned int)sh, sizeof(line));
    append_text(line, "  (", sizeof(line));
    append_aspect(line, sizeof(line), sw, sh);
    append_text(line, ")", sizeof(line));
    text_clip(display_card.x + 12,
              display_card.y + 30 + PLT_FONT_Y_SHIFT,
              line, THEME_TEXT, -1, clip);
    text_clip(display_card.x + 12,
              display_card.y + 56 + PLT_FONT_Y_SHIFT,
              display_backend == GFX_BACKEND_VIRTIO_GPU_2D
                  ? "VirtIO GPU 2D / 32bpp"
                  : "Bochs VBE / 32bpp",
              THEME_TEXT_FAINT, -1, clip);
    text_clip(ox, oy + 138, "Keyboard", THEME_ACCENT, -1, clip);
    draw_status_shortcut(ox, oy + 166, c.w - 4,
                         "Ctrl+Space", "Toggle IME", clip);
    draw_status_shortcut(ox, oy + 196, c.w - 4,
                         "[ / ]", "Display mode", clip);
    draw_status_shortcut(ox, oy + 226, c.w - 4,
                         "Esc", "Return to shell", clip);
    int mode_error = mode_error_until && tick < mode_error_until;
    text_clip(ox, oy + STATUS_RES_HEAD_Y,
              mode_error ? "Resolution unavailable" : "Resolution by aspect",
              mode_error ? THEME_DANGER : THEME_ACCENT, -1, clip);
    int active_mode = current_display_mode();
    int group = 0;
    int start = 0;
    int count = 0;
    while (display_group_at(group, &start, &count)) {
        struct rect label_r = status_group_label_rect(group);
        text_clip(label_r.x, label_r.y + 2, display_modes[start].ratio,
                  THEME_TEXT_FAINT, -1, clip);
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

static void dock_geometry(int *x, int *y, int *dock_w, int *task_cap) {
    int ids[MAX_GUI_APPS];
    int app_total = collect_open_apps(ids);
    int system_count = WIN_APP_BASE;
    int base_w = 18 + system_count * DOCK_SYSTEM_STEP;
    int overflow_base = 24 + system_count * DOCK_SYSTEM_STEP + DOCK_MORE_W;
    int max_w = min_i(1180, sw - 40);
    if (max_w < overflow_base)
        max_w = sw - 12;
    int full_w = base_w + app_total * DOCK_TASK_STEP;
    if (full_w <= max_w) {
        *task_cap = app_total;
        *dock_w = full_w;
    } else {
        *task_cap = max_i(0, (max_w - overflow_base) / DOCK_TASK_STEP);
        *dock_w = overflow_base + *task_cap * DOCK_TASK_STEP;
        if (*dock_w > max_w)
            *dock_w = max_w;
    }
    *x = (sw - *dock_w) / 2;
    *y = sh - 74;
}

static void draw_dock_tooltip(void) {
    if (!dock_tooltip_visible || dock_hover < 0 ||
        dock_hover > WIN_COUNT)
        return;
    const char *title;
    if (dock_hover == WIN_COUNT) {
        title = dock_expanded ? "Hide applications" : "More applications";
    } else {
        if (!windows[dock_hover].visible)
            return;
        title = windows[dock_hover].title;
    }
    int tw = min_i(sw - 16, gui_text_width(title) + 24);
    int tx = clamp_i(pointer_x - tw / 2, 8, sw - tw - 8);
    int ty = sh - 116;
    struct rect tip = {tx, ty, tw, 34};
    fill_round(tip, THEME_WIN_CONTROL);
    fill_round((struct rect){tip.x + 1, tip.y + 1, tip.w - 2, tip.h - 2},
               THEME_WIN_BODY);
    int text_y = tip.y + (tip.h - KFONT_HEIGHT) / 2 + PLT_FONT_Y_SHIFT;
    text_clip(tip.x + 12, text_y, title, THEME_TEXT, -1,
              (struct rect){tip.x + 5, tip.y + 2, tip.w - 10, tip.h - 4});
}

static void draw_dock_tile(struct rect b, int id, const char *label,
                           int icon_color) {
    int active = id >= 0 && id < WIN_COUNT && windows[id].active &&
                 !windows[id].minimized;
    int hovered = dock_hover == id;
    int pressed = hovered && (prev_buttons & 1);
    if (active)
        fill_round(b, pressed ? THEME_WIN_PRESSED : THEME_SELECTION_SOFT);
    else if (hovered)
        fill_round(b, pressed ? THEME_WIN_PRESSED : THEME_WIN_HOVER);

    struct rect icon = {b.x + (b.w - 30) / 2, b.y + 4, 30, 30};
    fill_round(icon, active ? THEME_ACCENT : THEME_FIELD_BORDER);
    fill_round((struct rect){icon.x + 2, icon.y + 2, icon.w - 4, icon.h - 4},
               icon_color);
    char monogram[2] = {
        label && label[0] ? label[0] : '?',
        0
    };
    int mono_w = gui_text_width(monogram);
    text_clip(icon.x + (icon.w - mono_w) / 2,
              icon.y + (icon.h - KFONT_HEIGHT) / 2 + PLT_FONT_Y_SHIFT,
              monogram, THEME_TEXT, -1, icon);

    if (active)
        fill_round((struct rect){b.x + 11, b.y + b.h - 3,
                                 b.w - 22, 3}, THEME_ACCENT);
    else if (id >= WIN_APP_BASE && id < WIN_COUNT)
        fill_circle(b.x + b.w / 2, b.y + b.h - 2, 2, THEME_TEXT_FAINT);
}

static void draw_dock(void) {
    int ids[MAX_GUI_APPS];
    int app_total = collect_open_apps(ids);
    int x, y, dock_w, task_cap;
    dock_geometry(&x, &y, &dock_w, &task_cap);
    struct rect r = {x, y, dock_w, 60};
    /* Lift the dock off the wallpaper with the same surface hierarchy as
     * application windows. */
    fill_blend((struct rect){r.x + 4, r.y + r.h, r.w - 4, 5}, 0, 70);
    fill_blend((struct rect){r.x + r.w, r.y + 4, 5, r.h - 4}, 0, 70);
    fill_round_t(r, corner_outer, THEME_FIELD_BORDER);
    fill_round_t((struct rect){r.x + 1, r.y + 1, r.w - 2, r.h - 2},
                 corner_inner, THEME_PANEL_RAISED);
    line_h(r.x + 5, r.y + 1, r.w - 10, THEME_WIN_HOVER);

    for (int i = 0; i < WIN_APP_BASE; i++) {
        struct rect b = {x + 12 + i * DOCK_SYSTEM_STEP, y + 10,
                         DOCK_SYSTEM_W, 40};
        int icon_color = i == WIN_LAUNCHER
            ? THEME_WIN_BORDER_ACT : THEME_MAX_GREEN;
        draw_dock_tile(b, i, windows[i].title, icon_color);
    }

    int shown = min_i(app_total, task_cap);
    int task_x = x + 12 + WIN_APP_BASE * DOCK_SYSTEM_STEP;
    if (app_total > 0)
        line_v(task_x - 6, y + 12, 36, THEME_DIVIDER);
    for (int i = 0; i < shown; i++) {
        int id = ids[i];
        struct rect b = {task_x + i * DOCK_TASK_STEP, y + 10,
                         DOCK_TASK_W, 40};
        draw_dock_tile(b, id, windows[id].title,
                       launcher_icon_color(windows[id].title));
    }
    int hidden = app_total - shown;
    if (hidden <= 0)
        dock_expanded = 0;
    if (hidden > 0) {
        int more_x = task_x + shown * DOCK_TASK_STEP;
        struct rect more = {more_x, y + 10, DOCK_MORE_W, 40};
        int hovered = dock_hover == WIN_COUNT;
        if (dock_expanded)
            fill_round(more, THEME_SELECTION_SOFT);
        else if (hovered)
            fill_round(more, (prev_buttons & 1)
                       ? THEME_WIN_PRESSED : THEME_WIN_HOVER);
        struct rect more_icon = {more.x + 8, more.y + 4, 30, 30};
        fill_round(more_icon, THEME_FIELD_BORDER);
        fill_round((struct rect){more_icon.x + 2, more_icon.y + 2,
                                 more_icon.w - 4, more_icon.h - 4},
                   THEME_WIN_CONTROL);
        const char *mark = dock_expanded ? "-" : "+";
        int mark_w = gui_text_width(mark);
        text_clip(more_icon.x + (more_icon.w - mark_w) / 2,
                  more_icon.y + (more_icon.h - KFONT_HEIGHT) / 2 +
                      PLT_FONT_Y_SHIFT,
                  mark, THEME_TEXT, -1, more_icon);
    }

    if (dock_expanded && hidden > 0) {
        int panel_w = min_i(380, sw - 24);
        int panel_h = 14 + hidden * 42;
        int panel_x = clamp_i(x + dock_w - panel_w, 12,
                              max_i(12, sw - panel_w - 12));
        int panel_y = y - panel_h - 6;
        struct rect panel = {panel_x, panel_y, panel_w, panel_h};
        fill_blend((struct rect){panel.x + 4, panel.y + panel.h,
                                 panel.w - 4, 4}, 0, 70);
        fill_round(panel, THEME_FIELD_BORDER);
        fill_round((struct rect){panel.x + 1, panel.y + 1,
                                 panel.w - 2, panel.h - 2}, THEME_PANEL_RAISED);
        for (int i = 0; i < hidden; i++) {
            int id = ids[shown + i];
            struct rect item = {panel_x + 7, panel_y + 7 + i * 42,
                                panel_w - 14, 38};
            button(item, windows[id].title, windows[id].active);
        }
    }
    draw_dock_tooltip();
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

static void draw_ime(void) {
    const char *mode = ime_enabled ? "中" : "英";
    struct rect badge = ime_badge_rect();
    fill_round(badge, ime_enabled ? THEME_ACCENT_DIM : THEME_WIN_CONTROL);
    int mode_w = gui_text_width(mode);
    int mode_y = badge.y + (badge.h - KFONT_HEIGHT) / 2 + PLT_FONT_Y_SHIFT;
    text_clip(badge.x + (badge.w - mode_w) / 2, mode_y, mode,
              THEME_TEXT, -1,
              (struct rect){badge.x + 2, badge.y + 1,
                            badge.w - 4, badge.h - 2});
    if (!ime_enabled || ime_length == 0)
        return;

    char comp[96];
    copy_text(comp, ime_buffer, sizeof(comp));
    if (ime_cand_count > IME_PAGE_SIZE) {
        append_text(comp, "  (", sizeof(comp));
        append_uint(comp, (unsigned int)(ime_page + 1), sizeof(comp));
        append_text(comp, "/", sizeof(comp));
        append_uint(comp, (unsigned int)ime_page_count(), sizeof(comp));
        append_text(comp, ")", sizeof(comp));
    }

    char cands[192];
    cands[0] = 0;
    for (int i = 0; i < IME_PAGE_SIZE; i++) {
        char item[GUIAPP_TEXT_MAX];
        if (!ime_candidate_at(i, item))
            break;
        char number[4] = {(char)('1' + i), '.', 0, 0};
        if (i)
            append_text(cands, "  ", sizeof(cands));
        append_text(cands, number, sizeof(cands));
        append_text(cands, item, sizeof(cands));
    }
    if (!cands[0])
        copy_text(cands, "(no match — Space commits pinyin)", sizeof(cands));

    int need_w = max_i(gui_text_width(comp), gui_text_width(cands)) + 28;
    int panel_w = min_i(sw - 24, max_i(320, need_w));
    int panel_h = 64;

    /* OS-style: float next to the text caret of the focused app. */
    struct rect caret = get_caret_area();
    int panel_x = caret.x;
    int panel_y = caret.y + caret.h + 6;
    if (panel_x + panel_w > sw - 8)
        panel_x = sw - 8 - panel_w;
    if (panel_x < 8)
        panel_x = 8;
    if (panel_y + panel_h > sh - 8)
        panel_y = caret.y - panel_h - 6;
    if (panel_y < TOPBAR_H + 4)
        panel_y = TOPBAR_H + 4;
    if (panel_y + panel_h > sh - 8)
        panel_y = sh - 8 - panel_h;
    struct rect panel = {panel_x, panel_y, panel_w, panel_h};
    fill_round(panel, THEME_FIELD_BORDER);
    fill_round((struct rect){panel.x + 1, panel.y + 1,
                             panel.w - 2, panel.h - 2}, THEME_PANEL_RAISED);
    struct rect clip = {panel.x + 8, panel.y + 4, panel.w - 16, panel.h - 8};
    text_clip(panel.x + 12, panel.y + 8, comp, THEME_ACCENT, -1, clip);
    text_clip(panel.x + 12, panel.y + 34, cands, THEME_TEXT, -1, clip);
}

static void draw_context_menu(void) {
    if (!context_open) return;
    static const char *labels[] = {"Copy", "Paste", "Cut"};
    struct rect menu = {context_x, context_y,
                        CONTEXT_MENU_W, CONTEXT_MENU_H};
    if (menu.x + menu.w > sw) menu.x = sw - menu.w;
    if (menu.y + menu.h > sh) menu.y = sh - menu.h;
    context_x = menu.x; context_y = menu.y;
    fill_round(menu, THEME_WIN_CONTROL);
    fill_round((struct rect){menu.x + 1, menu.y + 1,
                             menu.w - 2, menu.h - 2}, THEME_WIN_BODY);
    for (int i = 0; i < 3; i++) {
        int disabled = i == 1 && !clipboard[0];
        struct rect item = {menu.x + 6,
                            menu.y + 6 + i * CONTEXT_ITEM_STEP,
                            menu.w - 12, CONTEXT_ITEM_H};
        int hovered = !disabled && inside(pointer_x, pointer_y, item);
        if (hovered)
            fill_round(item, THEME_WIN_HOVER);
        int ty = item.y + (item.h - KFONT_HEIGHT) / 2 + PLT_FONT_Y_SHIFT;
        int fg = disabled ? THEME_TEXT_FAINT :
                 hovered ? THEME_TEXT : THEME_TEXT_DIM;
        text_clip(item.x + 12, ty, labels[i], fg, -1,
                  (struct rect){item.x + 8, item.y + 2,
                                item.w - 16, item.h - 4});
    }
}

static void draw_pointer(void) {
    static const uint16_t arrow[16] = {
        0x8000,0xC000,0xE000,0xF000,0xF800,0xFC00,0xFE00,0xFF00,
        0xFF80,0xF800,0xDC00,0x8C00,0x0600,0x0600,0x0300,0x0300
    };
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
    draw_background();
    draw_topbar();
    for (int i = 0; i < WIN_COUNT; i++) {
        int id = z_order[i];
        if (id == WIN_LAUNCHER)
            draw_launcher();
        else if (id == WIN_STATUS)
            draw_status();
        else if (id >= WIN_APP_BASE)
            draw_app_window(id);
    }
    draw_dock();
    draw_ime();
    draw_context_menu();
    draw_pointer();
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

static int render_region(struct rect area) {
    area = intersect_rect(area, (struct rect){0, 0, sw, sh});
    if (area.w <= 0 || area.h <= 0)
        return 0;
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
    if (r.y < TOPBAR_H) {
        r.h += r.y - TOPBAR_H;
        r.y = TOPBAR_H;
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
        int dock_x, dock_y, dock_w, task_cap;
        dock_geometry(&dock_x, &dock_y, &dock_w, &task_cap);
        windows[id].restore = windows[id].r;
        /* Keep maximized content inside the desktop work area instead of
         * extending underneath the Deck. */
        windows[id].r = (struct rect){8, TOPBAR_H + 4, sw - 16,
                                     dock_y - TOPBAR_H - 10};
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

static int hit_dock(int x, int y) {
    int ids[MAX_GUI_APPS];
    int app_total = collect_open_apps(ids);
    int dx, dy, dock_w, task_cap;
    dock_geometry(&dx, &dy, &dock_w, &task_cap);
    int shown = min_i(app_total, task_cap);
    int hidden = app_total - shown;
    if (dock_expanded && hidden > 0) {
        int panel_w = min_i(380, sw - 24);
        int panel_h = 14 + hidden * 42;
        int panel_x = clamp_i(dx + dock_w - panel_w, 12,
                              max_i(12, sw - panel_w - 12));
        int panel_y = dy - panel_h - 6;
        for (int i = 0; i < hidden; i++) {
            struct rect item = {panel_x + 7, panel_y + 7 + i * 42,
                                panel_w - 14, 38};
            if (inside(x, y, item))
                return ids[shown + i];
        }
    }
    for (int i = 0; i < WIN_APP_BASE; i++) {
        if (inside(x, y,
                   (struct rect){dx + 12 + i * DOCK_SYSTEM_STEP, dy + 10,
                                 DOCK_SYSTEM_W, 40}))
            return i;
    }
    int task_x = dx + 12 + WIN_APP_BASE * DOCK_SYSTEM_STEP;
    for (int i = 0; i < shown; i++) {
        if (inside(x, y,
                   (struct rect){task_x + i * DOCK_TASK_STEP, dy + 10,
                                 DOCK_TASK_W, 40}))
            return ids[i];
    }
    if (hidden > 0 &&
        inside(x, y,
               (struct rect){task_x + shown * DOCK_TASK_STEP, dy + 10,
                             DOCK_MORE_W, 40}))
        return WIN_COUNT;
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
        if (app_selected != old_selected) {
            struct rect c = content_rect(WIN_LAUNCHER);
            int row_top = LAUNCHER_HEADER_H +
                          app_selected * LAUNCHER_ROW_STEP;
            int row_bottom = row_top + LAUNCHER_ROW_H;
            if (row_top < scroll_y[WIN_LAUNCHER])
                scroll_y[WIN_LAUNCHER] = row_top;
            else if (row_bottom > scroll_y[WIN_LAUNCHER] + c.h)
                scroll_y[WIN_LAUNCHER] = row_bottom - c.h;
            clamp_scroll(WIN_LAUNCHER);
            win_damage(WIN_LAUNCHER);
        }
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
    if (pointer_drawn_valid)
        damage = union_rect(damage, pointer_damage_rect(pointer_drawn_x,
                                                        pointer_drawn_y));
    damage = union_rect(damage, pointer_damage_rect(pointer_x, pointer_y));
    return damage;
}

static void note_pointer_drawn(void) {
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

/* Geometry shared by paint, hit-test, and damage.  Rows retain their virtual
 * width so horizontal scrolling can reveal long names; paint and damage are
 * clipped back to the visible content rect. */
static struct rect launcher_row_paint_rect(int index) {
    struct rect c = content_rect(WIN_LAUNCHER);
    int ox = c.x - scroll_x[WIN_LAUNCHER];
    int oy = c.y - scroll_y[WIN_LAUNCHER];
    int row_w = content_width(WIN_LAUNCHER) - 20;
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

    int row = hit_launcher_row_at(pointer_x, pointer_y);
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
    int old_dock_hover = dock_hover;
    int pointer_moved = ms.x != pointer_x || ms.y != pointer_y;
    if (ms.buttons != prev_buttons)
        desktop_dirty = 1;
    pointer_x = ms.x;
    pointer_y = ms.y;
    if (pointer_moved) {
        queue_damage(pointer_damage_rect(old_pointer_x, old_pointer_y));
        queue_damage(pointer_damage_rect(pointer_x, pointer_y));
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
    dock_hover = hit_dock(pointer_x, pointer_y);
    if (dock_hover != old_dock_hover) {
        int keep_tooltip = old_dock_hover >= 0 && dock_hover >= 0 &&
                           dock_tooltip_visible;
        dock_hover_since = tick;
        dock_tooltip_visible = keep_tooltip;
        dock_damage();
    } else if (dock_hover >= 0 && !dock_tooltip_visible &&
               tick - dock_hover_since >= DOCK_TOOLTIP_DELAY) {
        dock_tooltip_visible = 1;
        dock_damage();
    }

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
        int relative_y = pointer_y - context_y - 6;
        int item = relative_y >= 0 ? relative_y / CONTEXT_ITEM_STEP : -1;
        int in_row = item >= 0 && item < 3 &&
                     relative_y - item * CONTEXT_ITEM_STEP < CONTEXT_ITEM_H;
        if (pointer_x >= context_x + 6 &&
            pointer_x < context_x + CONTEXT_MENU_W - 6 && in_row) {
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
        int h = hit_window(pointer_x, pointer_y);
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
        if (dock_hover == WIN_COUNT) {
            dock_expanded = !dock_expanded;
            desktop_dirty = 1;
        } else if (dock_hover >= 0) {
            if (windows[dock_hover].active &&
                !windows[dock_hover].minimized)
                minimize_window(dock_hover);
            else
                activate(dock_hover);
            dock_expanded = 0;
        } else {
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
            if (focus == WIN_LAUNCHER) {
                int idx = hit_launcher_row_at(pointer_x, pointer_y);
                if (idx >= 0) {
                    if (idx == app_last_click &&
                        tick - app_last_click_tick <= 25u) {
                        app_selected = idx;
                        app_last_click = -1;
                        run_app(apps[idx].path);
                        prev_buttons = ms.buttons;
                        return;
                    }
                    app_selected = idx;
                    app_last_click = idx;
                    app_last_click_tick = tick;
                }
            } else {
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
        app_mouse_capture = -1;
        drag_win = -1;
        scroll_drag_win = -1;
        resize_win = -1;
        if (finished_resize >= WIN_APP_BASE)
            (void)sync_app_size(finished_resize, 1);
        update_hover_app(1);
    }
    if (left && resize_win >= 0) {
        struct rect old = windows[resize_win].r;
        apply_resize(resize_win, pointer_x, pointer_y);
        struct rect now = windows[resize_win].r;
        queue_damage(union_rect(
            (struct rect){old.x, old.y, old.w + 6, old.h + 6},
            (struct rect){now.x, now.y, now.w + 6, now.h + 6}));
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
        if (r->y < TOPBAR_H) r->y = TOPBAR_H;
        if (r->x + r->w > sw) r->x = sw - r->w;
        if (r->y + r->h > sh - 12) r->y = sh - 12 - r->h;
        queue_damage(union_rect(
            (struct rect){old.x, old.y, old.w + 6, old.h + 6},
            (struct rect){r->x, r->y, r->w + 6, r->h + 6}));
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
    scan_apps();
    keyevent_fd = open("/dev/keyevent", O_RDONLY);
    layout();
}

static void shutdown_desktop(void) {
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
    uint32_t frame_deadline = monotonic_ms();
    uint32_t frame_fraction = 0;
    while (running) {
        reap_dead_apps();
        int key;
        while ((key = read_key_poll()) >= 0)
            handle_key(key);
        forward_key_releases();
        handle_mouse();
        flush_pending_app_resizes();
        send_app_ticks();
        uint32_t app_dirty = __sync_lock_test_and_set(&app_frame_dirty_mask, 0);
        struct rect damage = {0, 0, 0, 0};
        int have_damage = take_damage(&damage);
        for (int slot = 0; slot < MAX_GUI_APPS; slot++) {
            if (!(app_dirty & (1u << slot)))
                continue;
            struct rect dirty;
            if (!app_take_dirty(slot, &dirty))
                continue;
            struct rect area = app_damage_to_screen(slot, dirty);
            if (area.w <= 0 || area.h <= 0)
                continue;
            damage = have_damage ? union_rect(damage, area) : area;
            have_damage = 1;
        }
        int full_dirty = __sync_lock_test_and_set(&desktop_dirty, 0);
        if (full_dirty || tick - last_render_tick >= 60u) {
            render();
            last_render_tick = tick;
            note_pointer_drawn();
        } else if (have_damage) {
            /* App-list hover dirties whole rows frequently.  Those rects can
             * omit the previous cursor splat if it sat just outside a row
             * gap; always re-erase the last composed pointer. */
            damage = damage_with_pointer(damage);
            (void)render_region(damage);
            note_pointer_drawn();
        }
        tick++;
        if (app_mouse_capture >= 0) {
            frame_deadline += 4u;
        } else {
            /* 60 Hz = 16 2/3 ms. Use an absolute 16,17,17 ms cadence so
             * composition time is part of the budget instead of being added
             * after every frame. */
            frame_deadline += 16u;
            frame_fraction += 2u;
            if (frame_fraction >= 3u) {
                frame_deadline++;
                frame_fraction -= 3u;
            }
        }
        uint32_t now = monotonic_ms();
        if ((int32_t)(frame_deadline - now) > 0) {
            sleep_ms(frame_deadline - now);
        } else {
            yield();
            if ((int32_t)(now - frame_deadline) > 100)
                frame_deadline = now;
        }
    }
    shutdown_desktop();
    release_display();
    return 0;
}
