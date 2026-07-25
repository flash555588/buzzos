#include "appui.h"
#include "guiapp.h"
#include "libc.h"

enum {
    MAX_W = GUIAPP_MAX_W,
    MAX_H = GUIAPP_MAX_H,
    MAX_PROCESSES = 32,
    HISTORY_SAMPLES = 60,
    TOOLBAR_H = 58,
    TABS_H = 40,
    SUMMARY_H = 82,
    TABLE_HEADER_H = 34,
    FOOTER_H = 50,
    ROW_H = 32,
    TAB_PROCESSES = 0,
    TAB_RESOURCES = 1,
    SORT_PID = 0,
    SORT_NAME,
    SORT_STATE,
    SORT_CPU,
    SORT_MEMORY,
};

struct process_row {
    int pid;
    char name[20];
    char state[12];
    uint32_t ticks;
    uint32_t rss_kb;
    int cpu_tenths;
};

static uint8_t pixels[MAX_W * MAX_H];
static struct process_row processes[MAX_PROCESSES];
static struct process_row fresh[MAX_PROCESSES];
static int process_count;
static int selected_pid = -1;
static int confirm_pid = -1;
static int scroll_row;
static int active_tab = TAB_PROCESSES;
static int sort_column = SORT_CPU;
static int sort_descending = 1;
static int paused;
static int pointer_x = -1;
static int pointer_y = -1;
static int pointer_buttons;
static int previous_buttons;
static int w = 760;
static int h = 520;
static int own_pid;
static uint32_t memory_total_kb;
static uint32_t memory_used_kb;
static uint32_t last_sample_ms;
static uint32_t last_refresh_ms;
static int system_cpu_tenths;
static int memory_tenths;
static int cpu_history[HISTORY_SAMPLES];
static int memory_history[HISTORY_SAMPLES];
static char status_text[96] = "Collecting process information...";

static int clamp_int(int value, int lo, int hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static void set_status(const char *text) {
    appui_copy_text(status_text, text, sizeof(status_text));
}

static int read_text_file(const char *path, char *buffer, int capacity) {
    if (!buffer || capacity <= 0)
        return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        buffer[0] = 0;
        return -1;
    }
    int used = 0;
    while (used + 1 < capacity) {
        int got = read(fd, buffer + used, (size_t)(capacity - used - 1));
        if (got <= 0)
            break;
        used += got;
    }
    close(fd);
    buffer[used] = 0;
    return used;
}

static void skip_space(const char **cursor) {
    while (**cursor == ' ' || **cursor == '\t' ||
           **cursor == '\r' || **cursor == '\n')
        (*cursor)++;
}

static int next_token(const char **cursor, char *out, int capacity) {
    int used = 0;
    skip_space(cursor);
    if (!**cursor)
        return 0;
    while (**cursor && **cursor != ' ' && **cursor != '\t' &&
           **cursor != '\r' && **cursor != '\n') {
        if (used + 1 < capacity)
            out[used++] = **cursor;
        (*cursor)++;
    }
    if (capacity > 0)
        out[used] = 0;
    return 1;
}

static uint32_t parse_u32(const char *text) {
    uint32_t value = 0;
    while (text && *text >= '0' && *text <= '9') {
        value = value * 10u + (uint32_t)(*text - '0');
        text++;
    }
    return value;
}

static uint32_t previous_ticks(int pid) {
    for (int i = 0; i < process_count; i++)
        if (processes[i].pid == pid)
            return processes[i].ticks;
    return 0;
}

static int compare_rows(const struct process_row *left,
                        const struct process_row *right) {
    int result = 0;
    if (sort_column == SORT_PID) {
        result = left->pid < right->pid ? -1 : left->pid > right->pid;
    } else if (sort_column == SORT_NAME) {
        result = strcmp(left->name, right->name);
    } else if (sort_column == SORT_STATE) {
        result = strcmp(left->state, right->state);
    } else if (sort_column == SORT_CPU) {
        result = left->cpu_tenths < right->cpu_tenths ? -1 :
                 left->cpu_tenths > right->cpu_tenths;
    } else {
        result = left->rss_kb < right->rss_kb ? -1 :
                 left->rss_kb > right->rss_kb;
    }
    if (result == 0)
        result = left->pid < right->pid ? -1 : left->pid > right->pid;
    return sort_descending ? -result : result;
}

