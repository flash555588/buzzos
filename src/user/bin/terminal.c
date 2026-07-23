#include "appui.h"
#include "guiapp.h"
#include "libc.h"

enum {
    TERM_HISTORY_LINES = 256,
    TERM_LINE_BYTES = 512,
    TERM_MARGIN_X = 10,
    TERM_MARGIN_Y = 8,
    TERM_LINE_HEIGHT = KFONT_HEIGHT,
    TERM_SCROLLBAR_SIZE = 14,
    TERM_SCROLLBAR_MIN_THUMB = 22,
};

static struct guiapp_ctx gui;
static char lines[TERM_HISTORY_LINES][TERM_LINE_BYTES];
static int first_line;
static int line_count = 1;
static int cursor_line;
static int cursor_col;
static int scroll_x;
static int scroll_y;
static int view_width = 640;
static int view_height = 400;
static uint8_t *pixels;
static size_t pixel_capacity;
static volatile int terminal_lock;
static volatile int stopping;
static int shell_pid = -1;
static int shell_input = -1;
static int shell_output = -1;
static int reader_tid = -1;
static int ansi_state;
static int ansi_params[4];
static int ansi_param;
static int ansi_digit;
static char input_line[GUIAPP_PATH_MAX];
static int input_length;
static int previous_buttons;
static int selection_dragging;
static int selection_anchor_line;
static int selection_anchor_pos;
static int selection_focus_line;
static int selection_focus_pos;
static int scrollbar_drag_axis;
static int scrollbar_drag_mouse;
static int scrollbar_drag_scroll;

struct term_position {
    int line;
    int pos;
};

static void lock_terminal(void) {
    while (__sync_lock_test_and_set(&terminal_lock, 1))
        yield();
}

static void unlock_terminal(void) {
    __sync_lock_release(&terminal_lock);
}

