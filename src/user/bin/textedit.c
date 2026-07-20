#include "appui.h"
#include "guiapp.h"
#include "libc.h"

enum { MAX_W = GUIAPP_MAX_W, MAX_H = GUIAPP_MAX_H, TEXT_CAP = 4096 };

static uint8_t pixels[MAX_W * MAX_H];
static char textbuf[TEXT_CAP];
static int text_len;
static int cursor;
static int prev_buttons;
static int w = 560;
static int h = 360;
static int scroll_x;
static int scroll_y;
static int drag_scroll_axis = -1;
static int drag_mouse_start;
static int drag_scroll_start;
static char status[64] = "Ready";
static char file_path[GUIAPP_PATH_MAX] = "/fs/textedit.txt";
static char window_title[GUIAPP_TITLE_MAX] = "TextEdit";

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void set_status(const char *s) {
    appui_copy_text(status, s, sizeof(status));
}

static void set_path_status(const char *prefix) {
    appui_copy_text(status, prefix, sizeof(status));
    appui_append_text(status, file_path, sizeof(status));
}

static void set_document_path(const char *path) {
    if (!path || path[0] != '/')
        return;
    appui_copy_text(file_path, path, sizeof(file_path));
    const char *name = file_path;
    for (int i = 0; file_path[i]; i++)
        if (file_path[i] == '/' && file_path[i + 1])
            name = file_path + i + 1;
    appui_copy_text(window_title, "TextEdit - ", sizeof(window_title));
    appui_append_text(window_title, name, sizeof(window_title));
}

static struct appui_rect editor_rect(void) {
    return (struct appui_rect){12, 52, w - 34, h - 76};
}

static struct appui_rect text_clip_rect(void) {
    struct appui_rect e = editor_rect();
    return (struct appui_rect){e.x + 8, e.y + 8, e.w - 24, e.h - 24};
}

static void load_file(void) {
    int fd = open(file_path, O_RDONLY);
    if (fd < 0) {
        set_path_status("New ");
        return;
    }
    int n = read(fd, textbuf, TEXT_CAP - 1);
    close(fd);
    if (n < 0)
        n = 0;
    textbuf[n] = 0;
    text_len = n;
    cursor = text_len;
    set_path_status("Opened ");
}

static void save_file(void) {
    int fd = open(file_path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        set_status("Save failed");
        return;
    }
    int n = write(fd, textbuf, (size_t)text_len);
    close(fd);
    if (n == text_len)
        set_path_status("Saved ");
    else
        set_status("Save failed");
}

static void insert_char(char ch) {
    if (text_len >= TEXT_CAP - 1)
        return;
    for (int i = text_len; i > cursor; i--)
        textbuf[i] = textbuf[i - 1];
    textbuf[cursor++] = ch;
    text_len++;
    textbuf[text_len] = 0;
}

static void backspace(void) {
    if (cursor <= 0)
        return;
    for (int i = cursor - 1; i < text_len; i++)
        textbuf[i] = textbuf[i + 1];
    cursor--;
    text_len--;
}

static int line_start_of(int pos) {
    while (pos > 0 && textbuf[pos - 1] != '\n')
        pos--;
    return pos;
}

static int line_end_of(int pos) {
    while (pos < text_len && textbuf[pos] != '\n')
        pos++;
    return pos;
}

static void cursor_up(void) {
    int start = line_start_of(cursor);
    int col = cursor - start;
    if (start == 0)
        return;
    int prev_end = start - 1;
    int prev_start = line_start_of(prev_end);
    cursor = prev_start + appui_min(col, prev_end - prev_start);
}

static void cursor_down(void) {
    int start = line_start_of(cursor);
    int col = cursor - start;
    int end = line_end_of(cursor);
    if (end >= text_len)
        return;
    int next_start = end + 1;
    int next_end = line_end_of(next_start);
    cursor = next_start + appui_min(col, next_end - next_start);
}

static int max_line_len(void) {
    int best = 0;
    int cur = 0;
    for (int i = 0; i < text_len; i++) {
        if (textbuf[i] == '\n') {
            if (cur > best) best = cur;
            cur = 0;
        } else {
            cur++;
        }
    }
    if (cur > best) best = cur;
    return best;
}

static int line_count(void) {
    int lines = 1;
    for (int i = 0; i < text_len; i++)
        if (textbuf[i] == '\n')
            lines++;
    return lines;
}

static int content_w(void) {
    return max_line_len() * KFONT_WIDTH + 16;
}

static int content_h(void) {
    return line_count() * (KFONT_HEIGHT + 4) + 12;
}