static void sort_processes(void) {
    for (int i = 1; i < process_count; i++) {
        struct process_row value = processes[i];
        int j = i;
        while (j > 0 && compare_rows(&value, &processes[j - 1]) < 0) {
            processes[j] = processes[j - 1];
            j--;
        }
        processes[j] = value;
    }
}

static int process_index_for_pid(int pid) {
    for (int i = 0; i < process_count; i++)
        if (processes[i].pid == pid)
            return i;
    return -1;
}

static int parse_processes(uint32_t now) {
    char buffer[4096];
    const char *cursor;
    int count = 0;
    if (read_text_file("/proc/tasks", buffer, sizeof(buffer)) < 0)
        return -1;
    cursor = buffer;
    while (*cursor && *cursor != '\n')
        cursor++;
    if (*cursor == '\n')
        cursor++;

    while (*cursor && count < MAX_PROCESSES) {
        char pid[16], state[16], output[16], code[16];
        char ticks[16], rss[16], name[20];
        if (!next_token(&cursor, pid, sizeof(pid)) ||
            !next_token(&cursor, state, sizeof(state)) ||
            !next_token(&cursor, output, sizeof(output)) ||
            !next_token(&cursor, code, sizeof(code)) ||
            !next_token(&cursor, ticks, sizeof(ticks)) ||
            !next_token(&cursor, rss, sizeof(rss)) ||
            !next_token(&cursor, name, sizeof(name)))
            break;
        (void)output;
        (void)code;
        fresh[count].pid = atoi(pid);
        appui_copy_text(fresh[count].state, state,
                        sizeof(fresh[count].state));
        appui_copy_text(fresh[count].name, name,
                        sizeof(fresh[count].name));
        fresh[count].ticks = parse_u32(ticks);
        fresh[count].rss_kb = parse_u32(rss);
        fresh[count].cpu_tenths = 0;
        count++;
    }

    uint32_t elapsed = last_sample_ms ? now - last_sample_ms : 0;
    int total_non_idle = 0;
    for (int i = 0; i < count; i++) {
        uint32_t old_ticks = previous_ticks(fresh[i].pid);
        uint32_t delta = old_ticks ? fresh[i].ticks - old_ticks : 0;
        if (elapsed) {
            uint32_t scaled = delta * 4000u / elapsed;
            fresh[i].cpu_tenths = (int)(scaled > 1000u ? 1000u : scaled);
        }
        if (fresh[i].pid != 0)
            total_non_idle += fresh[i].cpu_tenths;
    }
    system_cpu_tenths = clamp_int(total_non_idle, 0, 1000);
    last_sample_ms = now;
    process_count = count;
    for (int i = 0; i < count; i++)
        processes[i] = fresh[i];
    sort_processes();

    if (selected_pid >= 0 && process_index_for_pid(selected_pid) < 0)
        selected_pid = -1;
    if (selected_pid < 0 && process_count > 0) {
        for (int i = 0; i < process_count; i++) {
            if (processes[i].pid > 0) {
                selected_pid = processes[i].pid;
                break;
            }
        }
    }
    return 0;
}

static int parse_memory(void) {
    char buffer[1024];
    char key[32];
    char value_text[24];
    const char *cursor;
    uint32_t page_size = 4096;
    uint32_t managed_pages = 0;
    uint32_t used_pages = 0;
    if (read_text_file("/proc/meminfo", buffer, sizeof(buffer)) < 0)
        return -1;
    cursor = buffer;
    while (next_token(&cursor, key, sizeof(key)) &&
           next_token(&cursor, value_text, sizeof(value_text))) {
        uint32_t value = parse_u32(value_text);
        if (strcmp(key, "page_size") == 0)
            page_size = value;
        else if (strcmp(key, "managed_pages") == 0)
            managed_pages = value;
        else if (strcmp(key, "used_pages") == 0)
            used_pages = value;
    }
    uint32_t page_kb = page_size / 1024u;
    if (!page_kb)
        page_kb = 1;
    memory_total_kb = managed_pages * page_kb;
    memory_used_kb = used_pages * page_kb;
    memory_tenths = managed_pages ?
        (int)(used_pages * 1000u / managed_pages) : 0;
    memory_tenths = clamp_int(memory_tenths, 0, 1000);
    return 0;
}