static int clamp_int(int value, int low, int high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static char *line_at(int logical_line) {
    return lines[(first_line + logical_line) % TERM_HISTORY_LINES];
}

static void clear_line(int logical_line) {
    memset(line_at(logical_line), 0, TERM_LINE_BYTES);
}

static void reset_terminal_locked(void) {
    memset(lines, 0, sizeof(lines));
    first_line = 0;
    line_count = 1;
    cursor_line = 0;
    cursor_col = 0;
    scroll_x = 0;
    scroll_y = 0;
    selection_dragging = 0;
    selection_anchor_line = 0;
    selection_anchor_pos = 0;
    selection_focus_line = 0;
    selection_focus_pos = 0;
    scrollbar_drag_axis = 0;
}

static struct appui_rect viewport_locked(void) {
    return (struct appui_rect){
        0, 0,
        view_width - TERM_SCROLLBAR_SIZE,
        view_height - TERM_SCROLLBAR_SIZE
    };
}

static int visible_rows_locked(void) {
    struct appui_rect viewport = viewport_locked();
    int rows = (viewport.h - TERM_MARGIN_Y * 2) / TERM_LINE_HEIGHT;
    return rows > 0 ? rows : 1;
}

static int content_width_locked(void) {
    int width = 0;
    for (int line = 0; line < line_count; line++) {
        int line_width = appui_text_width(line_at(line));
        if (line_width > width)
            width = line_width;
    }
    return width + TERM_MARGIN_X * 2;
}

static int content_height_locked(void) {
    return line_count * TERM_LINE_HEIGHT + TERM_MARGIN_Y * 2;
}

static int max_scroll_x_locked(void) {
    struct appui_rect viewport = viewport_locked();
    int maximum = content_width_locked() - viewport.w;
    return maximum > 0 ? maximum : 0;
}

static int max_scroll_y_locked(void) {
    struct appui_rect viewport = viewport_locked();
    int maximum = content_height_locked() - viewport.h;
    return maximum > 0 ? maximum : 0;
}

static void clamp_scroll_locked(void) {
    scroll_x = clamp_int(scroll_x, 0, max_scroll_x_locked());
    scroll_y = clamp_int(scroll_y, 0, max_scroll_y_locked());
}

static int selection_exists_locked(void) {
    return selection_anchor_line != selection_focus_line ||
           selection_anchor_pos != selection_focus_pos;
}

static void clear_selection_locked(void) {
    selection_anchor_line = cursor_line;
    selection_anchor_pos = cursor_col;
    selection_focus_line = cursor_line;
    selection_focus_pos = cursor_col;
    selection_dragging = 0;
}

static int compare_position(struct term_position a, struct term_position b) {
    if (a.line != b.line)
        return a.line < b.line ? -1 : 1;
    if (a.pos != b.pos)
        return a.pos < b.pos ? -1 : 1;
    return 0;
}

static void selection_range_locked(struct term_position *start,
                                   struct term_position *end) {
    struct term_position anchor = {
        selection_anchor_line, selection_anchor_pos
    };
    struct term_position focus = {
        selection_focus_line, selection_focus_pos
    };
    if (compare_position(anchor, focus) <= 0) {
        *start = anchor;
        *end = focus;
    } else {
        *start = focus;
        *end = anchor;
    }
}

static int line_width_to_pos_locked(int logical_line, int pos) {
    const char *line = line_at(logical_line);
    const char *cursor = line;
    int width = 0;
    int length = (int)strlen(line);
    pos = clamp_int(pos, 0, length);
    while (*cursor && (int)(cursor - line) < pos) {
        uint32_t codepoint = appui_utf8_next(&cursor);
        width += appui_codepoint_width(codepoint);
    }
    return width;
}

static struct appui_rect scrollbar_track_locked(int axis) {
    struct appui_rect viewport = viewport_locked();
    if (axis == 1) {
        return (struct appui_rect){
            viewport.w, 0, TERM_SCROLLBAR_SIZE, viewport.h
        };
    }
    return (struct appui_rect){
        0, viewport.h, viewport.w, TERM_SCROLLBAR_SIZE
    };
}

static struct appui_rect scrollbar_lane_locked(int axis) {
    struct appui_rect track = scrollbar_track_locked(axis);
    return (struct appui_rect){
        track.x + 2, track.y + 2, track.w - 4, track.h - 4
    };
}

static struct appui_rect scrollbar_thumb_locked(int axis) {
    struct appui_rect viewport = viewport_locked();
    struct appui_rect lane = scrollbar_lane_locked(axis);
    int track_length = axis == 1 ? lane.h : lane.w;
    int visible = axis == 1 ? viewport.h : viewport.w;
    int content = axis == 1 ? content_height_locked() : content_width_locked();
    int maximum = axis == 1 ? max_scroll_y_locked() : max_scroll_x_locked();
    int scroll = axis == 1 ? scroll_y : scroll_x;
    int thumb_length = track_length;
    int thumb_offset = 0;
    if (content > visible && track_length > 0) {
        thumb_length = visible * track_length / content;
        thumb_length = clamp_int(thumb_length, TERM_SCROLLBAR_MIN_THUMB,
                                 track_length);
        int span = track_length - thumb_length;
        if (maximum > 0 && span > 0)
            thumb_offset = scroll * span / maximum;
    }
    if (axis == 1) {
        return (struct appui_rect){
            lane.x, lane.y + thumb_offset, lane.w, thumb_length
        };
    }
    return (struct appui_rect){
        lane.x + thumb_offset, lane.y, thumb_length, lane.h
    };
}

static void append_line_locked(void) {
    cursor_col = 0;
    if (cursor_line + 1 < line_count) {
        cursor_line++;
        clear_line(cursor_line);
        return;
    }
    if (line_count < TERM_HISTORY_LINES) {
        cursor_line = line_count++;
        clear_line(cursor_line);
        return;
    }
    first_line = (first_line + 1) % TERM_HISTORY_LINES;
    cursor_line = TERM_HISTORY_LINES - 1;
    clear_line(cursor_line);
    if (scroll_y >= TERM_LINE_HEIGHT)
        scroll_y -= TERM_LINE_HEIGHT;
    else
        scroll_y = 0;
    clear_selection_locked();
}

static void erase_line_locked(int mode) {
    char *line = line_at(cursor_line);
    int length = (int)strlen(line);
    if (mode == 2) {
        memset(line, 0, TERM_LINE_BYTES);
        return;
    }
    if (mode == 1) {
        int end = cursor_col < TERM_LINE_BYTES - 1
            ? cursor_col : TERM_LINE_BYTES - 2;
        for (int i = 0; i <= end; i++)
            line[i] = ' ';
        if (length <= end)
            line[end + 1] = 0;
        return;
    }
    if (cursor_col < TERM_LINE_BYTES)
        memset(line + cursor_col, 0, (size_t)(TERM_LINE_BYTES - cursor_col));
}

static int ansi_value(int index, int fallback) {
    if (index < 0 || index >= 4 || ansi_params[index] == 0)
        return fallback;
    return ansi_params[index];
}

static void finish_ansi_locked(char command) {
    int amount = ansi_value(0, 1);
    switch (command) {
    case 'A':
        cursor_line = clamp_int(cursor_line - amount, 0, line_count - 1);
        break;
    case 'B':
        cursor_line = clamp_int(cursor_line + amount, 0, line_count - 1);
        break;
    case 'C':
        cursor_col = clamp_int(cursor_col + amount, 0, TERM_LINE_BYTES - 2);
        break;
    case 'D':
        cursor_col = clamp_int(cursor_col - amount, 0, TERM_LINE_BYTES - 2);
        break;
    case 'G':
        cursor_col = clamp_int(ansi_value(0, 1) - 1, 0, TERM_LINE_BYTES - 2);
        break;
    case 'H':
    case 'f': {
        int rows = visible_rows_locked();
        int screen_top = line_count > rows ? line_count - rows : 0;
        cursor_line = clamp_int(screen_top + ansi_value(0, 1) - 1,
                                0, line_count - 1);
        cursor_col = clamp_int(ansi_value(1, 1) - 1,
                               0, TERM_LINE_BYTES - 2);
        break;
    }
    case 'J':
        if (ansi_params[0] == 2 || !ansi_digit)
            reset_terminal_locked();
        break;
    case 'K':
        erase_line_locked(ansi_params[0]);
        break;
    default:
        break;
    }
    ansi_state = 0;
}

static int consume_ansi_locked(unsigned char character) {
    if (ansi_state == 0) {
        if (character == 0x1Bu) {
            ansi_state = 1;
            return 1;
        }
        return 0;
    }
    if (ansi_state == 1) {
        if (character == '[') {
            ansi_state = 2;
            memset(ansi_params, 0, sizeof(ansi_params));
            ansi_param = 0;
            ansi_digit = 0;
        } else {
            ansi_state = 0;
        }
        return 1;
    }
    if (character >= '0' && character <= '9') {
        if (ansi_param < 4) {
            ansi_params[ansi_param] =
                ansi_params[ansi_param] * 10 + (character - '0');
        }
        ansi_digit = 1;
        return 1;
    }
    if (character == ';') {
        if (ansi_param < 3)
            ansi_param++;
        return 1;
    }
    if (character == '?' || character == '>')
        return 1;
    finish_ansi_locked((char)character);
    return 1;
}

static void put_byte_locked(unsigned char character) {
    if (consume_ansi_locked(character))
        return;
    if (character == '\r') {
        cursor_col = 0;
        return;
    }
    if (character == '\n') {
        append_line_locked();
        return;
    }
    if (character == '\b' || character == 127) {
        if (cursor_col > 0)
            cursor_col--;
        return;
    }
    if (character == '\t') {
        int spaces = 8 - (cursor_col & 7);
        while (spaces-- > 0)
            put_byte_locked(' ');
        return;
    }
    if (character < 32)
        return;

    if (cursor_col >= TERM_LINE_BYTES - 2)
        append_line_locked();
    char *line = line_at(cursor_line);
    line[cursor_col++] = (char)character;
    if (!line[cursor_col])
        line[cursor_col] = 0;
}

static int ensure_pixels_locked(void) {
    size_t needed = (size_t)view_width * (size_t)view_height;
    if (needed <= pixel_capacity)
        return 0;
    uint8_t *replacement = realloc(pixels, needed);
    if (!replacement)
        return -1;
    pixels = replacement;
    pixel_capacity = needed;
    return 0;
}

static void fill_clipped_locked(struct appui_rect rect,
                                struct appui_rect clip, int color) {
    int right = appui_min(rect.x + rect.w, clip.x + clip.w);
    int bottom = appui_min(rect.y + rect.h, clip.y + clip.h);
    rect.x = appui_max(rect.x, clip.x);
    rect.y = appui_max(rect.y, clip.y);
    rect.w = right - rect.x;
    rect.h = bottom - rect.y;
    if (rect.w > 0 && rect.h > 0)
        appui_fill(pixels, view_width, view_height, rect, color);
}

static void draw_line_locked(int logical_line, int x, int y,
                             struct appui_rect clip, int foreground,
                             int selected_foreground,
                             int selection_background) {
    const char *line = line_at(logical_line);
    const char *cursor = line;
    struct term_position selection_start;
    struct term_position selection_end;
    int has_selection = selection_exists_locked();
    if (has_selection)
        selection_range_locked(&selection_start, &selection_end);

    while (*cursor) {
        int byte_start = (int)(cursor - line);
        const char *next = cursor;
        uint32_t codepoint = appui_utf8_next(&next);
        int byte_end = (int)(next - line);
        int glyph_width = appui_codepoint_width(codepoint);
        int selected = 0;
        if (has_selection) {
            struct term_position glyph_start = {
                logical_line, byte_start
            };
            struct term_position glyph_end = {
                logical_line, byte_end
            };
            selected =
                compare_position(glyph_end, selection_start) > 0 &&
                compare_position(glyph_start, selection_end) < 0;
        }
        if (selected) {
            fill_clipped_locked(
                (struct appui_rect){
                    x, y - PLT_FONT_Y_SHIFT,
                    glyph_width, TERM_LINE_HEIGHT
                },
                clip, selection_background);
        }
        (void)appui_draw_codepoint(
            pixels, view_width, view_height, x, y, codepoint,
            selected ? selected_foreground : foreground, -1, clip);
        x += glyph_width;
        cursor = next;
        if (x >= clip.x + clip.w)
            break;
    }
    if (has_selection && logical_line >= selection_start.line &&
        logical_line < selection_end.line) {
        fill_clipped_locked(
            (struct appui_rect){
                x, y - PLT_FONT_Y_SHIFT,
                KFONT_WIDTH, TERM_LINE_HEIGHT
            },
            clip, selection_background);
    }
}

static void draw_scrollbars_locked(void) {
    struct appui_rect viewport = viewport_locked();
    struct appui_rect vertical = scrollbar_track_locked(1);
    struct appui_rect horizontal = scrollbar_track_locked(2);
    struct appui_rect vertical_thumb = scrollbar_thumb_locked(1);
    struct appui_rect horizontal_thumb = scrollbar_thumb_locked(2);
    int track_color = plt_rgb(27, 32, 42);
    int thumb_color = plt_rgb(91, 104, 121);
    int active_color = plt_rgb(128, 147, 171);

    appui_fill(pixels, view_width, view_height, vertical, track_color);
    appui_fill(pixels, view_width, view_height, horizontal, track_color);
    appui_fill(pixels, view_width, view_height,
               (struct appui_rect){
                   viewport.w, viewport.h,
                   TERM_SCROLLBAR_SIZE, TERM_SCROLLBAR_SIZE
               },
               track_color);
    appui_fill_round(pixels, view_width, view_height, vertical_thumb,
                     scrollbar_drag_axis == 1 ? active_color : thumb_color);
    appui_fill_round(pixels, view_width, view_height, horizontal_thumb,
                     scrollbar_drag_axis == 2 ? active_color : thumb_color);
}

static void render_locked(void) {
    if (ensure_pixels_locked() < 0)
        return;
    struct appui_rect full = {0, 0, view_width, view_height};
    struct appui_rect clip = viewport_locked();
    int background = plt_rgb(14, 18, 26);
    int foreground = appui_rgb6(3, 5, 4);
    int selected_foreground = plt_rgb(244, 248, 252);
    int selection_background = plt_rgb(45, 91, 140);
    appui_fill(pixels, view_width, view_height, full, background);

    int first_visible = (scroll_y - TERM_MARGIN_Y) / TERM_LINE_HEIGHT;
    if (first_visible < 0)
        first_visible = 0;
    int last_visible =
        (scroll_y + clip.h - TERM_MARGIN_Y + TERM_LINE_HEIGHT - 1) /
        TERM_LINE_HEIGHT;
    if (last_visible >= line_count)
        last_visible = line_count - 1;
    int text_x = TERM_MARGIN_X - scroll_x;
    for (int row = first_visible; row <= last_visible; row++) {
        int y = TERM_MARGIN_Y + row * TERM_LINE_HEIGHT - scroll_y;
        draw_line_locked(row, text_x, y, clip, foreground,
                         selected_foreground, selection_background);
    }

    int cursor_x = TERM_MARGIN_X - scroll_x +
        line_width_to_pos_locked(cursor_line, cursor_col);
    int cursor_y = TERM_MARGIN_Y + cursor_line * TERM_LINE_HEIGHT -
        scroll_y + 3;
    if (cursor_x < clip.x + clip.w && cursor_x + 8 > clip.x &&
        cursor_y < clip.y + clip.h && cursor_y + 16 > clip.y) {
        fill_clipped_locked(
            (struct appui_rect){cursor_x, cursor_y, 8, 16},
            clip, 15);
    }
    draw_scrollbars_locked();
    (void)guiapp_send_frame(&gui, "Terminal", view_width, view_height, pixels);
}

static struct term_position position_from_mouse_locked(int x, int y) {
    struct term_position result = {0, 0};
    int content_y = y + scroll_y - TERM_MARGIN_Y;
    if (content_y > 0)
        result.line = content_y / TERM_LINE_HEIGHT;
    result.line = clamp_int(result.line, 0, line_count - 1);

    const char *line = line_at(result.line);
    const char *cursor = line;
    int content_x = x + scroll_x - TERM_MARGIN_X;
    int glyph_x = 0;
    if (content_x <= 0)
        return result;
    while (*cursor) {
        const char *next = cursor;
        uint32_t codepoint = appui_utf8_next(&next);
        int glyph_width = appui_codepoint_width(codepoint);
        if (content_x < glyph_x + (glyph_width + 1) / 2) {
            result.pos = (int)(cursor - line);
            return result;
        }
        glyph_x += glyph_width;
        cursor = next;
        result.pos = (int)(cursor - line);
        if (content_x < glyph_x)
            return result;
    }
    result.pos = (int)strlen(line);
    return result;
}

static int copy_selection_locked(char *output, int capacity) {
    struct term_position start;
    struct term_position end;
    int length = 0;
    if (!output || capacity <= 0)
        return 0;
    output[0] = 0;
    if (!selection_exists_locked())
        return 0;
    selection_range_locked(&start, &end);
    start.line = clamp_int(start.line, 0, line_count - 1);
    end.line = clamp_int(end.line, 0, line_count - 1);

    for (int logical_line = start.line;
         logical_line <= end.line && length + 1 < capacity;
         logical_line++) {
        const char *line = line_at(logical_line);
        int line_length = (int)strlen(line);
        int from = logical_line == start.line ? start.pos : 0;
        int to = logical_line == end.line ? end.pos : line_length;
        from = clamp_int(from, 0, line_length);
        to = clamp_int(to, from, line_length);
        const char *cursor = line + from;
        const char *limit = line + to;
        while (cursor < limit && *cursor) {
            const char *next = cursor;
            (void)appui_utf8_next(&next);
            int bytes = (int)(next - cursor);
            if (next > limit || length + bytes + 1 > capacity)
                break;
            memcpy(output + length, cursor, (size_t)bytes);
            length += bytes;
            cursor = next;
        }
        if (logical_line < end.line && length + 2 <= capacity)
            output[length++] = '\n';
    }
    output[length] = 0;
    return length;
}

static void begin_scrollbar_drag_locked(int axis, int x, int y) {
    struct appui_rect thumb = scrollbar_thumb_locked(axis);
    struct appui_rect lane = scrollbar_lane_locked(axis);
    int mouse = axis == 1 ? y : x;
    int inside_thumb = appui_inside(x, y, thumb);
    int thumb_length = axis == 1 ? thumb.h : thumb.w;
    int lane_start = axis == 1 ? lane.y : lane.x;
    int lane_length = axis == 1 ? lane.h : lane.w;
    int maximum = axis == 1 ? max_scroll_y_locked() : max_scroll_x_locked();

    if (!inside_thumb && maximum > 0) {
        int span = lane_length - thumb_length;
        int offset = mouse - lane_start - thumb_length / 2;
        int value = span > 0 ? offset * maximum / span : 0;
        if (axis == 1)
            scroll_y = clamp_int(value, 0, maximum);
        else
            scroll_x = clamp_int(value, 0, maximum);
    }
    scrollbar_drag_axis = axis;
    scrollbar_drag_mouse = mouse;
    scrollbar_drag_scroll = axis == 1 ? scroll_y : scroll_x;
}

static void update_scrollbar_drag_locked(int x, int y) {
    int axis = scrollbar_drag_axis;
    if (!axis)
        return;
    struct appui_rect thumb = scrollbar_thumb_locked(axis);
    struct appui_rect lane = scrollbar_lane_locked(axis);
    int mouse = axis == 1 ? y : x;
    int thumb_length = axis == 1 ? thumb.h : thumb.w;
    int lane_length = axis == 1 ? lane.h : lane.w;
    int maximum = axis == 1 ? max_scroll_y_locked() : max_scroll_x_locked();
    int span = lane_length - thumb_length;
    int value = scrollbar_drag_scroll;
    if (span > 0 && maximum > 0) {
        value += (mouse - scrollbar_drag_mouse) * maximum / span;
        value = clamp_int(value, 0, maximum);
    } else {
        value = 0;
    }
    if (axis == 1)
        scroll_y = value;
    else
        scroll_x = value;
}

static void update_selection_drag_locked(int x, int y) {
    struct appui_rect viewport = viewport_locked();
    if (y < viewport.y)
        scroll_y -= TERM_LINE_HEIGHT;
    else if (y >= viewport.y + viewport.h)
        scroll_y += TERM_LINE_HEIGHT;
    if (x < viewport.x)
        scroll_x -= KFONT_WIDTH * 3;
    else if (x >= viewport.x + viewport.w)
        scroll_x += KFONT_WIDTH * 3;
    clamp_scroll_locked();
    struct term_position focus = position_from_mouse_locked(x, y);
    selection_focus_line = focus.line;
    selection_focus_pos = focus.pos;
}

static void handle_mouse_locked(const struct guiapp_event *event) {
    struct appui_rect viewport = viewport_locked();
    struct appui_rect vertical = scrollbar_track_locked(1);
    struct appui_rect horizontal = scrollbar_track_locked(2);
    int left = (event->buttons & 1) != 0;
    int left_pressed = left && !(previous_buttons & 1);

    if (event->wheel) {
        scroll_y -= event->wheel * TERM_LINE_HEIGHT * 3;
        clamp_scroll_locked();
    }
    if (left_pressed) {
        if (appui_inside(event->x, event->y, vertical)) {
            selection_dragging = 0;
            begin_scrollbar_drag_locked(1, event->x, event->y);
        } else if (appui_inside(event->x, event->y, horizontal)) {
            selection_dragging = 0;
            begin_scrollbar_drag_locked(2, event->x, event->y);
        } else if (appui_inside(event->x, event->y, viewport)) {
            struct term_position position =
                position_from_mouse_locked(event->x, event->y);
            selection_anchor_line = position.line;
            selection_anchor_pos = position.pos;
            selection_focus_line = position.line;
            selection_focus_pos = position.pos;
            selection_dragging = 1;
            scrollbar_drag_axis = 0;
        }
    } else if (left && scrollbar_drag_axis) {
        update_scrollbar_drag_locked(event->x, event->y);
    } else if (left && selection_dragging) {
        update_selection_drag_locked(event->x, event->y);
    }
    if (!left) {
        if (selection_dragging)
            update_selection_drag_locked(event->x, event->y);
        selection_dragging = 0;
        scrollbar_drag_axis = 0;
    }
    previous_buttons = event->buttons;
}

static int write_all(int fd, const char *text, int length) {
    int done = 0;
    while (done < length) {
        int written = write(fd, text + done, (size_t)(length - done));
        if (written <= 0)
            return -1;
        done += written;
    }
    return 0;
}

static void send_shell(const char *text, int length) {
    if (shell_input >= 0 && text && length > 0)
        (void)write_all(shell_input, text, length);
}

static void track_input_text(const char *text) {
    while (text && *text && input_length + 1 < (int)sizeof(input_line))
        input_line[input_length++] = *text++;
    input_line[input_length] = 0;
}

static void track_input_backspace(void) {
    if (input_length <= 0)
        return;
    input_length = appui_utf8_prev(input_line, input_length);
    input_line[input_length] = 0;
}

static void send_key(int key) {
    char character;
    if (key == GUIAPP_KEY_UP)
        send_shell("\x1B[A", 3);
    else if (key == GUIAPP_KEY_DOWN)
        send_shell("\x1B[B", 3);
    else if (key == GUIAPP_KEY_RIGHT)
        send_shell("\x1B[C", 3);
    else if (key == GUIAPP_KEY_LEFT)
        send_shell("\x1B[D", 3);
    else if (key == GUIAPP_KEY_BACKSPACE || key == 127) {
        send_shell("\b", 1);
        track_input_backspace();
    } else if (key == '\r' || key == '\n') {
        send_shell("\n", 1);
        input_length = 0;
        input_line[0] = 0;
    } else if (key >= 0 && key < 256) {
        character = (char)key;
        send_shell(&character, 1);
        if (key >= 32 && key != 127) {
            char value[2] = {character, 0};
            track_input_text(value);
        }
    }
}

static void shell_reader(void) {
    char buffer[256];
    while (!stopping && shell_output >= 0) {
        int count = read(shell_output, buffer, sizeof(buffer));
        if (count <= 0)
            break;
        lock_terminal();
        int was_at_bottom = scroll_y >= max_scroll_y_locked();
        for (int i = 0; i < count; i++)
            put_byte_locked((unsigned char)buffer[i]);
        if (was_at_bottom)
            scroll_y = max_scroll_y_locked();
        else
            clamp_scroll_locked();
        render_locked();
        unlock_terminal();
    }
}

static int start_shell(void) {
    int input_pipe[2] = {-1, -1};
    int output_pipe[2] = {-1, -1};
    if (pipe(input_pipe) < 0)
        return -1;
    if (pipe(output_pipe) < 0) {
        close(input_pipe[0]);
        close(input_pipe[1]);
        return -1;
    }

    int saved[3] = {dup(0), dup(1), dup(2)};
    if (saved[0] < 0 || saved[1] < 0 || saved[2] < 0) {
        for (int i = 0; i < 3; i++)
            if (saved[i] >= 0) close(saved[i]);
        close(input_pipe[0]);
        close(input_pipe[1]);
        close(output_pipe[0]);
        close(output_pipe[1]);
        return -1;
    }

    int redirected =
        dup2(input_pipe[0], 0) >= 0 &&
        dup2(output_pipe[1], 1) >= 0 &&
        dup2(output_pipe[1], 2) >= 0;
    char *shell_argv[] = {"/bin/sh"};
    int pid = redirected
        ? spawn_process_args("/bin/sh", shell_argv, 1,
                             SPAWN_FLAG_SILENT | SPAWN_FLAG_INHERIT_STDIO)
        : -1;
    (void)dup2(saved[0], 0);
    (void)dup2(saved[1], 1);
    (void)dup2(saved[2], 2);
    for (int i = 0; i < 3; i++)
        close(saved[i]);
    close(input_pipe[0]);
    close(output_pipe[1]);
    if (pid < 0) {
        close(input_pipe[1]);
        close(output_pipe[0]);
        return -1;
    }

    shell_pid = pid;
    shell_input = input_pipe[1];
    shell_output = output_pipe[0];
    reader_tid = spawn(shell_reader);
    if (reader_tid < 0)
        return -1;
    return 0;
}

static void stop_shell(void) {
    stopping = 1;
    if (shell_input >= 0) {
        close(shell_input);
        shell_input = -1;
    }
    if (shell_pid > 0) {
        int status;
        (void)kill(shell_pid);
        (void)waitpid(shell_pid, &status, 0);
        shell_pid = -1;
    }
    if (reader_tid > 0) {
        (void)join(reader_tid);
        reader_tid = -1;
    }
    if (shell_output >= 0) {
        close(shell_output);
        shell_output = -1;
    }
}

static void handle_command(int command) {
    char clipboard[GUIAPP_PATH_MAX];
    int had_selection;
    if (command != GUIAPP_CMD_COPY && command != GUIAPP_CMD_CUT)
        return;

    lock_terminal();
    had_selection = selection_exists_locked();
    if (had_selection) {
        (void)copy_selection_locked(clipboard, sizeof(clipboard));
    } else {
        int length = (int)strlen(input_line);
        if (length >= (int)sizeof(clipboard))
            length = (int)sizeof(clipboard) - 1;
        memcpy(clipboard, input_line, (size_t)length);
        clipboard[length] = 0;
    }
    (void)guiapp_set_clipboard(&gui, clipboard);
    unlock_terminal();

    if (command == GUIAPP_CMD_CUT && !had_selection) {
        send_shell("\x15", 1);
        input_length = 0;
        input_line[0] = 0;
    }
}

static void handle_event(const struct guiapp_event *event) {
    if (event->type == GUIAPP_EVT_INIT || event->type == GUIAPP_EVT_RESIZE) {
        lock_terminal();
        int was_at_bottom = scroll_y >= max_scroll_y_locked();
        view_width = clamp_int(event->width, 180, GUIAPP_MAX_W);
        view_height = clamp_int(event->height, 140, GUIAPP_MAX_H);
        if (was_at_bottom)
            scroll_y = max_scroll_y_locked();
        clamp_scroll_locked();
        render_locked();
        unlock_terminal();
    } else if (event->type == GUIAPP_EVT_KEY && event->buttons) {
        lock_terminal();
        clear_selection_locked();
        render_locked();
        unlock_terminal();
        send_key(event->key);
    } else if (event->type == GUIAPP_EVT_TEXT) {
        lock_terminal();
        clear_selection_locked();
        render_locked();
        unlock_terminal();
        int length = (int)strlen(event->text);
        send_shell(event->text, length);
        track_input_text(event->text);
    } else if (event->type == GUIAPP_EVT_MOUSE) {
        lock_terminal();
        handle_mouse_locked(event);
        render_locked();
        unlock_terminal();
    } else if (event->type == GUIAPP_EVT_COMMAND) {
        handle_command(event->key);
    }
}

int main(int argc, char **argv) {
    struct guiapp_event event;
    if (guiapp_parse_args(argc, argv, &gui) < 0)
        return 1;
    reset_terminal_locked();
    if (start_shell() < 0) {
        stop_shell();
        return 1;
    }
    if (argv[4] && argv[4][0]) {
        send_shell(argv[4], (int)strlen(argv[4]));
        send_shell("\n", 1);
    }

    while (guiapp_read_event(&gui, &event) == 0) {
        if (event.type == GUIAPP_EVT_CLOSE)
            break;
        handle_event(&event);
    }
    stop_shell();
    free(pixels);
    return 0;
}
