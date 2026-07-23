#include "appui.h"
#include "guiapp.h"
#include "libc.h"

enum {
    TERM_HISTORY_LINES = 256,
    TERM_LINE_BYTES = 512,
    TERM_MARGIN_X = 10,
    TERM_MARGIN_Y = 8,
    TERM_LINE_HEIGHT = KFONT_HEIGHT,
};

static struct guiapp_ctx gui;
static char lines[TERM_HISTORY_LINES][TERM_LINE_BYTES];
static int first_line;
static int line_count = 1;
static int cursor_line;
static int cursor_col;
static int view_offset;
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
    view_offset = 0;
}

static int visible_rows_locked(void) {
    int rows = (view_height - TERM_MARGIN_Y * 2) / TERM_LINE_HEIGHT;
    return rows > 0 ? rows : 1;
}

static int terminal_columns_locked(void) {
    int columns = (view_width - TERM_MARGIN_X * 2) / KFONT_WIDTH;
    return columns > 0 ? columns : 1;
}

static int max_view_offset_locked(void) {
    int extra = line_count - visible_rows_locked();
    return extra > 0 ? extra : 0;
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

    int columns = terminal_columns_locked();
    if (cursor_col >= TERM_LINE_BYTES - 2 || cursor_col >= columns * 4)
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

static void render_locked(void) {
    if (ensure_pixels_locked() < 0)
        return;
    struct appui_rect full = {0, 0, view_width, view_height};
    struct appui_rect clip = {
        TERM_MARGIN_X, TERM_MARGIN_Y,
        view_width - TERM_MARGIN_X * 2,
        view_height - TERM_MARGIN_Y * 2
    };
    int background = plt_rgb(14, 18, 26);
    int foreground = appui_rgb6(3, 5, 4);
    appui_fill(pixels, view_width, view_height, full, background);

    int rows = visible_rows_locked();
    int bottom_start = line_count > rows ? line_count - rows : 0;
    int start = bottom_start - view_offset;
    if (start < 0)
        start = 0;
    int y = TERM_MARGIN_Y;
    for (int row = start; row < line_count && row < start + rows; row++) {
        appui_text(pixels, view_width, view_height, TERM_MARGIN_X, y,
                   line_at(row), foreground, -1, clip);
        y += TERM_LINE_HEIGHT;
    }

    if (view_offset == 0 && cursor_line >= start &&
        cursor_line < start + rows) {
        char *line = line_at(cursor_line);
        int saved = cursor_col;
        if (saved < 0) saved = 0;
        if (saved >= TERM_LINE_BYTES) saved = TERM_LINE_BYTES - 1;
        char tail = line[saved];
        line[saved] = 0;
        int cursor_x = TERM_MARGIN_X + appui_text_width(line);
        line[saved] = tail;
        int cursor_y = TERM_MARGIN_Y +
            (cursor_line - start) * TERM_LINE_HEIGHT + 3;
        appui_fill(pixels, view_width, view_height,
                   (struct appui_rect){cursor_x, cursor_y, 8, 16}, 15);
    }
    (void)guiapp_send_frame(&gui, "Terminal", view_width, view_height, pixels);
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
        int was_at_bottom = view_offset == 0;
        for (int i = 0; i < count; i++)
            put_byte_locked((unsigned char)buffer[i]);
        if (was_at_bottom)
            view_offset = 0;
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
    if (command == GUIAPP_CMD_COPY) {
        (void)guiapp_set_clipboard(&gui, input_line);
    } else if (command == GUIAPP_CMD_CUT) {
        (void)guiapp_set_clipboard(&gui, input_line);
        send_shell("\x15", 1);
        input_length = 0;
        input_line[0] = 0;
    }
}

static void handle_event(const struct guiapp_event *event) {
    if (event->type == GUIAPP_EVT_INIT || event->type == GUIAPP_EVT_RESIZE) {
        lock_terminal();
        view_width = clamp_int(event->width, 180, GUIAPP_MAX_W);
        view_height = clamp_int(event->height, 140, GUIAPP_MAX_H);
        view_offset = clamp_int(view_offset, 0, max_view_offset_locked());
        render_locked();
        unlock_terminal();
    } else if (event->type == GUIAPP_EVT_KEY && event->buttons) {
        send_key(event->key);
    } else if (event->type == GUIAPP_EVT_TEXT) {
        int length = (int)strlen(event->text);
        send_shell(event->text, length);
        track_input_text(event->text);
    } else if (event->type == GUIAPP_EVT_MOUSE && event->wheel) {
        lock_terminal();
        view_offset = clamp_int(view_offset + event->wheel * 3,
                                0, max_view_offset_locked());
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