static void push_history(int *history, int value) {
    for (int i = 1; i < HISTORY_SAMPLES; i++)
        history[i - 1] = history[i];
    history[HISTORY_SAMPLES - 1] = clamp_int(value, 0, 1000);
}

static void refresh_data(void) {
    uint32_t now = monotonic_ms();
    int processes_ok = parse_processes(now) == 0;
    int memory_ok = parse_memory() == 0;
    if (!processes_ok || !memory_ok) {
        set_status("Some system information is unavailable");
    } else {
        set_status(paused ? "Paused" : "Live - 1 s refresh");
    }
    push_history(cpu_history, system_cpu_tenths);
    push_history(memory_history, memory_tenths);
    last_refresh_ms = now;
}

static void append_percent(char *out, int tenths, int capacity) {
    appui_append_int(out, tenths / 10, capacity);
    appui_append_text(out, ".", capacity);
    appui_append_int(out, tenths % 10, capacity);
    appui_append_text(out, "%", capacity);
}

static void format_percent(char *out, int tenths, int capacity) {
    out[0] = 0;
    append_percent(out, tenths, capacity);
}

static void format_memory(char *out, uint32_t kb, int capacity) {
    out[0] = 0;
    if (kb >= 1024u) {
        uint32_t tenths = kb * 10u / 1024u;
        appui_append_int(out, (int)(tenths / 10u), capacity);
        appui_append_text(out, ".", capacity);
        appui_append_int(out, (int)(tenths % 10u), capacity);
        appui_append_text(out, " MiB", capacity);
    } else {
        appui_append_int(out, (int)kb, capacity);
        appui_append_text(out, " KiB", capacity);
    }
}

static void format_uptime(char *out, int capacity) {
    uint32_t seconds = monotonic_ms() / 1000u;
    uint32_t hours = seconds / 3600u;
    uint32_t minutes = (seconds / 60u) % 60u;
    seconds %= 60u;
    out[0] = 0;
    appui_append_int(out, (int)hours, capacity);
    appui_append_text(out, "h ", capacity);
    appui_append_int(out, (int)minutes, capacity);
    appui_append_text(out, "m ", capacity);
    appui_append_int(out, (int)seconds, capacity);
    appui_append_text(out, "s", capacity);
}

static struct appui_rect tab_rect(int tab) {
    return (struct appui_rect){12 + tab * 156, TOOLBAR_H + 4, 148, 32};
}

static struct appui_rect refresh_button_rect(void) {
    return (struct appui_rect){appui_max(12, w - 232), 12, 112, 34};
}

static struct appui_rect pause_button_rect(void) {
    return (struct appui_rect){appui_max(112, w - 112), 12, 100, 34};
}

static int table_y(void) {
    return TOOLBAR_H + TABS_H + SUMMARY_H;
}

static int visible_rows(void) {
    int rows = (h - table_y() - TABLE_HEADER_H - FOOTER_H) / ROW_H;
    return rows > 0 ? rows : 1;
}

static void clamp_scroll(void) {
    int maximum = appui_max(0, process_count - visible_rows());
    scroll_row = clamp_int(scroll_row, 0, maximum);
    int selected = process_index_for_pid(selected_pid);
    if (selected >= 0 && selected < scroll_row)
        scroll_row = selected;
    if (selected >= scroll_row + visible_rows())
        scroll_row = selected - visible_rows() + 1;
}

static struct appui_rect end_button_rect(void) {
    return (struct appui_rect){appui_max(12, w - 192), h - 42, 180, 34};
}

static int can_end_selected(void) {
    return selected_pid > 0 && selected_pid != own_pid &&
           process_index_for_pid(selected_pid) >= 0;
}