static int max_scroll_x(void) {
    struct appui_rect c = text_clip_rect();
    return appui_max(0, content_w() - c.w);
}

static int max_scroll_y(void) {
    struct appui_rect c = text_clip_rect();
    return appui_max(0, content_h() - c.h);
}

static void clamp_scrolls(void) {
    scroll_x = clamp_int(scroll_x, 0, max_scroll_x());
    scroll_y = clamp_int(scroll_y, 0, max_scroll_y());
}

static void cursor_xy(int *x_out, int *y_out) {
    int x = 0;
    int y = 0;
    for (int i = 0; i < cursor && i < text_len; i++) {
        if (textbuf[i] == '\n') {
            x = 0;
            y += KFONT_HEIGHT + 4;
        } else {
            x += KFONT_WIDTH;
        }
    }
    *x_out = x;
    *y_out = y;
}

static void ensure_cursor_visible(void) {
    struct appui_rect c = text_clip_rect();
    int cx, cy;
    cursor_xy(&cx, &cy);
    if (cx < scroll_x)
        scroll_x = cx;
    if (cx + KFONT_WIDTH > scroll_x + c.w)
        scroll_x = cx + KFONT_WIDTH - c.w;
    if (cy < scroll_y)
        scroll_y = cy;
    if (cy + KFONT_HEIGHT > scroll_y + c.h)
        scroll_y = cy + KFONT_HEIGHT - c.h;
    clamp_scrolls();
}

static struct appui_rect vtrack(void) {
    struct appui_rect e = editor_rect();
    return (struct appui_rect){e.x + e.w - 11, e.y + 1, 10, e.h - 14};
}

static struct appui_rect htrack(void) {
    struct appui_rect e = editor_rect();
    return (struct appui_rect){e.x + 1, e.y + e.h - 11, e.w - 14, 10};
}

static struct appui_rect vthumb(void) {
    struct appui_rect t = vtrack();
    int maxs = max_scroll_y();
    if (maxs <= 0)
        return t;
    int th = appui_max(22, t.h * t.h / appui_max(t.h, content_h()));
    if (th > t.h) th = t.h;
    return (struct appui_rect){t.x, t.y + scroll_y * (t.h - th) / maxs, t.w, th};
}

static struct appui_rect hthumb(void) {
    struct appui_rect t = htrack();
    int maxs = max_scroll_x();
    if (maxs <= 0)
        return t;
    int tw = appui_max(28, t.w * t.w / appui_max(t.w, content_w()));
    if (tw > t.w) tw = t.w;
    return (struct appui_rect){t.x + scroll_x * (t.w - tw) / maxs, t.y, tw, t.h};
}

static void draw_scrollbars(void) {
    appui_fill(pixels, w, h, vtrack(), appui_gray(2));
    appui_fill(pixels, w, h, htrack(), appui_gray(2));
    appui_fill(pixels, w, h, vthumb(), max_scroll_y() ? appui_gray(8) : appui_gray(4));
    appui_fill(pixels, w, h, hthumb(), max_scroll_x() ? appui_gray(8) : appui_gray(4));
}

static void render(void) {
    clamp_scrolls();
    appui_fill(pixels, w, h, (struct appui_rect){0, 0, w, h}, appui_gray(3));
    appui_fill(pixels, w, h, (struct appui_rect){0, 0, w, 42}, appui_gray(2));
    appui_button(pixels, w, h, (struct appui_rect){10, 8, 70, 26}, "Open", 0);
    appui_button(pixels, w, h, (struct appui_rect){88, 8, 70, 26}, "Save", 0);
    appui_button(pixels, w, h, (struct appui_rect){166, 8, 70, 26}, "Clear", 0);
    appui_text(pixels, w, h, 250, 13, status, appui_gray(12), -1,
               (struct appui_rect){250, 8, w - 260, 28});

    struct appui_rect editor = editor_rect();
    appui_fill(pixels, w, h, editor, 15);
    appui_border(pixels, w, h, editor, appui_gray(8), appui_gray(1));
    struct appui_rect clip = text_clip_rect();
    int x = clip.x - scroll_x;
    int y = clip.y - scroll_y;
    int cur_x = x;
    int cur_y = y;
    for (int i = 0; i <= text_len; i++) {
        if (i == cursor) {
            cur_x = x;
            cur_y = y;
        }
        if (i == text_len)
            break;
        char ch = textbuf[i];
        if (ch == '\n') {
            x = clip.x - scroll_x;
            y += KFONT_HEIGHT + 4;
            continue;
        }
        if (y + KFONT_HEIGHT >= clip.y && y < clip.y + clip.h &&
            x + KFONT_WIDTH >= clip.x && x < clip.x + clip.w) {
            char s[2] = { ch, 0 };
            appui_text(pixels, w, h, x, y, s, 0, -1, clip);
        }
        x += KFONT_WIDTH;
    }
    appui_fill(pixels, w, h, (struct appui_rect){cur_x, cur_y, 2, KFONT_HEIGHT + 2},
               appui_rgb6(0, 2, 5));
    draw_scrollbars();
}

