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
    WIN_TERMINAL = 1,
    WIN_STATUS = 2,
    WIN_APP_BASE = 3,
    MAX_GUI_APPS = 10,
    WIN_COUNT = WIN_APP_BASE + MAX_GUI_APPS,

    MAX_APPS = 16,
    TERM_LINES = 256,
    TERM_COLS = 128,
    TERM_LINE_BYTES = TERM_COLS * 4,
    APP_DEFAULT_W = 560,
    APP_DEFAULT_H = 360,
    APP_SURFACE_MAX_W = GUIAPP_MAX_W,
    APP_SURFACE_MAX_H = GUIAPP_MAX_H,
    MAX_SW = 1280,
    MAX_SH = 800,
    WIN_MIN_W = 260,
    WIN_MIN_H = 170,
    RESIZE_PAD = 6,
};

struct rect {
    int x;
    int y;
    int w;
    int h;
};

struct window {
    const char *title;
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
    int resize_dirty;
    int reader_tid;
    volatile int reader_dead;
    volatile int closing;
    uint32_t shm_token;
    struct guiapp_shared_surface *shared;
    volatile int dirty_lock;
    int dirty_valid;
    struct rect dirty_rect;
    uint32_t last_sequence;
    uint16_t xmap[APP_SURFACE_MAX_W];
    uint16_t ymap[APP_SURFACE_MAX_H];
    char title[GUIAPP_TITLE_MAX];
};

static int sw;
static int sh;
static uint8_t fb[MAX_SW * MAX_SH];
static int running = 1;
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
static char term_lines[TERM_LINES][TERM_LINE_BYTES + 1];
static int term_row;
static int term_col;
static volatile int term_lock;
static int term_in_fd = -1;
static int term_out_fd = -1;
static int term_pid = -1;
static int term_reader_tid = -1;
static int keyevent_fd = -1;
static char term_input[512];
static int term_input_len;
static int term_select_anchor_row;
static int term_select_anchor_pos;
static int term_select_row;
static int term_select_pos;
static int term_selecting;
static int term_ansi_state;
static int term_ansi_param;
static unsigned int tick;
static unsigned int last_render_tick;
static volatile int desktop_dirty = 1;
static volatile uint32_t app_frame_dirty_mask;
static uint8_t scaled_scanline[APP_SURFACE_MAX_W];
static struct rect compose_clip = {0, 0, MAX_SW, MAX_SH};
static struct rect pending_damage;
static int pending_damage_valid;
static uint32_t last_wheel_seq;
static int last_wheel_value;
static int ime_enabled;
static char ime_buffer[24];
static int ime_length;
static char clipboard[GUIAPP_PATH_MAX];
static int context_open;
static int context_x;
static int context_y;
static int context_target = -1;

static int rgb6(int r, int g, int b) {
    if (r < 0) r = 0; if (r > 5) r = 5;
    if (g < 0) g = 0; if (g > 5) g = 5;
    if (b < 0) b = 0; if (b > 5) b = 5;
    return 40 + r * 36 + g * 6 + b;
}

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
    area = intersect_rect(area, (struct rect){0, 0, sw, sh});
    if (area.w <= 0 || area.h <= 0)
        return;
    pending_damage = pending_damage_valid
        ? union_rect(pending_damage, area) : area;
    pending_damage_valid = 1;
}

static int take_damage(struct rect *out) {
    if (!pending_damage_valid)
        return 0;
    *out = pending_damage;
    pending_damage_valid = 0;
    pending_damage = (struct rect){0, 0, 0, 0};
    return 1;
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
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > sw) r.w = sw - r.x;
    if (r.y + r.h > sh) r.h = sh - r.y;
    r = intersect_rect(r, compose_clip);
    if (r.w <= 0 || r.h <= 0)
        return;
    for (int yy = 0; yy < r.h; yy++) {
        uint8_t *row = fb + (r.y + yy) * sw + r.x;
        memset(row, color, (size_t)r.w);
    }
}