static struct appui_rect header_cell(int column) {
    int state_x = appui_max(184, w - 348);
    int cpu_x = appui_max(state_x + 100, w - 228);
    int memory_x = appui_max(cpu_x + 82, w - 138);
    int y = table_y();
    if (column == SORT_PID)
        return (struct appui_rect){12, y, 62, TABLE_HEADER_H};
    if (column == SORT_NAME)
        return (struct appui_rect){74, y, appui_max(60, state_x - 74),
                                   TABLE_HEADER_H};
    if (column == SORT_STATE)
        return (struct appui_rect){state_x, y, cpu_x - state_x,
                                   TABLE_HEADER_H};
    if (column == SORT_CPU)
        return (struct appui_rect){cpu_x, y, memory_x - cpu_x,
                                   TABLE_HEADER_H};
    return (struct appui_rect){memory_x, y, w - memory_x - 8,
                               TABLE_HEADER_H};
}

static void draw_label(int x, int y, const char *text, int color,
                       struct appui_rect clip) {
    appui_text(pixels, w, h, x, y, text, color, -1, clip);
}

static void draw_summary_card(struct appui_rect area, const char *label,
                              const char *value, int accent, int tenths) {
    appui_fill_round(pixels, w, h, area, THEME_DIVIDER);
    appui_fill_round(pixels, w, h,
                     (struct appui_rect){area.x + 1, area.y + 1,
                                         area.w - 2, area.h - 2},
                     THEME_PANEL_RAISED);
    draw_label(area.x + 10, area.y + 10, label, THEME_TEXT_DIM,
               (struct appui_rect){area.x + 8, area.y + 4,
                                   area.w - 16, 28});
    draw_label(area.x + 10, area.y + 34, value, THEME_TEXT,
               (struct appui_rect){area.x + 8, area.y + 30,
                                   area.w - 16, 28});
    if (tenths >= 0) {
        struct appui_rect track = {area.x + 10, area.y + area.h - 10,
                                   area.w - 20, 4};
        appui_fill(pixels, w, h, track, THEME_FIELD_BG);
        appui_fill(pixels, w, h,
                   (struct appui_rect){track.x, track.y,
                                       track.w * clamp_int(tenths, 0, 1000) /
                                           1000,
                                       track.h},
                   accent);
    }
}

static void draw_summary(void) {
    int y = TOOLBAR_H + TABS_H + 8;
    int gap = 8;
    int card_w = (w - 24 - gap * 2) / 3;
    char cpu[24], memory[40], count[24];
    format_percent(cpu, system_cpu_tenths, sizeof(cpu));
    format_memory(memory, memory_used_kb, sizeof(memory));
    count[0] = 0;
    appui_append_int(count, process_count, sizeof(count));
    appui_append_text(count, process_count == 1 ? " process" : " processes",
                      sizeof(count));
    draw_summary_card((struct appui_rect){12, y, card_w, 66},
                      "CPU", cpu, THEME_ACCENT, system_cpu_tenths);
    draw_summary_card((struct appui_rect){12 + card_w + gap, y, card_w, 66},
                      "Memory", memory, THEME_MAX_GREEN, memory_tenths);
    draw_summary_card((struct appui_rect){12 + (card_w + gap) * 2, y,
                                          w - 24 - (card_w + gap) * 2, 66},
                      "Processes", count, THEME_MIN_YELLOW, -1);
}

static void header_label(int column, const char *label) {
    char text[24];
    appui_copy_text(text, label, sizeof(text));
    if (sort_column == column)
        appui_append_text(text, sort_descending ? " v" : " ^", sizeof(text));
    struct appui_rect cell = header_cell(column);
    draw_label(cell.x + 8, cell.y + 7, text,
               sort_column == column ? THEME_TEXT : THEME_TEXT_DIM,
               (struct appui_rect){cell.x + 4, cell.y + 2,
                                   cell.w - 8, cell.h - 4});
}

