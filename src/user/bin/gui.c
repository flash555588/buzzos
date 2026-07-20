#include "libc.h"
#include "guiapp.h"
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
    MAX_GUI_APPS = 3,
    WIN_COUNT = WIN_APP_BASE + MAX_GUI_APPS,

    MAX_APPS = 16,
    TERM_LINES = 256,
    TERM_COLS = 128,
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
    int want_w;
    int want_h;
    int resize_dirty;
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
static uint8_t app_pixels[MAX_GUI_APPS][APP_SURFACE_MAX_W * APP_SURFACE_MAX_H];
static int app_count;
static int app_selected;
static int app_last_click = -1;
static unsigned int app_last_click_tick;
static int dock_hover = -1;
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
static char term_lines[TERM_LINES][TERM_COLS + 1];
static int term_row;
static int term_col;
static volatile int term_lock;
static int term_in_fd = -1;
static int term_out_fd = -1;
static int term_pid = -1;
static int term_reader_tid = -1;
static int term_ansi_state;
static int term_ansi_param;
static unsigned int tick;
static unsigned int last_render_tick;
static volatile int desktop_dirty = 1;
static uint32_t last_wheel_seq;
static int last_wheel_value;

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
    if (r.w <= 0 || r.h <= 0)
        return;
    for (int yy = 0; yy < r.h; yy++) {
        uint8_t *row = fb + (r.y + yy) * sw + r.x;
        for (int xx = 0; xx < r.w; xx++)
            row[xx] = (uint8_t)color;
    }
}