static void click(int x, int y) {
    if (appui_inside(x, y, (struct appui_rect){10, 8, 70, 26}))
        load_file();
    else if (appui_inside(x, y, (struct appui_rect){88, 8, 70, 26}))
        save_file();
    else if (appui_inside(x, y, (struct appui_rect){166, 8, 70, 26})) {
        text_len = 0;
        cursor = 0;
        textbuf[0] = 0;
        set_status("Cleared");
    }
}

static void mouse(int x, int y, int buttons, int wheel) {
    int pressed = (buttons & 1) && !(prev_buttons & 1);
    if (wheel)
        scroll_y -= wheel * 40;
    if (pressed) {
        if (max_scroll_y() > 0 && appui_inside(x, y, vtrack())) {
            struct appui_rect t = vtrack();
            struct appui_rect th = vthumb();
            if (!appui_inside(x, y, th)) {
                int span = appui_max(1, t.h - th.h);
                scroll_y = (y - t.y - th.h / 2) * max_scroll_y() / span;
                clamp_scrolls();
            }
            drag_scroll_axis = 1;
            drag_mouse_start = y;
            drag_scroll_start = scroll_y;
        } else if (max_scroll_x() > 0 && appui_inside(x, y, htrack())) {
            struct appui_rect t = htrack();
            struct appui_rect th = hthumb();
            if (!appui_inside(x, y, th)) {
                int span = appui_max(1, t.w - th.w);
                scroll_x = (x - t.x - th.w / 2) * max_scroll_x() / span;
                clamp_scrolls();
            }
            drag_scroll_axis = 0;
            drag_mouse_start = x;
            drag_scroll_start = scroll_x;
        } else {
            click(x, y);
        }
    }
    if ((buttons & 1) && drag_scroll_axis >= 0) {
        if (drag_scroll_axis) {
            struct appui_rect t = vtrack();
            struct appui_rect th = vthumb();
            int span = appui_max(1, t.h - th.h);
            scroll_y = drag_scroll_start + (y - drag_mouse_start) * max_scroll_y() / span;
        } else {
            struct appui_rect t = htrack();
            struct appui_rect th = hthumb();
            int span = appui_max(1, t.w - th.w);
            scroll_x = drag_scroll_start + (x - drag_mouse_start) * max_scroll_x() / span;
        }
    }
    if (!(buttons & 1))
        drag_scroll_axis = -1;
    prev_buttons = buttons;
    clamp_scrolls();
}

static void key(int k) {
    if (k == GUIAPP_KEY_BACKSPACE || k == 127)
        backspace();
    else if (k == GUIAPP_KEY_LEFT && cursor > 0)
        cursor--;
    else if (k == GUIAPP_KEY_RIGHT && cursor < text_len)
        cursor++;
    else if (k == GUIAPP_KEY_UP)
        cursor_up();
    else if (k == GUIAPP_KEY_DOWN)
        cursor_down();
    else if (k == '\r' || k == '\n')
        insert_char('\n');
    else if (k >= 32 && k < 127)
        insert_char((char)k);
    ensure_cursor_visible();
}

int main(int argc, char **argv) {
    struct guiapp_ctx ctx;
    struct guiapp_event ev;
    if (guiapp_parse_args(argc, argv, &ctx) < 0)
        return 1;
    if (argc > 4)
        set_document_path(argv[4]);
    load_file();
    for (;;) {
        if (guiapp_read_event(&ctx, &ev) < 0 || ev.type == GUIAPP_EVT_CLOSE)
            break;
        if (ev.type == GUIAPP_EVT_INIT || ev.type == GUIAPP_EVT_RESIZE) {
            w = clamp_int(ev.width, 220, MAX_W);
            h = clamp_int(ev.height, 160, MAX_H);
            ensure_cursor_visible();
        } else if (ev.type == GUIAPP_EVT_KEY) {
            key(ev.key);
        } else if (ev.type == GUIAPP_EVT_MOUSE) {
            mouse(ev.x, ev.y, ev.buttons, ev.wheel);
        }
        render();
        if (guiapp_send_frame(&ctx, window_title, w, h, pixels) < 0)
            break;
    }
    return 0;
}