static void draw_process_table(void) {
    int y = table_y();
    appui_fill(pixels, w, h,
               (struct appui_rect){8, y, w - 16, TABLE_HEADER_H},
               THEME_LIST_HEADER);
    header_label(SORT_PID, "PID");
    header_label(SORT_NAME, "Process");
    header_label(SORT_STATE, "State");
    header_label(SORT_CPU, "CPU");
    header_label(SORT_MEMORY, "Memory");

    int rows = visible_rows();
    int body_y = y + TABLE_HEADER_H;
    int state_x = header_cell(SORT_STATE).x;
    int cpu_x = header_cell(SORT_CPU).x;
    int memory_x = header_cell(SORT_MEMORY).x;
    struct appui_rect body = {8, body_y, w - 16, rows * ROW_H};
    appui_fill(pixels, w, h, body, THEME_LIST_BG);
    for (int shown = 0; shown < rows; shown++) {
        int index = scroll_row + shown;
        if (index >= process_count)
            break;
        struct process_row *row = &processes[index];
        int row_y = body_y + shown * ROW_H;
        int selected = row->pid == selected_pid;
        int bg = selected ? THEME_SELECTION_BG :
                 ((index & 1) ? THEME_LIST_ALT : THEME_LIST_BG);
        appui_fill(pixels, w, h,
                   (struct appui_rect){8, row_y, w - 16, ROW_H}, bg);
        char pid[16], cpu[20], memory[24];
        pid[0] = 0;
        appui_append_int(pid, row->pid, sizeof(pid));
        format_percent(cpu, row->cpu_tenths, sizeof(cpu));
        format_memory(memory, row->rss_kb, sizeof(memory));
        int fg = selected ? THEME_SELECTION_TEXT : THEME_LIST_TEXT;
        int dim = selected ? THEME_SELECTION_TEXT : THEME_TEXT_DIM;
        int text_y = row_y + (ROW_H - KFONT_HEIGHT) / 2 + PLT_FONT_Y_SHIFT;
        draw_label(20, text_y, pid, dim,
                   (struct appui_rect){12, row_y + 2, 58, ROW_H - 4});
        draw_label(82, text_y, row->name, fg,
                   (struct appui_rect){78, row_y + 2,
                                       appui_max(40, state_x - 84),
                                       ROW_H - 4});
        draw_label(state_x + 8, text_y, row->state, dim,
                   (struct appui_rect){state_x + 4, row_y + 2,
                                       cpu_x - state_x - 8, ROW_H - 4});
        draw_label(cpu_x + 8, text_y, cpu, fg,
                   (struct appui_rect){cpu_x + 4, row_y + 2,
                                       memory_x - cpu_x - 8, ROW_H - 4});
        draw_label(memory_x + 8, text_y, memory, fg,
                   (struct appui_rect){memory_x + 4, row_y + 2,
                                       w - memory_x - 16, ROW_H - 4});
    }

    if (process_count > rows) {
        struct appui_rect track = {w - 10, body_y, 4, rows * ROW_H};
        int thumb_h = appui_max(20, track.h * rows / process_count);
        int maximum = process_count - rows;
        int thumb_y = track.y + (track.h - thumb_h) * scroll_row /
                                appui_max(1, maximum);
        appui_fill_round(pixels, w, h, track, THEME_FIELD_BG);
        appui_fill_round(pixels, w, h,
                         (struct appui_rect){track.x, thumb_y,
                                             track.w, thumb_h},
                         THEME_TEXT_FAINT);
    }
}