static void pixel(int x, int y, int color) {
    if (x >= 0 && y >= 0 && x < sw && y < sh &&
        inside(x, y, compose_clip))
        fb[y * sw + x] = (uint8_t)color;
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

static int gui_utf8_prev(const char *text, int pos) {
    if (!text || pos <= 0) return 0;
    pos--;
    while (pos > 0 && ((uint8_t)text[pos] & 0xC0u) == 0x80u) pos--;
    return pos;
}

static int gui_codepoint_width(uint32_t cp) {
    if (cp < 0x80u) return KFONT_WIDTH;
    uint8_t bits[FONT_GLYPH_BYTES];
    int width = font_glyph(cp, bits, sizeof(bits));
    return width > 0 ? width : KFONT_WIDTH;
}

static int gui_text_width(const char *s) {
    int width = 0;
    while (s && *s) {
        uint32_t cp = gui_utf8_next(&s);
        if (cp == '\n') break;
        width += gui_codepoint_width(cp);
    }
    return width;
}

static void text_clip(int x, int y, const char *s, int fg, int bg, struct rect clip) {
    y -= PLT_FONT_Y_SHIFT;
    while (s && *s) {
        uint32_t cp = gui_utf8_next(&s);
        if (cp == '\n') {
            y += KFONT_HEIGHT;
            x = clip.x;
            continue;
        }
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
                    int under = bg >= 0 ? bg : fb[ty * sw + tx];
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

/* Blend palette color fg over the current backbuffer contents. */
static void fill_blend(struct rect r, int fg, int alpha) {
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.x + r.w > sw) r.w = sw - r.x;
    if (r.y + r.h > sh) r.h = sh - r.y;
    r = intersect_rect(r, compose_clip);
    if (r.w <= 0 || r.h <= 0)
        return;
    for (int yy = 0; yy < r.h; yy++) {
        uint8_t *row = fb + (r.y + yy) * sw + r.x;
        for (int xx = 0; xx < r.w; xx++)
            row[xx] = (uint8_t)plt_blend(fg, row[xx], alpha);
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

/* Soft drop shadow: two blended black layers, 6px max extent (matches
 * the +6 damage margin used by window move/resize code). */
static void shadow(struct rect r) {
    fill_blend((struct rect){r.x + 6, r.y + r.h, r.w, 6}, 0, 55);
    fill_blend((struct rect){r.x + r.w, r.y + 6, 6, r.h}, 0, 55);
    fill_blend((struct rect){r.x + 3, r.y + r.h, r.w, 3}, 0, 120);
    fill_blend((struct rect){r.x + r.w, r.y + 3, 3, r.h}, 0, 120);
}

static void button(struct rect r, const char *label, int active) {
    int bg = active ? THEME_ACCENT_DIM : THEME_WIN_CONTROL;
    int edge = active ? THEME_ACCENT : THEME_WIN_BORDER_INACT;
    fill_round_t(r, corner_outer, edge);
    fill_round_t((struct rect){r.x + 1, r.y + 1, r.w - 2, r.h - 2},
                 corner_inner, bg);
    text_clip(r.x + 10, r.y + 5, label, THEME_TEXT, -1,
              (struct rect){r.x + 4, r.y + 2, r.w - 8, r.h - 4});
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
static void close_window(int id);
static void clamp_scroll(int id);
static void activate(int id);
static void run_app_with_arg(const char *path, const char *argument);
static void terminal_execute_path(const char *path);

static void term_lock_enter(void) {
    while (__sync_lock_test_and_set(&term_lock, 1))
        yield();
}

static void term_lock_leave(void) {
    __sync_lock_release(&term_lock);
}

static void term_scroll_to_bottom(void) {
    if (windows[WIN_TERMINAL].r.w > 0) {
        scroll_y[WIN_TERMINAL] = max_scroll_y(WIN_TERMINAL);
        scroll_x[WIN_TERMINAL] = 0;
    }
}

static void term_newline_locked(void) {
    term_col = 0;
    if (term_row + 1 < TERM_LINES) {
        term_row++;
    } else {
        for (int i = 1; i < TERM_LINES; i++)
            copy_text(term_lines[i - 1], term_lines[i], sizeof(term_lines[i - 1]));
        term_lines[TERM_LINES - 1][0] = 0;
    }
    term_scroll_to_bottom();
}

static void term_clear_line_from_cursor_locked(void) {
    for (int i = term_col; i < TERM_LINE_BYTES; i++)
        term_lines[term_row][i] = 0;
}

static void term_putc_locked(char ch) {
    if (term_ansi_state == 1) {
        if (ch == '[') {
            term_ansi_state = 2;
            term_ansi_param = 0;
        } else {
            term_ansi_state = 0;
        }
        return;
    }
    if (term_ansi_state == 2) {
        if (ch >= '0' && ch <= '9') {
            term_ansi_param = term_ansi_param * 10 + (ch - '0');
            return;
        }
        int n = term_ansi_param > 0 ? term_ansi_param : 1;
        if (ch == 'D') {
            term_col -= n;
            if (term_col < 0) term_col = 0;
        } else if (ch == 'C') {
            term_col += n;
            if (term_col >= TERM_LINE_BYTES) term_col = TERM_LINE_BYTES - 1;
        } else if (ch == 'K') {
            term_clear_line_from_cursor_locked();
        }
        term_ansi_state = 0;
        return;
    }
    if ((unsigned char)ch == 0x1B) {
        term_ansi_state = 1;
        return;
    }
    if (ch == '\r') {
        term_col = 0;
        return;
    }
    if (ch == '\n') {
        term_newline_locked();
        return;
    }
    if (ch == '\b' || ch == 127) {
        if (term_col > 0) {
            term_col--;
            term_lines[term_row][term_col] = 0;
        }
        return;
    }
    if ((unsigned char)ch < 32)
        return;
    if (term_col >= TERM_LINE_BYTES - 1)
        term_newline_locked();
    term_lines[term_row][term_col++] = ch;
    term_lines[term_row][term_col] = 0;
    term_scroll_to_bottom();
}

static void term_write_text(const char *s) {
    term_lock_enter();
    while (s && *s)
        term_putc_locked(*s++);
    term_lock_leave();
}

static void term_log(const char *s) {
    term_write_text(s);
    term_write_text("\n");
}

static void terminal_reader(void) {
    char buf[96];
    for (;;) {
        if (term_out_fd < 0)
            return;
        int n = read(term_out_fd, buf, sizeof(buf));
        if (n <= 0)
            return;
        term_lock_enter();
        for (int i = 0; i < n; i++)
            term_putc_locked(buf[i]);
        term_lock_leave();
        desktop_dirty = 1;
    }
}

static int start_terminal_shell(void) {
    int in_pipe[2];
    int out_pipe[2];
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0)
        return -1;

    int save0 = dup(0);
    int save1 = dup(1);
    int save2 = dup(2);
    if (save0 < 0 || save1 < 0 || save2 < 0)
        return -1;

    dup2(in_pipe[0], 0);
    dup2(out_pipe[1], 1);
    dup2(out_pipe[1], 2);
    char *argv[1];
    argv[0] = "/bin/sh";
    int pid = spawn_process_args("/bin/sh", argv, 1,
                                 SPAWN_FLAG_SILENT | SPAWN_FLAG_INHERIT_STDIO);
    dup2(save0, 0);
    dup2(save1, 1);
    dup2(save2, 2);
    close(save0);
    close(save1);
    close(save2);
    close(in_pipe[0]);
    close(out_pipe[1]);
    if (pid < 0) {
        close(in_pipe[1]);
        close(out_pipe[0]);
        return -1;
    }

    term_pid = pid;
    term_in_fd = in_pipe[1];
    term_out_fd = out_pipe[0];
    term_reader_tid = spawn(terminal_reader);
    if (term_reader_tid < 0)
        return -1;
    term_log("[desktop] attached /bin/sh");
    return 0;
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
            desktop_dirty = 1;
            continue;
        }
        if (frame.type == GUIAPP_FRAME_EXEC) {
            if (exec_target_allowed(frame.target))
                terminal_execute_path(frame.target);
            else
                term_log("exec request rejected");
            continue;
        }
        if (frame.type != GUIAPP_FRAME_LAUNCH)
            break;
        if (app_target_allowed(frame.target))
            run_app_with_arg(frame.target, frame.argument[0] ? frame.argument : 0);
        else
            term_log("launch request rejected");
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
    int full_change = app_sessions[slot].surface_w != frame.width ||
        app_sessions[slot].surface_h != frame.height ||
        app_sessions[slot].scaled_surface != scaled ||
        app_sessions[slot].source_w != source_w ||
        app_sessions[slot].source_h != source_h ||
        strcmp(app_sessions[slot].title, frame.title) != 0;
    app_sessions[slot].surface_w = frame.width;
    app_sessions[slot].surface_h = frame.height;
    app_sessions[slot].scaled_surface = scaled;
    app_sessions[slot].source_w = source_w;
    app_sessions[slot].source_h = source_h;
    app_sessions[slot].last_sequence = frame.sequence;
    copy_text(app_sessions[slot].title, frame.title, sizeof(app_sessions[slot].title));
    windows[WIN_APP_BASE + slot].title = app_sessions[slot].title[0]
        ? app_sessions[slot].title : "Application";
    clamp_scroll(WIN_APP_BASE + slot);
    if (full_change) {
        desktop_dirty = 1;
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
        desktop_dirty = 1;
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

static int sync_app_size(int id) {
    int slot = app_slot_for_win(id);
    if (slot < 0 || !app_sessions[slot].used)
        return -1;
    int target_w;
    int target_h;
    app_target_size(id, &target_w, &target_h);
    if (target_w <= 0 || target_h <= 0)
        return -1;
    /* want_w/want_h are the last dimensions already submitted. Do not flood
     * the event pipe while waiting for the application's next frame. */
    if (target_w == app_sessions[slot].want_w &&
        target_h == app_sessions[slot].want_h) {
        app_sessions[slot].resize_dirty = 0;
        return 0;
    }
    app_sessions[slot].want_w = target_w;
    app_sessions[slot].want_h = target_h;
    if (app_send_event(slot, GUIAPP_EVT_RESIZE, 0, 0, 0, 0, 0) < 0)
        return -1;
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
        term_log("no free app window");
        return;
    }

    struct shm_mapping mapping;
    uint32_t surface_pixels = (uint32_t)sw * (uint32_t)sh;
    if (shm_create(GUIAPP_SHARED_SIZE_FOR_PIXELS(surface_pixels), &mapping) < 0) {
        term_log("shared surface failed");
        return;
    }
    struct guiapp_shared_surface *shared =
        (struct guiapp_shared_surface *)mapping.address;
    shared->sequence = 0;
    shared->capacity_pixels = surface_pixels;
    shared->width = 0;
    shared->height = 0;

    int ev_pipe[2] = {-1, -1};
    int frame_pipe[2] = {-1, -1};
    if (pipe(ev_pipe) < 0 || pipe(frame_pipe) < 0) {
        if (ev_pipe[0] >= 0) close(ev_pipe[0]);
        if (ev_pipe[1] >= 0) close(ev_pipe[1]);
        (void)shm_unmap(mapping.token);
        term_log("pipe failed");
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
    term_log(msg);
    int pid = spawn_process_args(path, argv, argc,
                                 SPAWN_FLAG_SILENT | SPAWN_FLAG_INHERIT_FDS);
    close(ev_pipe[0]);
    close(frame_pipe[1]);
    if (pid < 0) {
        close(ev_pipe[1]);
        close(frame_pipe[0]);
        (void)shm_unmap(mapping.token);
        term_log("launch failed");
        return;
    }

    int id = WIN_APP_BASE + slot;
    windows[id].title = app_sessions[slot].title;
    windows[id].r = (struct rect){
        80 + slot * 36, 74 + slot * 34,
        min_i(APP_DEFAULT_W + 30, sw - 120),
        min_i(APP_DEFAULT_H + 70, sh - 150)
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
    app_sessions[slot].reader_dead = 0;
    app_sessions[slot].closing = 0;
    app_sessions[slot].shm_token = mapping.token;
    app_sessions[slot].shared = shared;
    app_sessions[slot].dirty_lock = 0;
    app_sessions[slot].dirty_valid = 0;
    app_sessions[slot].dirty_rect = (struct rect){0, 0, 0, 0};
    app_sessions[slot].last_sequence = 0;
    copy_text(app_sessions[slot].title, "Application", sizeof(app_sessions[slot].title));

    app_sessions[slot].reader_tid = spawn(app_reader_functions[slot]);
    if (app_sessions[slot].reader_tid < 0 ||
        app_send_event(slot, GUIAPP_EVT_INIT, 0, 0, 0, 0, 0) < 0) {
        term_log("app protocol failed");
        close_window(id);
        return;
    }

    copy_text(msg, "started pid ", sizeof(msg));
    append_uint(msg, (unsigned int)pid, sizeof(msg));
    term_log(msg);
    activate(id);
}

static void run_app(const char *path) {
    run_app_with_arg(path, 0);
}

static void activate(int id) {
    if (id < 0 || id >= WIN_COUNT)
        return;
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
}

static void layout(void) {
    int margin = max_i(18, sw / 48);
    int top = 30;
    int dock = 72;
    int content_h = sh - top - dock - margin * 2;
    int left_w = min_i(max_i(360, sw / 3), 520);
    int right_w = min_i(max_i(320, sw / 4), 460);
    int term_h = min_i(max_i(250, content_h / 2), content_h - 80);

    windows[WIN_LAUNCHER].title = "Applications";
    windows[WIN_LAUNCHER].r = (struct rect){margin, top + margin, left_w, content_h};
    windows[WIN_LAUNCHER].restore = windows[WIN_LAUNCHER].r;
    windows[WIN_LAUNCHER].visible = 1;

    windows[WIN_TERMINAL].title = "Terminal";
    windows[WIN_TERMINAL].r = (struct rect){
        margin + left_w + margin,
        top + margin,
        max_i(360, sw - left_w - right_w - margin * 4),
        term_h
    };
    windows[WIN_TERMINAL].restore = windows[WIN_TERMINAL].r;
    windows[WIN_TERMINAL].visible = 1;

    windows[WIN_STATUS].title = "System";
    windows[WIN_STATUS].r = (struct rect){
        sw - right_w - margin,
        top + margin,
        right_w,
        min_i(content_h, max_i(260, content_h / 2))
    };
    windows[WIN_STATUS].restore = windows[WIN_STATUS].r;
    windows[WIN_STATUS].visible = 1;

    z_order[0] = WIN_LAUNCHER;
    z_order[1] = WIN_TERMINAL;
    z_order[2] = WIN_STATUS;
    for (int i = 0; i < MAX_GUI_APPS; i++) {
        int id = WIN_APP_BASE + i;
        windows[id].title = "Application";
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
    fill((struct rect){0, 0, sw, sh}, plt_rgb(22, 30, 42));
}

static void draw_topbar(void) {
    fill((struct rect){0, 0, sw, 30}, plt_rgb(22, 26, 34));
    fill((struct rect){0, 29, sw, 1}, plt_rgb(52, 62, 80));
    text(14, 7, "BuzzOS", THEME_ACCENT, -1);
    text(88, 7, "Desktop", THEME_TEXT_DIM, -1);
    text(sw - 178, 7, "Framebuffer 32bpp", THEME_TEXT_FAINT, -1);
}

static struct rect close_rect(int id);
static struct rect max_rect(int id);
static struct rect min_rect(int id);

static void draw_window_frame(int id) {
    struct window *w = &windows[id];
    if (!w->visible || w->minimized)
        return;
    struct rect r = w->r;
    shadow(r);
    fill_round_t(r, corner_outer,
                 w->active ? THEME_WIN_BORDER_ACT : THEME_WIN_BORDER_INACT);
    fill_round_t((struct rect){r.x + 1, r.y + 1, r.w - 2, r.h - 2},
                 corner_inner, THEME_WIN_BODY);
    fill_round_top((struct rect){r.x + 1, r.y + 1, r.w - 2, 28},
                   corner_inner,
                   w->active ? THEME_TITLE_ACT : THEME_TITLE_INACT);
    line_h(r.x + 1, r.y + 29, r.w - 2,
           w->active ? THEME_ACCENT_SOFT : THEME_WIN_BORDER_INACT);
    text_clip(r.x + 12, r.y + 7, w->title,
              w->active ? THEME_TEXT : THEME_TEXT_FAINT, -1,
              (struct rect){r.x + 10, r.y + 4, r.w - 88, 22});
    struct rect mn = min_rect(id);
    struct rect mx = max_rect(id);
    struct rect cl = close_rect(id);
    int idle = gray(3);
    fill_circle(mn.x + 6, mn.y + 6, 5, w->active ? THEME_MIN_YELLOW : idle);
    fill_circle(mx.x + 6, mx.y + 6, 5, w->active ? THEME_MAX_GREEN : idle);
    fill_circle(cl.x + 6, cl.y + 6, 5, w->active ? THEME_CLOSE_RED : idle);
    int glyph = plt_rgb(40, 20, 20);
    line_h(mn.x + 4, mn.y + 6, 5, gray(0));
    border((struct rect){mx.x + 4, mx.y + 4, 5, 5}, gray(0), gray(0));
    for (int i = 0; i < 5; i++) {
        pixel(cl.x + 4 + i, cl.y + 4 + i, glyph);
        pixel(cl.x + 8 - i, cl.y + 4 + i, glyph);
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
    return (struct rect){r.x + 12, r.y + 40, r.w - 30, r.h - 70};
}

static struct rect scaled_view_rect(int id, int slot) {
    struct rect c = content_rect(id);
    int aw = app_sessions[slot].surface_w;
    int ah = app_sessions[slot].surface_h;
    int source_w = app_sessions[slot].source_w;
    int source_h = app_sessions[slot].source_h;
    if (!app_sessions[slot].scaled_surface || aw <= 0 || ah <= 0 ||
        source_w <= 0 || source_h <= 0)
        return c;
    int vw = aw;
    int vh = aw * source_h / source_w;
    if (vh > ah) {
        vh = ah;
        vw = ah * source_w / source_h;
    }
    return (struct rect){c.x + (aw - vw) / 2,
                         c.y + (ah - vh) / 2, vw, vh};
}

static struct rect close_rect(int id) {
    struct rect r = windows[id].r;
    return (struct rect){r.x + r.w - 27, r.y + 8, 12, 12};
}

static struct rect max_rect(int id) {
    struct rect r = windows[id].r;
    return (struct rect){r.x + r.w - 47, r.y + 8, 12, 12};
}

static struct rect min_rect(int id) {
    struct rect r = windows[id].r;
    return (struct rect){r.x + r.w - 67, r.y + 8, 12, 12};
}

static int content_width(int id) {
    int slot = app_slot_for_win(id);
    if (slot >= 0 && app_sessions[slot].used)
        return content_rect(id).w;
    if (id == WIN_TERMINAL)
        return TERM_COLS * FONT_GLYPH_MAX_WIDTH + 28;
    if (id == WIN_LAUNCHER)
        return 460;
    return 520;
}

static int content_height(int id) {
    int slot = app_slot_for_win(id);
    if (slot >= 0 && app_sessions[slot].used)
        return content_rect(id).h;
    if (id == WIN_LAUNCHER)
        return 28 + max_i(app_count, 1) * 34 + 12;
    if (id == WIN_TERMINAL)
        return max_i((term_row + 2) * 22 + 18, 180);
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
    fill(vt, THEME_WIN_BODY);
    fill(ht, THEME_WIN_BODY);
    struct rect vth = vscroll_thumb(id);
    struct rect hth = hscroll_thumb(id);
    fill_round((struct rect){vth.x + 1, vth.y + 1, vth.w - 2, vth.h - 2},
               max_scroll_y(id) ? gray(5) : gray(3));
    fill_round((struct rect){hth.x + 1, hth.y + 1, hth.w - 2, hth.h - 2},
               max_scroll_x(id) ? gray(5) : gray(3));
}

static void draw_launcher(void) {
    if (!windows[WIN_LAUNCHER].visible || windows[WIN_LAUNCHER].minimized)
        return;
    draw_window_frame(WIN_LAUNCHER);
    struct rect c = content_rect(WIN_LAUNCHER);
    fill(c, THEME_WIN_BODY);
    struct rect clip = c;
    int ox = c.x - scroll_x[WIN_LAUNCHER];
    int oy = c.y - scroll_y[WIN_LAUNCHER];
    text_clip(ox, oy, "Installed", THEME_ACCENT, -1, clip);
    int y = oy + 28;
    if (app_count == 0) {
        text_clip(ox, y, "No apps in /fs/apps", THEME_TEXT_DIM, -1, clip);
        draw_scrollbars(WIN_LAUNCHER);
        return;
    }
    for (int i = 0; i < app_count; i++) {
        struct rect row = {ox, y + i * 34, content_width(WIN_LAUNCHER) - 20, 28};
        struct rect visible = intersect_rect(row, clip);
        if (visible.w <= 0 || visible.h <= 0)
            continue;
        int selected = i == app_selected;
        if (selected) {
            struct rect sel = intersect_rect(
                (struct rect){row.x, row.y, row.w - 6, row.h}, clip);
            if (sel.w > 0 && sel.h > 0)
                fill_round(sel, THEME_ACCENT_SOFT);
        }
        struct rect icon = intersect_rect(
            (struct rect){row.x + 8, row.y + 6, 16, 16}, clip);
        if (icon.w > 0 && icon.h > 0)
            fill_round(icon, rgb6((i % 5) + 1, 3, 4));
        text_clip(row.x + 34, row.y + 6, apps[i].name,
                  selected ? THEME_TEXT : THEME_TEXT_DIM, -1, clip);
    }
    draw_scrollbars(WIN_LAUNCHER);
}

static void draw_terminal(void) {
    if (!windows[WIN_TERMINAL].visible || windows[WIN_TERMINAL].minimized)
        return;
    draw_window_frame(WIN_TERMINAL);
    struct rect c = content_rect(WIN_TERMINAL);
    fill(c, plt_rgb(14, 18, 26));
    border(c, gray(3), THEME_WIN_BORDER_INACT);
    struct rect clip = {c.x + 1, c.y + 1, c.w - 2, c.h - 2};
    int ox = c.x + 10 - scroll_x[WIN_TERMINAL];
    int y = c.y + 8 - scroll_y[WIN_TERMINAL];
    term_lock_enter();
    for (int i = 0; i < TERM_LINES; i++) {
        if (term_lines[i][0]) {
            int ar = term_select_anchor_row, ap = term_select_anchor_pos;
            int br = term_select_row, bp = term_select_pos;
            if (ar > br || (ar == br && ap > bp)) {
                int tr = ar, tp = ap; ar = br; ap = bp; br = tr; bp = tp;
            }
            int px = ox;
            int pos = 0;
            while (term_lines[i][pos]) {
                int start = pos;
                const char *p = term_lines[i] + pos;
                uint32_t cp = gui_utf8_next(&p);
                pos = (int)(p - term_lines[i]);
                int glyph_w = gui_codepoint_width(cp);
                int selected = (i > ar || (i == ar && pos > ap)) &&
                               (i < br || (i == br && start < bp));
                if (selected) {
                    struct rect mark = intersect_rect(
                        (struct rect){px, y - PLT_FONT_Y_SHIFT, glyph_w, KFONT_HEIGHT}, clip);
                    if (mark.w > 0 && mark.h > 0)
                        fill(mark, rgb6(1, 3, 5));
                }
                px += glyph_w;
            }
            text_clip(ox, y, term_lines[i], rgb6(3, 5, 4), -1, clip);
        }
        y += 22;
    }
    if ((tick / 30) & 1)
        fill(intersect_rect((struct rect){
            ox + gui_text_width(term_lines[term_row]),
            c.y + 8 + term_row * 22 - scroll_y[WIN_TERMINAL] + 4,
            8, 16
        }, clip), 15);
    term_lock_leave();
    draw_scrollbars(WIN_TERMINAL);
}

static void terminal_position_at(int mx, int my, int *row_out, int *pos_out) {
    struct rect c = content_rect(WIN_TERMINAL);
    int row = (my - (c.y + 8) + scroll_y[WIN_TERMINAL]) / 22;
    row = clamp_i(row, 0, TERM_LINES - 1);
    int target_x = mx - (c.x + 10) + scroll_x[WIN_TERMINAL];
    if (target_x < 0) target_x = 0;
    int x = 0;
    int pos = 0;
    while (term_lines[row][pos]) {
        int start = pos;
        const char *p = term_lines[row] + pos;
        uint32_t cp = gui_utf8_next(&p);
        pos = (int)(p - term_lines[row]);
        int width = gui_codepoint_width(cp);
        if (target_x < x + width / 2) { pos = start; break; }
        x += width;
    }
    *row_out = row;
    *pos_out = pos;
}

static int terminal_has_selection(void) {
    return term_select_anchor_row != term_select_row ||
           term_select_anchor_pos != term_select_pos;
}

static void terminal_copy_selection(void) {
    int ar = term_select_anchor_row, ap = term_select_anchor_pos;
    int br = term_select_row, bp = term_select_pos;
    if (ar > br || (ar == br && ap > bp)) {
        int tr = ar, tp = ap; ar = br; ap = bp; br = tr; bp = tp;
    }
    int out = 0;
    clipboard[0] = 0;
    for (int row = ar; row <= br && out + 1 < (int)sizeof(clipboard); row++) {
        int first = row == ar ? ap : 0;
        int last = row == br ? bp : (int)strlen(term_lines[row]);
        int pos = first;
        while (pos < last) {
            const char *p = term_lines[row] + pos;
            (void)gui_utf8_next(&p);
            int next = (int)(p - term_lines[row]);
            int bytes = next - pos;
            if (out + bytes >= (int)sizeof(clipboard)) break;
            for (int i = 0; i < bytes; i++) clipboard[out++] = term_lines[row][pos + i];
            pos = next;
        }
        if (row < br && out + 1 < (int)sizeof(clipboard))
            clipboard[out++] = '\n';
    }
    clipboard[out] = 0;
}

static void draw_status(void) {
    if (!windows[WIN_STATUS].visible || windows[WIN_STATUS].minimized)
        return;
    draw_window_frame(WIN_STATUS);
    struct rect c = content_rect(WIN_STATUS);
    fill(c, THEME_WIN_BODY);
    struct rect clip = c;
    int ox = c.x - scroll_x[WIN_STATUS];
    int oy = c.y - scroll_y[WIN_STATUS];
    char line[96];
    text_clip(ox, oy, "Display", THEME_ACCENT, -1, clip);
    copy_text(line, "resolution ", sizeof(line));
    append_uint(line, (unsigned int)sw, sizeof(line));
    append_text(line, " x ", sizeof(line));
    append_uint(line, (unsigned int)sh, sizeof(line));
    text_clip(ox, oy + 30, line, THEME_TEXT_DIM, -1, clip);
    copy_text(line, "apps ", sizeof(line));
    append_uint(line, (unsigned int)app_count, sizeof(line));
    text_clip(ox, oy + 58, line, THEME_TEXT_DIM, -1, clip);
    text_clip(ox, oy + 96, "Controls", THEME_ACCENT, -1, clip);
    text_clip(ox, oy + 126, "Enter launches selected app", THEME_TEXT_DIM, -1, clip);
    text_clip(ox, oy + 150, "Tab cycles windows", THEME_TEXT_DIM, -1, clip);
    text_clip(ox, oy + 174, "Esc exits desktop", THEME_TEXT_DIM, -1, clip);
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
    uint32_t sequence;
    const uint8_t *pixels = (const uint8_t *)shared +
        GUIAPP_SHARED_HEADER_SIZE;
    if (app_sessions[slot].scaled_surface && source_w > 0 && source_h > 0) {
        struct rect view = scaled_view_rect(id, slot);
        int vw = view.w;
        int vh = view.h;
        int dx = view.x;
        int dy = view.y;
        int right = ox + aw;
        int bottom = oy + ah;
        fill((struct rect){ox, oy, aw, dy - oy}, 0);
        fill((struct rect){ox, dy + vh, aw, bottom - (dy + vh)}, 0);
        fill((struct rect){ox, dy, dx - ox, vh}, 0);
        fill((struct rect){dx + vw, dy, right - (dx + vw), vh}, 0);
        struct rect visible = intersect_rect(intersect_rect(view, clip), compose_clip);
        if (visible.w <= 0 || visible.h <= 0)
            return;
        if (app_sessions[slot].scale_map_w != vw ||
            app_sessions[slot].scale_map_h != vh ||
            app_sessions[slot].scale_source_w != source_w ||
            app_sessions[slot].scale_source_h != source_h) {
            for (int x = 0; x < vw; x++)
                app_sessions[slot].xmap[x] = (uint16_t)(x * source_w / vw);
            for (int y = 0; y < vh; y++)
                app_sessions[slot].ymap[y] = (uint16_t)(y * source_h / vh);
            app_sessions[slot].scale_map_w = vw;
            app_sessions[slot].scale_map_h = vh;
            app_sessions[slot].scale_source_w = source_w;
            app_sessions[slot].scale_source_h = source_h;
        }
        int xscale = vw / source_w, yscale = vh / source_h;
        /* Copy straight into the backbuffer, retrying until the seqlock
         * confirms a tear-free pass.  Intermediate tears never reach the
         * screen: the display is only updated from the backbuffer after
         * compose finishes. */
        int copied = 0;
        for (int attempt = 0; attempt < 100 && !copied; attempt++) {
            sequence = shared->sequence;
            if (sequence & 1u) {
                yield();
                continue;
            }
            __sync_synchronize();
        if (xscale > 0 && yscale > 0 &&
            xscale * source_w == vw && yscale * source_h == vh) {
            /* Expand each source row only once, then reuse it for all of its
             * vertically repeated rows.  The old loop expanded every output
             * row separately (and made tens of thousands of tiny memset
             * calls per second for a 4x Game Boy surface). */
            int first_x = visible.x - dx;
            int cached_sy = -1;
            for (int y = 0; y < visible.h; y++) {
                uint8_t *dst = fb + (visible.y + y) * sw + visible.x;
                int sy = (visible.y + y - dy) / yscale;
                if (sy != cached_sy) {
                    const uint8_t *src = pixels + sy * source_w;
                    int out = 0, pos = first_x;
                    while (out < visible.w) {
                        int sx = pos / xscale;
                        int run = xscale - pos % xscale;
                        if (run > visible.w - out) run = visible.w - out;
                        memset(scaled_scanline + out, src[sx], (size_t)run);
                        out += run;
                        pos += run;
                    }
                    cached_sy = sy;
                }
                memcpy(dst, scaled_scanline, (size_t)visible.w);
            }
        } else {
            int cached_sy = -1;
            for (int y = 0; y < visible.h; y++) {
                uint8_t *dst = fb + (visible.y + y) * sw + visible.x;
                int sy = app_sessions[slot].ymap[visible.y + y - dy];
                if (sy != cached_sy) {
                    const uint8_t *src = pixels + sy * source_w;
                    int sx = visible.x - dx;
                    for (int x = 0; x < visible.w; x++)
                        scaled_scanline[x] =
                            src[app_sessions[slot].xmap[sx + x]];
                    cached_sy = sy;
                }
                memcpy(dst, scaled_scanline, (size_t)visible.w);
            }
        }
            __sync_synchronize();
            copied = shared->sequence == sequence;
            if (!copied && (attempt & 3) == 3)
                yield();
        }
        if (!copied)
            app_note_dirty(slot, (struct rect){0, 0, source_w, source_h});
        return;
    }
    /* Paint only the margins around the surface; the surface area is
     * (re)painted below once the seqlock confirms a clean copy. */
    fill((struct rect){ox + aw, c.y, (c.x + c.w) - (ox + aw), c.h},
         THEME_WIN_BODY);
    fill((struct rect){c.x, oy + ah, c.w, (c.y + c.h) - (oy + ah)},
         THEME_WIN_BODY);
    struct rect visible = intersect_rect(
        intersect_rect((struct rect){ox, oy, aw, ah}, clip), compose_clip);
    if (visible.w > 0 && visible.h > 0) {
        int sx = visible.x - ox;
        int sy = visible.y - oy;
        int copied = 0;
        for (int attempt = 0; attempt < 100 && !copied; attempt++) {
            sequence = shared->sequence;
            if (sequence & 1u) {
                yield();
                continue;
            }
            __sync_synchronize();
            for (int y = 0; y < visible.h; y++)
                memcpy(fb + (visible.y + y) * sw + visible.x,
                       pixels + (sy + y) * aw + sx, (size_t)visible.w);
            __sync_synchronize();
            copied = shared->sequence == sequence;
            if (!copied && (attempt & 3) == 3)
                yield();
        }
        if (!copied)
            app_note_dirty(slot, (struct rect){0, 0, aw, ah});
    }
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
    *dock_w = min_i(980, sw - 40);
    if (*dock_w < 420)
        *dock_w = sw - 12;
    *x = (sw - *dock_w) / 2;
    *y = sh - 66;
    int available = *dock_w - 36 - 3 * 70 - 70;
    *task_cap = max_i(1, available / 118);
}

static void draw_dock_tooltip(void) {
    if (dock_hover < WIN_APP_BASE || dock_hover >= WIN_COUNT ||
        !windows[dock_hover].visible)
        return;
    const char *title = windows[dock_hover].title;
    int tw = min_i(sw - 16, gui_text_width(title) + 20);
    int tx = clamp_i(pointer_x - tw / 2, 8, sw - tw - 8);
    int ty = sh - 96;
    struct rect tip = {tx, ty, tw, 26};
    fill_round(tip, THEME_WIN_CONTROL);
    fill_round((struct rect){tip.x + 1, tip.y + 1, tip.w - 2, tip.h - 2},
               THEME_WIN_BODY);
    text_clip(tip.x + 10, tip.y + 5, title, THEME_TEXT, -1,
              (struct rect){tip.x + 5, tip.y + 2, tip.w - 10, tip.h - 4});
}

static void draw_dock(void) {
    int ids[MAX_GUI_APPS];
    int app_total = collect_open_apps(ids);
    int x, y, dock_w, task_cap;
    dock_geometry(&x, &y, &dock_w, &task_cap);
    struct rect r = {x, y, dock_w, 54};
    /* Lift the dock off the wallpaper: soft shadow, bright ring,
     * lighter panel and a top highlight. */
    fill_blend((struct rect){r.x + 4, r.y + r.h, r.w - 4, 5}, 0, 70);
    fill_blend((struct rect){r.x + r.w, r.y + 4, 5, r.h - 4}, 0, 70);
    fill_round_t(r, corner_outer, plt_rgb(78, 92, 114));
    fill_round_t((struct rect){r.x + 1, r.y + 1, r.w - 2, r.h - 2},
                 corner_inner, plt_rgb(43, 51, 66));
    line_h(r.x + 5, r.y + 1, r.w - 10, plt_rgb(96, 112, 138));

    static const char *system_labels[] = {"Apps", "Term", "Sys"};
    for (int i = 0; i < WIN_APP_BASE; i++) {
        struct rect b = {x + 12 + i * 70, y + 9, 64, 36};
        button(b, system_labels[i], windows[i].active || dock_hover == i);
    }

    int shown = min_i(app_total, task_cap);
    int task_x = x + 12 + WIN_APP_BASE * 70;
    line_v(task_x - 6, y + 10, 34, plt_rgb(78, 92, 114));
    for (int i = 0; i < shown; i++) {
        int id = ids[i];
        struct rect b = {task_x + i * 118, y + 9, 112, 36};
        button(b, windows[id].title, windows[id].active || dock_hover == id);
        if (windows[id].active)
            fill_circle(b.x + b.w / 2, y + 49, 2, THEME_ACCENT);
    }
    if (app_total > 0) {
        int more_x = x + dock_w - 76;
        button((struct rect){more_x, y + 9, 64, 36},
               dock_expanded ? "Hide" : "More", dock_hover == WIN_COUNT);
    }

    if (dock_expanded && app_total > 0) {
        int panel_w = min_i(380, sw - 24);
        int panel_h = 14 + app_total * 30;
        int panel_x = x + dock_w - panel_w;
        int panel_y = y - panel_h - 6;
        struct rect panel = {panel_x, panel_y, panel_w, panel_h};
        fill_round(panel, THEME_WIN_CONTROL);
        fill_round((struct rect){panel.x + 1, panel.y + 1,
                                 panel.w - 2, panel.h - 2}, THEME_WIN_BODY);
        for (int i = 0; i < app_total; i++) {
            int id = ids[i];
            struct rect item = {panel_x + 7, panel_y + 7 + i * 30,
                                panel_w - 14, 26};
            button(item, windows[id].title,
                   windows[id].active || dock_hover == id);
        }
    }
    draw_dock_tooltip();
}

static const char *ime_candidates(void) {
    for (int i = 0; i < PINYIN_ENTRY_COUNT; i++)
        if (strcmp(pinyin_entries[i].key, ime_buffer) == 0)
            return pinyin_entries[i].items;
    return 0;
}

static int ime_candidate_at(int wanted, char out[GUIAPP_TEXT_MAX]) {
    const char *items = ime_candidates();
    int index = 0;
    if (!items || wanted < 0)
        return 0;
    while (*items) {
        while (*items == ' ') items++;
        if (!*items) break;
        const char *start = items;
        while (*items && *items != ' ') items++;
        if (index++ == wanted) {
            int n = (int)(items - start);
            if (n >= GUIAPP_TEXT_MAX) n = GUIAPP_TEXT_MAX - 1;
            for (int i = 0; i < n; i++) out[i] = start[i];
            out[n] = 0;
            return 1;
        }
    }
    return 0;
}

static void draw_ime(void) {
    const char *mode = ime_enabled ? "中" : "英";
    struct rect badge = {sw - 48, 8, 34, 25};
    fill_round(badge, ime_enabled ? THEME_ACCENT_DIM : THEME_WIN_CONTROL);
    text_clip(badge.x + 5, badge.y + 2, mode, THEME_TEXT, -1,
              (struct rect){badge.x + 2, badge.y + 1, badge.w - 4, badge.h - 2});
    if (!ime_enabled || ime_length == 0)
        return;
    char line[192];
    copy_text(line, ime_buffer, sizeof(line));
    append_text(line, "   ", sizeof(line));
    for (int i = 0; i < 9; i++) {
        char item[GUIAPP_TEXT_MAX];
        char number[4] = {(char)('1' + i), '.', 0, 0};
        if (!ime_candidate_at(i, item)) break;
        append_text(line, number, sizeof(line));
        append_text(line, item, sizeof(line));
        append_text(line, "  ", sizeof(line));
    }
    int panel_w = min_i(sw - 24, max_i(260, gui_text_width(line) + 20));
    struct rect panel = {(sw - panel_w) / 2, sh - 108, panel_w, 32};
    fill_round(panel, THEME_ACCENT_DIM);
    fill_round((struct rect){panel.x + 1, panel.y + 1,
                             panel.w - 2, panel.h - 2}, THEME_WIN_BODY);
    text_clip(panel.x + 10, panel.y + 5, line, THEME_TEXT, -1,
              (struct rect){panel.x + 5, panel.y + 3, panel.w - 10, panel.h - 6});
}

static void draw_context_menu(void) {
    if (!context_open) return;
    static const char *labels[] = {"Copy", "Paste", "Cut"};
    struct rect menu = {context_x, context_y, 124, 88};
    if (menu.x + menu.w > sw) menu.x = sw - menu.w;
    if (menu.y + menu.h > sh) menu.y = sh - menu.h;
    context_x = menu.x; context_y = menu.y;
    fill_round(menu, THEME_WIN_CONTROL);
    fill_round((struct rect){menu.x + 1, menu.y + 1,
                             menu.w - 2, menu.h - 2}, THEME_WIN_BODY);
    for (int i = 0; i < 3; i++)
        button((struct rect){menu.x + 4, menu.y + 4 + i * 27, menu.w - 8, 25},
               labels[i], i == 1 && clipboard[0]);
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
            pixel(pointer_x + x, pointer_y + y, edge ? 0 : 15);
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
        else if (id == WIN_TERMINAL)
            draw_terminal();
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

static int render_region(struct rect area) {
    area = intersect_rect(area, (struct rect){0, 0, sw, sh});
    if (area.w <= 0 || area.h <= 0)
        return 0;
    compose_clip = area;
    compose_scene();
    compose_clip = (struct rect){0, 0, sw, sh};
    return fb_blit_stride(area.x, area.y, area.w, area.h,
                          fb + area.y * sw + area.x, sw);
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
    if (!app_sessions[slot].scaled_surface) {
        struct rect area = {
            content.x + dirty.x, content.y + dirty.y, dirty.w, dirty.h
        };
        return intersect_rect(area,
            intersect_rect(content, (struct rect){0, 0, sw, sh}));
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
    return intersect_rect(area,
        intersect_rect(view, (struct rect){0, 0, sw, sh}));
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
    struct rect title = {r.x, r.y, r.w, 30};
    return inside(x, y, title) ? i : -1;
}

static int hit_window(int x, int y) {
    return top_window_at(x, y);
}

static int hit_control(int x, int y, int *control_out) {
    int i = top_window_at(x, y);
    if (i < 0)
        return -1;
    if (inside(x, y, close_rect(i))) {
        *control_out = 2;
        return i;
    }
    if (inside(x, y, max_rect(i))) {
        *control_out = 1;
        return i;
    }
    if (inside(x, y, min_rect(i))) {
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
        int in_title = covered && y >= r.y && y < r.y + 30;
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
    if (r.y < 30) {
        r.h += r.y - 30;
        r.y = 30;
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
    if (slot >= 0 && app_sessions[slot].used)
        app_sessions[slot].resize_dirty = 1;
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
    int slot = app_slot_for_win(id);
    if (slot >= 0 && app_sessions[slot].used) {
        app_sessions[slot].closing = 1;
        (void)app_send_event(slot, GUIAPP_EVT_CLOSE, 0, 0, 0, 0, 0);
        close(app_sessions[slot].to_fd);
        if (app_sessions[slot].pid > 0) {
            int status;
            (void)kill(app_sessions[slot].pid);
            (void)waitpid(app_sessions[slot].pid, &status, 0);
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
            term_log("app protocol ended");
            puts("[gui] app protocol ended");
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
        windows[id].r = (struct rect){8, 34, sw - 16, dock_y - 40};
        windows[id].maximized = 1;
    }
    clamp_scroll(id);
    int slot = app_slot_for_win(id);
    if (slot >= 0 && app_sessions[slot].used) {
        app_sessions[slot].resize_dirty = 1;
        (void)sync_app_size(id);
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
    if (dock_expanded && app_total > 0) {
        int panel_w = min_i(380, sw - 24);
        int panel_h = 14 + app_total * 30;
        int panel_x = dx + dock_w - panel_w;
        int panel_y = dy - panel_h - 6;
        for (int i = 0; i < app_total; i++) {
            struct rect item = {panel_x + 7, panel_y + 7 + i * 30,
                                panel_w - 14, 26};
            if (inside(x, y, item))
                return ids[i];
        }
    }
    for (int i = 0; i < WIN_APP_BASE; i++) {
        if (inside(x, y, (struct rect){dx + 12 + i * 70, dy + 9, 64, 36}))
            return i;
    }
    int shown = min_i(app_total, task_cap);
    int task_x = dx + 12 + WIN_APP_BASE * 70;
    for (int i = 0; i < shown; i++) {
        if (inside(x, y, (struct rect){task_x + i * 118, dy + 9, 112, 36}))
            return ids[i];
    }
    if (app_total > 0 &&
        inside(x, y, (struct rect){dx + dock_w - 76, dy + 9, 64, 36}))
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

static void flush_pending_app_resizes(void) {
    /* Send coalesced resize events to apps even mid-drag so their content
     * relayouts live.  sync_app_size() dedups against want_w/want_h, so
     * unchanged sizes cost nothing. */
    for (int slot = 0; slot < MAX_GUI_APPS; slot++) {
        if (!app_sessions[slot].used || !app_sessions[slot].resize_dirty)
            continue;
        (void)sync_app_size(WIN_APP_BASE + slot);
    }
}

static void terminal_send(const char *s, int len) {
    if (term_in_fd >= 0 && s && len > 0)
        (void)write(term_in_fd, s, (size_t)len);
}

static void terminal_execute_path(const char *path) {
    terminal_send("\x15", 1);
    terminal_send(path, (int)strlen(path));
    terminal_send("\n", 1);
    term_input_len = 0;
    term_input[0] = 0;
    activate(WIN_TERMINAL);
}

static void terminal_input_append(const char *s) {
    int n = (int)strlen(s);
    if (n <= 0 || term_input_len + n >= (int)sizeof(term_input)) return;
    for (int i = 0; i < n; i++) term_input[term_input_len++] = s[i];
    term_input[term_input_len] = 0;
}

static void terminal_input_backspace(void) {
    term_input_len = gui_utf8_prev(term_input, term_input_len);
    term_input[term_input_len] = 0;
}

static void terminal_send_key(int k) {
    char ch;
    if (k == KEY_UP)
        terminal_send("\x1B[A", 3);
    else if (k == KEY_DOWN)
        terminal_send("\x1B[B", 3);
    else if (k == KEY_RIGHT)
        terminal_send("\x1B[C", 3);
    else if (k == KEY_LEFT)
        terminal_send("\x1B[D", 3);
    else if (k == KEY_BACKSPACE || k == 127)
        { terminal_send("\b", 1); terminal_input_backspace(); }
    else if (k == '\r') {
        terminal_send("\n", 1);
        term_input_len = 0; term_input[0] = 0;
    }
    else if (k >= 0 && k < 256) {
        ch = (char)k;
        terminal_send(&ch, 1);
        if (k >= 32 && k != 127) {
            char value[2] = {ch, 0};
            terminal_input_append(value);
        }
    }
}

static int ime_target_active(void) {
    if (focus == WIN_TERMINAL)
        return 1;
    int slot = app_slot_for_win(focus);
    return slot >= 0 && app_sessions[slot].used;
}

static void ime_submit(const char *value) {
    if (!value || !value[0]) return;
    if (focus == WIN_TERMINAL) {
        terminal_send(value, (int)strlen(value));
        terminal_input_append(value);
    } else {
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
}

static void clipboard_command(int command) {
    if (context_target < 0) return;
    activate(context_target);
    if (command == GUIAPP_CMD_PASTE) {
        if (clipboard[0]) ime_submit(clipboard);
        return;
    }
    if (context_target == WIN_TERMINAL) {
        if (terminal_has_selection())
            terminal_copy_selection();
        else
            copy_text(clipboard, term_input, sizeof(clipboard));
        if (!terminal_has_selection() && command == GUIAPP_CMD_CUT && term_input_len > 0) {
            terminal_send("\x15", 1); /* Ctrl+U: clear shell edit line */
            term_input_len = 0;
            term_input[0] = 0;
        }
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
}

static void ime_commit_candidate(int index) {
    char item[GUIAPP_TEXT_MAX];
    if (ime_candidate_at(index, item))
        ime_submit(item);
    else
        ime_submit(ime_buffer);
    ime_clear();
}

static const char *ime_punctuation(int k) {
    switch (k) {
    case ',': return "，"; case '.': return "。"; case '?': return "？";
    case '!': return "！"; case ':': return "："; case ';': return "；";
    case '(': return "（"; case ')': return "）";
    default: return 0;
    }
}

/* Returns non-zero when the desktop IME consumed the key. */
static int ime_handle_key(int k) {
    if (k == 0x1F) { /* Ctrl+Space from the keyboard driver */
        ime_enabled = !ime_enabled;
        ime_clear();
        desktop_dirty = 1;
        return 1;
    }
    if (!ime_enabled || !ime_target_active())
        return 0;
    if (k == KEY_ESC && ime_length > 0) {
        ime_clear(); desktop_dirty = 1; return 1;
    }
    if ((k == KEY_BACKSPACE || k == 127) && ime_length > 0) {
        ime_buffer[--ime_length] = 0;
        desktop_dirty = 1;
        return 1;
    }
    if ((k >= 'a' && k <= 'z') || (k >= 'A' && k <= 'Z')) {
        if (ime_length + 1 < (int)sizeof(ime_buffer)) {
            if (k >= 'A' && k <= 'Z') k += 'a' - 'A';
            ime_buffer[ime_length++] = (char)k;
            ime_buffer[ime_length] = 0;
        }
        desktop_dirty = 1;
        return 1;
    }
    if (ime_length > 0) {
        if (k >= '1' && k <= '9') {
            ime_commit_candidate(k - '1'); desktop_dirty = 1; return 1;
        }
        if (k == ' ' || k == '\r' || k == '\n') {
            ime_commit_candidate(0); desktop_dirty = 1; return 1;
        }
        /* Commit the composition before passing punctuation/navigation on. */
        ime_commit_candidate(0);
    }
    const char *punct = ime_punctuation(k);
    if (punct) {
        ime_submit(punct); desktop_dirty = 1; return 1;
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
        return;
    }
    if (focus == WIN_LAUNCHER) {
        if (k == KEY_UP && app_selected > 0)
            app_selected--;
        else if (k == KEY_DOWN && app_selected + 1 < app_count)
            app_selected++;
        else if ((k == '\n' || k == '\r') && app_count > 0)
            run_app(apps[app_selected].path);
        else if (k == 'r' || k == 'R')
            scan_apps();
        return;
    }
    if (focus == WIN_TERMINAL) {
        terminal_send_key(k);
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

static void handle_mouse(void) {
    struct mouse_state ms;
    if (mouse_get(&ms) < 0)
        return;
    int old_pointer_x = pointer_x;
    int old_pointer_y = pointer_y;
    int old_dock_hover = dock_hover;
    int pointer_moved = ms.x != pointer_x || ms.y != pointer_y;
    if (ms.buttons != prev_buttons || ms.wheel_seq != last_wheel_seq)
        desktop_dirty = 1;
    pointer_x = ms.x;
    pointer_y = ms.y;
    if (pointer_moved) {
        queue_damage((struct rect){old_pointer_x - 1, old_pointer_y - 1, 18, 18});
        queue_damage((struct rect){pointer_x - 1, pointer_y - 1, 18, 18});
    }
    int left = ms.buttons & 1;
    int right = ms.buttons & 2;
    dock_hover = hit_dock(pointer_x, pointer_y);
    if (dock_hover != old_dock_hover)
        desktop_dirty = 1;

    if (right && !(prev_buttons & 2)) {
        int target = hit_window(pointer_x, pointer_y);
        if (target == WIN_TERMINAL ||
            (target >= WIN_APP_BASE && inside(pointer_x, pointer_y, content_rect(target)))) {
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
        int item = (pointer_y - context_y - 4) / 27;
        if (pointer_x >= context_x + 4 && pointer_x < context_x + 120 &&
            pointer_y >= context_y + 4 && item >= 0 && item < 3) {
            static const int commands[] = {GUIAPP_CMD_COPY, GUIAPP_CMD_PASTE, GUIAPP_CMD_CUT};
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
            }
        }
        last_wheel_seq = ms.wheel_seq;
        last_wheel_value = ms.wheel;
    }

    if (left && !prev_buttons) {
        if (dock_hover == WIN_COUNT) {
            dock_expanded = !dock_expanded;
            desktop_dirty = 1;
        } else if (dock_hover >= 0) {
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
            int t = hit_window_title(pointer_x, pointer_y);
            if (t >= 0) {
                drag_win = t;
                drag_dx = pointer_x - windows[t].r.x;
                drag_dy = pointer_y - windows[t].r.y;
            }
            if (focus == WIN_LAUNCHER) {
                struct rect c = content_rect(WIN_LAUNCHER);
                int rel = pointer_y - (c.y + 28) + scroll_y[WIN_LAUNCHER];
                if (rel >= 0) {
                    int idx = rel / 34;
                    if (idx >= 0 && idx < app_count) {
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
                }
            } else {
                int slot = app_slot_for_win(focus);
                if (focus == WIN_TERMINAL && inside(pointer_x, pointer_y, content_rect(focus))) {
                    terminal_position_at(pointer_x, pointer_y,
                                         &term_select_anchor_row, &term_select_anchor_pos);
                    term_select_row = term_select_anchor_row;
                    term_select_pos = term_select_anchor_pos;
                    term_selecting = 1;
                } else if (slot >= 0 && inside(pointer_x, pointer_y, content_rect(focus))) {
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
        term_selecting = 0;
        if (finished_resize >= WIN_APP_BASE)
            (void)sync_app_size(finished_resize);
    }
    if (left && term_selecting) {
        terminal_position_at(pointer_x, pointer_y, &term_select_row, &term_select_pos);
        queue_damage(windows[WIN_TERMINAL].r);
        prev_buttons = ms.buttons;
        return;
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
        if (r->y < 30) r->y = 30;
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
    } else {
        sw = (int)info.width;
        sh = (int)info.height;
    }
    if (sw > MAX_SW)
        sw = MAX_SW;
    if (sh > MAX_SH)
        sh = MAX_SH;
    compose_clip = (struct rect){0, 0, sw, sh};
    gfx_set_origin(0, 0);
    pointer_x = sw / 2;
    pointer_y = sh / 2;
    for (int i = 0; i < TERM_LINES; i++)
        term_lines[i][0] = 0;
    scan_apps();
    keyevent_fd = open("/dev/keyevent", O_RDONLY);
    layout();
    scroll_y[WIN_TERMINAL] = max_scroll_y(WIN_TERMINAL);
    scroll_x[WIN_TERMINAL] = 0;
    if (start_terminal_shell() < 0)
        term_log("terminal: failed to start /bin/sh");
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

    if (term_in_fd >= 0) {
        close(term_in_fd);
        term_in_fd = -1;
    }
    if (term_pid > 0) {
        int status;
        (void)kill(term_pid);
        (void)waitpid(term_pid, &status, 0);
        term_pid = -1;
    }
    if (term_reader_tid > 0) {
        (void)join(term_reader_tid);
        term_reader_tid = -1;
    }
    if (term_out_fd >= 0) {
        close(term_out_fd);
        term_out_fd = -1;
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    init_desktop();
    uint32_t frame_deadline = monotonic_ms();
    uint32_t frame_fraction = 0;
    while (running) {
        reap_dead_apps();
        int key;
        while ((key = read_key_poll()) >= 0) {
            desktop_dirty = 1;
            handle_key(key);
        }
        forward_key_releases();
        handle_mouse();
        flush_pending_app_resizes();
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
        } else if (have_damage) {
            (void)render_region(damage);
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
    return 0;
}