static void pixel(int x, int y, int color) {
    if (x >= 0 && y >= 0 && x < sw && y < sh)
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

static void text_clip(int x, int y, const char *s, int fg, int bg, struct rect clip) {
    while (s && *s) {
        unsigned char ch = (unsigned char)*s++;
        if (ch == '\n') {
            y += KFONT_HEIGHT;
            x = clip.x;
            continue;
        }
        if (ch < KFONT_FIRST || ch >= KFONT_FIRST + KFONT_COUNT)
            ch = '?';
        if (x >= clip.x + clip.w)
            return;
        if (x + KFONT_WIDTH > clip.x && y + KFONT_HEIGHT > clip.y &&
            x < clip.x + clip.w && y < clip.y + clip.h) {
        const uint8_t *alpha = &kfont_alpha[ch - KFONT_FIRST][0][0];
        for (int py = 0; py < KFONT_HEIGHT; py++) {
            for (int px = 0; px < KFONT_WIDTH; px++) {
                uint8_t a = alpha[py * KFONT_WIDTH + px];
                if (a >= 128)
                    pixel_clip(x + px, y + py, fg, clip);
                else if (bg >= 0)
                    pixel_clip(x + px, y + py, bg, clip);
            }
        }
        }
        x += KFONT_WIDTH;
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

static void shadow(struct rect r) {
    fill((struct rect){r.x + 6, r.y + r.h, r.w, 6}, gray(2));
    fill((struct rect){r.x + r.w, r.y + 6, 6, r.h}, gray(2));
}

static void button(struct rect r, const char *label, int active) {
    int bg = active ? rgb6(1, 3, 5) : gray(4);
    fill(r, bg);
    border(r, active ? rgb6(3, 5, 5) : gray(8), gray(1));
    text_clip(r.x + 10, r.y + 5, label, 15, -1,
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
    for (int i = term_col; i < TERM_COLS; i++)
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
            if (term_col >= TERM_COLS) term_col = TERM_COLS - 1;
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
    if (term_col >= TERM_COLS - 1)
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
            copy_text(apps[app_count].name, ents[i].d_name, sizeof(apps[app_count].name));
            copy_text(apps[app_count].path, "/fs/apps/", sizeof(apps[app_count].path));
            append_text(apps[app_count].path, ents[i].d_name, sizeof(apps[app_count].path));
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

static int app_read_frame(int slot) {
    if (slot < 0 || slot >= MAX_GUI_APPS || !app_sessions[slot].used)
        return -1;
    struct guiapp_frame frame;
    if (read_full(app_sessions[slot].from_fd, &frame, (int)sizeof(frame)) < 0)
        return -1;
    if (frame.magic != GUIAPP_MAGIC || frame.width <= 0 || frame.height <= 0 ||
        frame.width > APP_SURFACE_MAX_W || frame.height > APP_SURFACE_MAX_H)
        return -1;
    if (frame.type == GUIAPP_FRAME_DIRTY) {
        if (frame.x < 0 || frame.y < 0 || frame.dirty_w <= 0 || frame.dirty_h <= 0 ||
            frame.x + frame.dirty_w > frame.width ||
            frame.y + frame.dirty_h > frame.height)
            return -1;
        if (app_sessions[slot].surface_w != frame.width ||
            app_sessions[slot].surface_h != frame.height)
            return -1;
        for (int row = 0; row < frame.dirty_h; row++) {
            uint8_t *dst = app_pixels[slot] +
                           (frame.y + row) * frame.width + frame.x;
            if (read_full(app_sessions[slot].from_fd, dst, frame.dirty_w) < 0)
                return -1;
        }
    } else {
        int bytes = frame.width * frame.height;
        if (read_full(app_sessions[slot].from_fd, app_pixels[slot], bytes) < 0)
            return -1;
    }
    app_sessions[slot].surface_w = frame.width;
    app_sessions[slot].surface_h = frame.height;
    copy_text(app_sessions[slot].title, frame.title, sizeof(app_sessions[slot].title));
    windows[WIN_APP_BASE + slot].title = app_sessions[slot].title[0]
        ? app_sessions[slot].title : "Application";
    clamp_scroll(WIN_APP_BASE + slot);
    desktop_dirty = 1;
    return 0;
}

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
    if (target_w == app_sessions[slot].want_w &&
        target_h == app_sessions[slot].want_h &&
        target_w == app_sessions[slot].surface_w &&
        target_h == app_sessions[slot].surface_h) {
        app_sessions[slot].resize_dirty = 0;
        return 0;
    }
    app_sessions[slot].want_w = target_w;
    app_sessions[slot].want_h = target_h;
    if (app_send_event(slot, GUIAPP_EVT_RESIZE, 0, 0, 0, 0, 0) < 0)
        return -1;
    if (app_read_frame(slot) < 0)
        return -1;
    app_sessions[slot].resize_dirty =
        (app_sessions[slot].surface_w != target_w ||
         app_sessions[slot].surface_h != target_h);
    scroll_x[id] = 0;
    scroll_y[id] = 0;
    return app_sessions[slot].resize_dirty ? -1 : 0;
}

static void run_app(const char *path) {
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

    int ev_pipe[2];
    int frame_pipe[2];
    if (pipe(ev_pipe) < 0 || pipe(frame_pipe) < 0) {
        term_log("pipe failed");
        return;
    }

    char ev_fd[12];
    char frame_fd[12];
    int_to_dec(ev_pipe[0], ev_fd, sizeof(ev_fd));
    int_to_dec(frame_pipe[1], frame_fd, sizeof(frame_fd));
    char *argv[4];
    argv[0] = (char *)path;
    argv[1] = "--buzz-gui";
    argv[2] = ev_fd;
    argv[3] = frame_fd;

    copy_text(msg, "launch ", sizeof(msg));
    append_text(msg, path, sizeof(msg));
    term_log(msg);
    int pid = spawn_process_args(path, argv, 4,
                                 SPAWN_FLAG_SILENT | SPAWN_FLAG_INHERIT_FDS);
    close(ev_pipe[0]);
    close(frame_pipe[1]);
    if (pid < 0) {
        close(ev_pipe[1]);
        close(frame_pipe[0]);
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
    copy_text(app_sessions[slot].title, "Application", sizeof(app_sessions[slot].title));

    if (app_send_event(slot, GUIAPP_EVT_INIT, 0, 0, 0, 0, 0) < 0 ||
        app_read_frame(slot) < 0) {
        term_log("app protocol failed");
        close_window(id);
        return;
    }

    copy_text(msg, "started pid ", sizeof(msg));
    append_uint(msg, (unsigned int)pid, sizeof(msg));
    term_log(msg);
    activate(id);
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
    fill((struct rect){0, 0, sw, sh}, rgb6(0, 2, 3));
}

static void draw_topbar(void) {
    fill((struct rect){0, 0, sw, 30}, gray(2));
    fill((struct rect){0, 29, sw, 1}, gray(7));
    text(14, 7, "BuzzOS", 15, -1);
    text(88, 7, "Desktop", gray(12), -1);
    text(sw - 178, 7, "Framebuffer 32bpp", gray(12), -1);
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
    fill(r, gray(3));
    border(r, w->active ? rgb6(2, 5, 5) : gray(8), gray(1));
    fill((struct rect){r.x + 1, r.y + 1, r.w - 2, 28},
         w->active ? rgb6(0, 3, 5) : gray(5));
    text_clip(r.x + 12, r.y + 7, w->title, 15, -1,
              (struct rect){r.x + 10, r.y + 4, r.w - 88, 22});
    struct rect mn = min_rect(id);
    struct rect mx = max_rect(id);
    struct rect cl = close_rect(id);
    fill(mn, gray(8));
    fill(mx, rgb6(2, 4, 3));
    fill(cl, rgb6(4, 1, 1));
    line_h(mn.x + 3, mn.y + 8, 6, 0);
    border(mx, 15, gray(1));
    line_h(cl.x + 3, cl.y + 3, 6, 15);
    line_h(cl.x + 3, cl.y + 8, 6, 15);
    if (!w->maximized) {
        for (int i = 0; i < 3; i++) {
            line_h(r.x + r.w - 18 + i * 5, r.y + r.h - 6 - i * 5,
                   12 - i * 4, gray(8));
        }
    }
}

static struct rect content_rect(int id) {
    struct rect r = windows[id].r;
    return (struct rect){r.x + 12, r.y + 40, r.w - 30, r.h - 70};
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
        return TERM_COLS * KFONT_WIDTH + 28;
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
    fill(vt, gray(2));
    fill(ht, gray(2));
    fill(vscroll_thumb(id), max_scroll_y(id) ? gray(8) : gray(4));
    fill(hscroll_thumb(id), max_scroll_x(id) ? gray(8) : gray(4));
}

static void draw_launcher(void) {
    if (!windows[WIN_LAUNCHER].visible || windows[WIN_LAUNCHER].minimized)
        return;
    draw_window_frame(WIN_LAUNCHER);
    struct rect c = content_rect(WIN_LAUNCHER);
    fill(c, gray(3));
    struct rect clip = c;
    int ox = c.x - scroll_x[WIN_LAUNCHER];
    int oy = c.y - scroll_y[WIN_LAUNCHER];
    text_clip(ox, oy, "Installed", gray(13), -1, clip);
    int y = oy + 28;
    if (app_count == 0) {
        text_clip(ox, y, "No apps in /fs/apps", gray(11), -1, clip);
        draw_scrollbars(WIN_LAUNCHER);
        return;
    }
    for (int i = 0; i < app_count; i++) {
        struct rect row = {ox, y + i * 34, content_width(WIN_LAUNCHER) - 20, 28};
        struct rect visible = intersect_rect(row, clip);
        if (visible.w <= 0 || visible.h <= 0)
            continue;
        int selected = i == app_selected;
        fill(visible, selected ? rgb6(0, 3, 5) : gray(4));
        border(visible, selected ? rgb6(2, 5, 5) : gray(6), gray(2));
        fill(intersect_rect((struct rect){row.x + 8, row.y + 6, 16, 16}, clip),
             rgb6((i % 5) + 1, 3, 4));
        text_clip(row.x + 34, row.y + 6, apps[i].name, 15, -1, clip);
    }
    draw_scrollbars(WIN_LAUNCHER);
}

static void draw_terminal(void) {
    if (!windows[WIN_TERMINAL].visible || windows[WIN_TERMINAL].minimized)
        return;
    draw_window_frame(WIN_TERMINAL);
    struct rect c = content_rect(WIN_TERMINAL);
    fill(c, 0);
    border(c, gray(6), gray(1));
    struct rect clip = {c.x + 1, c.y + 1, c.w - 2, c.h - 2};
    int ox = c.x + 10 - scroll_x[WIN_TERMINAL];
    int y = c.y + 8 - scroll_y[WIN_TERMINAL];
    term_lock_enter();
    for (int i = 0; i < TERM_LINES; i++) {
        if (term_lines[i][0])
            text_clip(ox, y, term_lines[i], rgb6(3, 5, 4), -1, clip);
        y += 22;
    }
    if ((tick / 30) & 1)
        fill(intersect_rect((struct rect){
            ox + term_col * KFONT_WIDTH,
            c.y + 8 + term_row * 22 - scroll_y[WIN_TERMINAL] + 4,
            8, 16
        }, clip), 15);
    term_lock_leave();
    draw_scrollbars(WIN_TERMINAL);
}

static void draw_status(void) {
    if (!windows[WIN_STATUS].visible || windows[WIN_STATUS].minimized)
        return;
    draw_window_frame(WIN_STATUS);
    struct rect c = content_rect(WIN_STATUS);
    fill(c, gray(3));
    struct rect clip = c;
    int ox = c.x - scroll_x[WIN_STATUS];
    int oy = c.y - scroll_y[WIN_STATUS];
    char line[96];
    text_clip(ox, oy, "Display", gray(13), -1, clip);
    copy_text(line, "resolution ", sizeof(line));
    append_uint(line, (unsigned int)sw, sizeof(line));
    append_text(line, " x ", sizeof(line));
    append_uint(line, (unsigned int)sh, sizeof(line));
    text_clip(ox, oy + 30, line, 15, -1, clip);
    copy_text(line, "apps ", sizeof(line));
    append_uint(line, (unsigned int)app_count, sizeof(line));
    text_clip(ox, oy + 58, line, 15, -1, clip);
    text_clip(ox, oy + 96, "Controls", gray(13), -1, clip);
    text_clip(ox, oy + 126, "Enter launches selected app", 15, -1, clip);
    text_clip(ox, oy + 150, "Tab cycles windows", 15, -1, clip);
    text_clip(ox, oy + 174, "Esc exits desktop", 15, -1, clip);
    draw_scrollbars(WIN_STATUS);
}

static void draw_app_window(int id) {
    int slot = app_slot_for_win(id);
    if (slot < 0 || !app_sessions[slot].used ||
        !windows[id].visible || windows[id].minimized)
        return;
    draw_window_frame(id);
    struct rect c = content_rect(id);
    fill(c, gray(2));
    struct rect clip = c;
    int ox = c.x;
    int oy = c.y;
    int aw = app_sessions[slot].surface_w;
    int ah = app_sessions[slot].surface_h;
    for (int y = 0; y < ah; y++) {
        int dy = oy + y;
        if (dy < clip.y || dy >= clip.y + clip.h)
            continue;
        for (int x = 0; x < aw; x++) {
            int dx = ox + x;
            if (dx < clip.x || dx >= clip.x + clip.w)
                continue;
            pixel(dx, dy, app_pixels[slot][y * aw + x]);
        }
    }
}

static void draw_dock(void) {
    int dock_w = min_i(18 + WIN_COUNT * 92 + 18, sw - 80);
    int dock_h = 54;
    int x = (sw - dock_w) / 2;
    int y = sh - dock_h - 12;
    struct rect r = {x, y, dock_w, dock_h};
    fill(r, gray(3));
    border(r, gray(8), gray(1));
    const char *labels[WIN_COUNT] = {"Apps", "Term", "Sys", "App1", "App2", "App3"};
    for (int i = 0; i < WIN_COUNT; i++) {
        struct rect b = {x + 18 + i * 92, y + 9, 76, 36};
        if (i >= WIN_APP_BASE && !windows[i].visible)
            continue;
        button(b, labels[i], windows[i].active || dock_hover == i);
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
            pixel(pointer_x + x, pointer_y + y, edge ? 0 : 15);
        }
    }
}

static void render(void) {
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
    draw_pointer();
    fb_blit(0, 0, sw, sh, fb);
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
        (void)app_send_event(slot, GUIAPP_EVT_CLOSE, 0, 0, 0, 0, 0);
        close(app_sessions[slot].to_fd);
        close(app_sessions[slot].from_fd);
        if (app_sessions[slot].pid > 0) {
            int status;
            (void)kill(app_sessions[slot].pid);
            (void)waitpid(app_sessions[slot].pid, &status, 0);
        }
        app_sessions[slot].used = 0;
        app_sessions[slot].pid = 0;
        app_sessions[slot].to_fd = -1;
        app_sessions[slot].from_fd = -1;
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

static void toggle_maximize(int id) {
    if (id < 0 || id >= WIN_COUNT)
        return;
    if (windows[id].maximized) {
        windows[id].r = windows[id].restore;
        windows[id].maximized = 0;
    } else {
        windows[id].restore = windows[id].r;
        windows[id].r = (struct rect){8, 34, sw - 16, sh - 46};
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
    int dock_w = min_i(18 + WIN_COUNT * 92 + 18, sw - 80);
    int dock_h = 54;
    int dx = (sw - dock_w) / 2;
    int dy = sh - dock_h - 12;
    for (int i = 0; i < WIN_COUNT; i++) {
        if (i >= WIN_APP_BASE && !windows[i].visible)
            continue;
        struct rect b = {dx + 18 + i * 92, dy + 9, 76, 36};
        if (inside(x, y, b))
            return i;
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
    if (app_send_event(slot, GUIAPP_EVT_MOUSE, x, y, 0, buttons, wheel) == 0)
        (void)app_read_frame(slot);
}

static void flush_pending_app_resizes(void) {
    if (resize_win >= WIN_APP_BASE)
        return;
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
        terminal_send("\b", 1);
    else if (k == '\r')
        terminal_send("\n", 1);
    else if (k >= 0 && k < 256) {
        ch = (char)k;
        terminal_send(&ch, 1);
    }
}

static void handle_key(int k) {
    if (k == KEY_ESC) {
        running = 0;
        return;
    }
    if (k == '\t') {
        activate((focus + 1) % WIN_COUNT);
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
        if (app_send_event(slot, GUIAPP_EVT_KEY, 0, 0, k, 0, 0) == 0)
            (void)app_read_frame(slot);
    }
}

static void handle_mouse(void) {
    struct mouse_state ms;
    if (mouse_get(&ms) < 0)
        return;
    if (ms.x != pointer_x || ms.y != pointer_y ||
        ms.buttons != prev_buttons || ms.wheel_seq != last_wheel_seq)
        desktop_dirty = 1;
    pointer_x = ms.x;
    pointer_y = ms.y;
    int left = ms.buttons & 1;
    dock_hover = hit_dock(pointer_x, pointer_y);

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
        if (dock_hover >= 0) {
            activate(dock_hover);
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
            (void)sync_app_size(finished_resize);
    }
    if (left && resize_win >= 0) {
        apply_resize(resize_win, pointer_x, pointer_y);
        if (resize_win >= WIN_APP_BASE)
            (void)sync_app_size(resize_win);
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
        prev_buttons = ms.buttons;
        return;
    }
    if (left && drag_win >= 0) {
        struct rect *r = &windows[drag_win].r;
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
    gfx_set_origin(0, 0);
    pointer_x = sw / 2;
    pointer_y = sh / 2;
    for (int i = 0; i < TERM_LINES; i++)
        term_lines[i][0] = 0;
    scan_apps();
    layout();
    scroll_y[WIN_TERMINAL] = max_scroll_y(WIN_TERMINAL);
    scroll_x[WIN_TERMINAL] = 0;
    if (start_terminal_shell() < 0)
        term_log("terminal: failed to start /bin/sh");
}

static void shutdown_desktop(void) {
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
    while (running) {
        int key;
        while ((key = read_key_poll()) >= 0) {
            desktop_dirty = 1;
            handle_key(key);
        }
        handle_mouse();
        flush_pending_app_resizes();
        if (desktop_dirty || tick - last_render_tick >= 60u) {
            desktop_dirty = 0;
            render();
            last_render_tick = tick;
        }
        tick++;
        sleep_ms(app_mouse_capture >= 0 ? 2 : 16);
    }
    shutdown_desktop();
    return 0;
}