static void draw_line(struct appui_rect clip, int x0, int y0,
                      int x1, int y1, int color) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy_abs = y1 > y0 ? y1 - y0 : y0 - y1;
    int dy = -dy_abs;
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        if (appui_inside(x0, y0, clip)) {
            appui_pixel(pixels, w, h, x0, y0, color);
            appui_pixel(pixels, w, h, x0, y0 + 1, color);
        }
        if (x0 == x1 && y0 == y1)
            break;
        int doubled = error * 2;
        if (doubled >= dy) {
            error += dy;
            x0 += sx;
        }
        if (doubled <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

static void draw_history_graph(struct appui_rect area, const int *history,
                               int color) {
    appui_fill_round(pixels, w, h, area, THEME_DIVIDER);
    appui_fill(pixels, w, h,
               (struct appui_rect){area.x + 1, area.y + 1,
                                   area.w - 2, area.h - 2},
               THEME_FIELD_BG);
    struct appui_rect plot = {area.x + 10, area.y + 8,
                              area.w - 20, area.h - 16};
    for (int i = 1; i < 4; i++) {
        int gy = plot.y + plot.h * i / 4;
        appui_fill(pixels, w, h,
                   (struct appui_rect){plot.x, gy, plot.w, 1},
                   THEME_WIN_PANEL);
    }
    int previous_x = plot.x;
    int previous_y = plot.y + plot.h -
                     history[0] * appui_max(1, plot.h - 1) / 1000;
    for (int i = 1; i < HISTORY_SAMPLES; i++) {
        int x = plot.x + i * appui_max(1, plot.w - 1) /
                         (HISTORY_SAMPLES - 1);
        int y = plot.y + plot.h -
                history[i] * appui_max(1, plot.h - 1) / 1000;
        draw_line(plot, previous_x, previous_y, x, y, color);
        previous_x = x;
        previous_y = y;
    }
}

static void draw_resources(void) {
    int top = TOOLBAR_H + TABS_H + 10;
    int graph_h = appui_max(36, (h - top - 120) / 2);
    char value[64];
    format_percent(value, system_cpu_tenths, sizeof(value));
    draw_label(16, top, "CPU history", THEME_TEXT,
               (struct appui_rect){12, top - 2, w - 24, 30});
    int value_w = appui_text_width(value);
    draw_label(w - 16 - value_w, top, value, THEME_ACCENT,
               (struct appui_rect){12, top - 2, w - 24, 30});
    draw_history_graph((struct appui_rect){12, top + 30, w - 24, graph_h},
                       cpu_history, THEME_ACCENT);

    int memory_y = top + 42 + graph_h;
    value[0] = 0;
    format_memory(value, memory_used_kb, sizeof(value));
    appui_append_text(value, " of ", sizeof(value));
    char total[24];
    format_memory(total, memory_total_kb, sizeof(total));
    appui_append_text(value, total, sizeof(value));
    draw_label(16, memory_y, "Memory history", THEME_TEXT,
               (struct appui_rect){12, memory_y - 2, w - 24, 30});
    value_w = appui_text_width(value);
    draw_label(w - 16 - value_w, memory_y, value, THEME_MAX_GREEN,
               (struct appui_rect){12, memory_y - 2, w - 24, 30});
    draw_history_graph((struct appui_rect){12, memory_y + 30,
                                           w - 24, graph_h},
                       memory_history, THEME_MAX_GREEN);
}

static struct appui_rect confirm_box(void) {
    int box_w = appui_min(620, w - 32);
    int box_h = 200;
    return (struct appui_rect){(w - box_w) / 2, (h - box_h) / 2,
                               box_w, box_h};
}

static struct appui_rect confirm_button(int accept) {
    struct appui_rect box = confirm_box();
    return (struct appui_rect){box.x + box.w - (accept ? 194 : 334),
                               box.y + box.h - 48,
                               accept ? 176 : 124, 34};
}

static void draw_confirmation(void) {
    if (confirm_pid < 0)
        return;
    appui_fill_blend(pixels, w, h, (struct appui_rect){0, 0, w, h},
                     THEME_DESKTOP_DEEP, 176);
    struct appui_rect box = confirm_box();
    appui_fill_round(pixels, w, h, box, THEME_DANGER);
    appui_fill_round(pixels, w, h,
                     (struct appui_rect){box.x + 1, box.y + 1,
                                         box.w - 2, box.h - 2},
                     THEME_PANEL_RAISED);
    draw_label(box.x + 18, box.y + 18, "End this process?",
               THEME_TEXT,
               (struct appui_rect){box.x + 12, box.y + 10,
                                   box.w - 24, 32});
    char message[112] = "Terminate PID ";
    appui_append_int(message, confirm_pid, sizeof(message));
    appui_append_text(message, " and all of its threads?\n", sizeof(message));
    appui_append_text(message, "Unsaved work may be lost.", sizeof(message));
    draw_label(box.x + 18, box.y + 58, message, THEME_TEXT_DIM,
               (struct appui_rect){box.x + 12, box.y + 48,
                                   box.w - 24, 68});
    appui_button_ex(pixels, w, h, confirm_button(0), "Cancel",
                    APPUI_BTN_DEFAULT,
                    appui_pointer_state(confirm_button(0), pointer_x,
                                        pointer_y, pointer_buttons));
    appui_button_ex(pixels, w, h, confirm_button(1), "End Process",
                    APPUI_BTN_DANGER,
                    appui_pointer_state(confirm_button(1), pointer_x,
                                        pointer_y, pointer_buttons));
}

static void render(void) {
    appui_fill(pixels, w, h, (struct appui_rect){0, 0, w, h},
               THEME_APP_BG);
    appui_fill(pixels, w, h, (struct appui_rect){0, 0, w, TOOLBAR_H},
               THEME_TOOLBAR_BG);
    draw_label(16, 18, "System Monitor", THEME_TEXT,
               (struct appui_rect){12, 8, appui_max(100, w - 240), 40});
    appui_button_ex(pixels, w, h, refresh_button_rect(), "Refresh",
                    APPUI_BTN_DEFAULT,
                    appui_pointer_state(refresh_button_rect(), pointer_x,
                                        pointer_y, pointer_buttons));
    appui_button_ex(pixels, w, h, pause_button_rect(),
                    paused ? "Resume" : "Pause",
                    paused ? APPUI_BTN_PRIMARY : APPUI_BTN_DEFAULT,
                    appui_pointer_state(pause_button_rect(), pointer_x,
                                        pointer_y, pointer_buttons));
    appui_fill(pixels, w, h,
               (struct appui_rect){0, TOOLBAR_H, w, TABS_H},
               THEME_PANEL_BG);
    appui_button_ex(pixels, w, h, tab_rect(TAB_PROCESSES), "Processes",
                    APPUI_BTN_GHOST,
                    appui_pointer_state(tab_rect(TAB_PROCESSES), pointer_x,
                                        pointer_y, pointer_buttons) |
                    (active_tab == TAB_PROCESSES ? APPUI_STATE_SELECTED : 0));
    appui_button_ex(pixels, w, h, tab_rect(TAB_RESOURCES), "Resources",
                    APPUI_BTN_GHOST,
                    appui_pointer_state(tab_rect(TAB_RESOURCES), pointer_x,
                                        pointer_y, pointer_buttons) |
                    (active_tab == TAB_RESOURCES ? APPUI_STATE_SELECTED : 0));

    if (active_tab == TAB_PROCESSES) {
        draw_summary();
        draw_process_table();
        appui_fill(pixels, w, h,
                   (struct appui_rect){0, h - FOOTER_H, w, FOOTER_H},
                   THEME_TOOLBAR_BG);
        draw_label(14, h - 31, status_text, THEME_TEXT_DIM,
                   (struct appui_rect){10, h - 42,
                                       appui_max(40, w - 216), 36});
        int state = appui_pointer_state(end_button_rect(), pointer_x,
                                        pointer_y, pointer_buttons);
        if (!can_end_selected())
            state |= APPUI_STATE_DISABLED;
        appui_button_ex(pixels, w, h, end_button_rect(), "End Process",
                        APPUI_BTN_DANGER, state);
    } else {
        draw_resources();
        char uptime[48];
        format_uptime(uptime, sizeof(uptime));
        char footer[80] = "Uptime ";
        appui_append_text(footer, uptime, sizeof(footer));
        draw_label(14, h - 28, footer, THEME_TEXT_DIM,
                   (struct appui_rect){10, h - 40, w - 20, 34});
    }
    draw_confirmation();
}

static void select_sort(int column) {
    if (sort_column == column) {
        sort_descending = !sort_descending;
    } else {
        sort_column = column;
        sort_descending = column == SORT_CPU || column == SORT_MEMORY;
    }
    sort_processes();
    clamp_scroll();
}

static void request_end_selected(void) {
    if (can_end_selected())
        confirm_pid = selected_pid;
}

static void end_confirmed_process(void) {
    int pid = confirm_pid;
    confirm_pid = -1;
    if (pid <= 0 || pid == own_pid)
        return;
    if (kill(pid) < 0) {
        set_status("The process could not be terminated");
    } else {
        set_status("Process terminated");
        if (selected_pid == pid)
            selected_pid = -1;
        refresh_data();
    }
}

static void handle_click(int x, int y) {
    if (confirm_pid >= 0) {
        if (appui_inside(x, y, confirm_button(0)))
            confirm_pid = -1;
        else if (appui_inside(x, y, confirm_button(1)))
            end_confirmed_process();
        return;
    }
    if (appui_inside(x, y, refresh_button_rect())) {
        refresh_data();
        return;
    }
    if (appui_inside(x, y, pause_button_rect())) {
        paused = !paused;
        set_status(paused ? "Paused" : "Live - 1 s refresh");
        if (!paused)
            refresh_data();
        return;
    }
    for (int tab = 0; tab <= 1; tab++) {
        if (appui_inside(x, y, tab_rect(tab))) {
            active_tab = tab;
            return;
        }
    }
    if (active_tab != TAB_PROCESSES)
        return;
    for (int column = SORT_PID; column <= SORT_MEMORY; column++) {
        if (appui_inside(x, y, header_cell(column))) {
            select_sort(column);
            return;
        }
    }
    if (appui_inside(x, y, end_button_rect())) {
        request_end_selected();
        return;
    }
    int body_y = table_y() + TABLE_HEADER_H;
    if (x >= 8 && x < w - 8 && y >= body_y &&
        y < h - FOOTER_H) {
        int index = scroll_row + (y - body_y) / ROW_H;
        if (index >= 0 && index < process_count)
            selected_pid = processes[index].pid;
    }
}

static void handle_mouse(int x, int y, int buttons, int wheel) {
    pointer_x = x;
    pointer_y = y;
    pointer_buttons = buttons;
    if (active_tab == TAB_PROCESSES && confirm_pid < 0 && wheel) {
        scroll_row -= wheel * 3;
        clamp_scroll();
    }
    if ((buttons & 1) && !(previous_buttons & 1))
        handle_click(x, y);
    previous_buttons = buttons;
}

static void move_selection(int direction) {
    int index = process_index_for_pid(selected_pid);
    if (index < 0)
        index = direction > 0 ? -1 : process_count;
    index = clamp_int(index + direction, 0, appui_max(0, process_count - 1));
    if (process_count > 0)
        selected_pid = processes[index].pid;
    clamp_scroll();
}

static void handle_key(int key) {
    if (confirm_pid >= 0) {
        if (key == GUIAPP_KEY_ESC)
            confirm_pid = -1;
        else if (key == '\r' || key == '\n')
            end_confirmed_process();
        return;
    }
    if (key == GUIAPP_KEY_UP)
        move_selection(-1);
    else if (key == GUIAPP_KEY_DOWN)
        move_selection(1);
    else if (key == GUIAPP_KEY_LEFT)
        active_tab = TAB_PROCESSES;
    else if (key == GUIAPP_KEY_RIGHT)
        active_tab = TAB_RESOURCES;
    else if (key == 'r' || key == 'R')
        refresh_data();
    else if (key == ' ' || key == 'p' || key == 'P') {
        paused = !paused;
        if (!paused)
            refresh_data();
    } else if (key == 'k' || key == 'K')
        request_end_selected();
}

int main(int argc, char **argv) {
    struct guiapp_ctx ctx;
    struct guiapp_event event;
    if (guiapp_parse_args(argc, argv, &ctx) < 0)
        return 1;
    own_pid = getpid();
    refresh_data();
    for (;;) {
        if (guiapp_read_event(&ctx, &event) < 0 ||
            event.type == GUIAPP_EVT_CLOSE)
            break;
        if (event.type == GUIAPP_EVT_INIT ||
            event.type == GUIAPP_EVT_RESIZE) {
            w = clamp_int(event.width, 420, MAX_W);
            h = clamp_int(event.height, 300, MAX_H);
            clamp_scroll();
        } else if (event.type == GUIAPP_EVT_MOUSE) {
            handle_mouse(event.x, event.y, event.buttons, event.wheel);
        } else if (event.type == GUIAPP_EVT_KEY && event.buttons) {
            handle_key(event.key);
        } else if (event.type == GUIAPP_EVT_TICK && !paused &&
                   (uint32_t)(monotonic_ms() - last_refresh_ms) >= 1000u) {
            refresh_data();
            clamp_scroll();
        }
        render();
        if (guiapp_send_frame(&ctx, "System Monitor", w, h, pixels) < 0)
            break;
    }
    return 0;
}
