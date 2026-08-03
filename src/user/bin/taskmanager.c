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
    HEADER_CHEVRON = 10,
    CPU_BAR_MIN_W = 48,
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

static uint32_t *pixels;
static size_t pixels_cap;
static struct process_row processes[MAX_PROCESSES];
static struct process_row fresh[MAX_PROCESSES];
static int process_count;
static int selected_pid = -1;
static int confirm_pid = -1;
static int scroll_row;
static int scroll_dragging;
static int scroll_drag_mouse;
static int scroll_drag_start_px;
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

/* Returns 1 if this pid was present in the previous sample. */
static int previous_ticks(int pid, uint32_t *out) {
    for (int i = 0; i < process_count; i++) {
        if (processes[i].pid == pid) {
            if (out)
                *out = processes[i].ticks;
            return 1;
        }
    }
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
    /* Keep in sync with kernel TIMER_HZ (src/kernel/drv/timer.h). */
    enum { SAMPLE_TIMER_HZ = 250 };
    char buffer[12288];
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

    /*
     * Single-core CPU% for the sample window:
     *   expected jiffies ≈ elapsed_ms * TIMER_HZ / 1000
     *   denom = max(expected, ΣΔticks)
     *   row%  = Δticks / denom
     *   header total%  ≡ sum of non-idle row%  (same numbers, no second formula)
     * Unaccounted wall time is folded into idle so rows (incl. idle) ≈ 100%.
     */
    uint32_t deltas[MAX_PROCESSES];
    uint32_t total_delta = 0;
    uint32_t idle_delta = 0;
    int idle_index = -1;
    for (int i = 0; i < count; i++) {
        uint32_t old = 0;
        uint32_t delta = 0;
        if (last_sample_ms && previous_ticks(fresh[i].pid, &old))
            delta = fresh[i].ticks - old;
        deltas[i] = delta;
        total_delta += delta;
        if (fresh[i].pid == 0) {
            idle_delta = delta;
            idle_index = i;
        }
    }

    uint32_t elapsed_ms = last_sample_ms ? (now - last_sample_ms) : 0;
    if (!last_sample_ms || elapsed_ms < 50u) {
        for (int i = 0; i < count; i++) {
            int prev = process_index_for_pid(fresh[i].pid);
            fresh[i].cpu_tenths = prev >= 0 ? processes[prev].cpu_tenths : 0;
        }
        if (!last_sample_ms)
            system_cpu_tenths = 0;
        else {
            int busy = 0;
            for (int i = 0; i < count; i++)
                if (fresh[i].pid != 0)
                    busy += fresh[i].cpu_tenths;
            system_cpu_tenths = clamp_int(busy, 0, 1000);
        }
    } else {
        uint32_t elapsed_ticks =
            (elapsed_ms * (uint32_t)SAMPLE_TIMER_HZ + 500u) / 1000u;
        if (elapsed_ticks == 0)
            elapsed_ticks = 1;
        uint32_t denom = total_delta > elapsed_ticks ? total_delta
                                                     : elapsed_ticks;
        if (denom == 0)
            denom = 1;

        int busy_sum = 0;
        for (int i = 0; i < count; i++) {
            if (fresh[i].pid == 0)
                continue; /* idle filled in after busy sum */
            int sample = (int)((deltas[i] * 1000u) / denom);
            if (sample > 1000)
                sample = 1000;
            fresh[i].cpu_tenths = sample;
            busy_sum += sample;
        }
        if (busy_sum > 1000) {
            /* Clock skew: scale busy rows so they fit in 100%. */
            for (int i = 0; i < count; i++) {
                if (fresh[i].pid == 0)
                    continue;
                fresh[i].cpu_tenths =
                    (int)((uint32_t)fresh[i].cpu_tenths * 1000u /
                          (uint32_t)busy_sum);
            }
            busy_sum = 0;
            for (int i = 0; i < count; i++)
                if (fresh[i].pid != 0)
                    busy_sum += fresh[i].cpu_tenths;
        }
        /* Header total is exactly the sum of non-idle rows. */
        system_cpu_tenths = clamp_int(busy_sum, 0, 1000);

        if (idle_index >= 0) {
            /* Idle + unaccounted wall time so listed rows sum to ~100%. */
            fresh[idle_index].cpu_tenths = 1000 - system_cpu_tenths;
            (void)idle_delta;
        }
    }

    last_sample_ms = now ? now : 1;
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

static void draw_summary_card(struct appui_rect area, const char *label,
                              const char *value, uint32_t accent, int tenths) {
    appui_card(pixels, w, h, area);
    appui_label(pixels, w, h,
                (struct appui_rect){area.x + 12, area.y + 6, area.w - 24, 20},
                label, UI_FONT_CAPTION, UI_TEXT_TERTIARY, UI_ALIGN_LEFT);
    appui_label(pixels, w, h,
                (struct appui_rect){area.x + 12, area.y + 26, area.w - 24, 24},
                value, UI_FONT_TITLE, UI_TEXT_PRIMARY, UI_ALIGN_LEFT);
    if (tenths >= 0) {
        struct appui_rect track = {area.x + 12, area.y + area.h - 12,
                                   area.w - 24, 4};
        /* appui_progress paints the accent fill; a status-coloured bar keeps
         * the per-card hue, so those two are drawn by hand over its track. */
        appui_fill_round_r(pixels, w, h, track, track.h / 2, UI_CTRL_REST);
        appui_fill_round_r(pixels, w, h,
                           (struct appui_rect){track.x, track.y,
                                               track.w *
                                                   clamp_int(tenths, 0, 1000) /
                                                   1000,
                                               track.h},
                           track.h / 2, accent);
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
                      "CPU", cpu, UI_ACCENT_FILL, system_cpu_tenths);
    draw_summary_card((struct appui_rect){12 + card_w + gap, y, card_w, 66},
                      "Memory", memory, UI_SYS_SUCCESS, memory_tenths);
    draw_summary_card((struct appui_rect){12 + (card_w + gap) * 2, y,
                                          w - 24 - (card_w + gap) * 2, 66},
                      "Processes", count, UI_SYS_CAUTION, -1);
}

/* Sort direction is a chevron rather than an ASCII "v"/"^", which never
 * aligned with the label baseline. */
static void header_label(int column, const char *label) {
    struct appui_rect cell = header_cell(column);
    int active = sort_column == column;
    int label_w = appui_label_width(label, UI_FONT_CAPTION);
    int text_w = appui_min(label_w, appui_max(1, cell.w - 16));
    appui_label(pixels, w, h,
                (struct appui_rect){cell.x + 8, cell.y, text_w, cell.h}, label,
                UI_FONT_CAPTION,
                active ? UI_TEXT_PRIMARY : UI_TEXT_TERTIARY, UI_ALIGN_LEFT);
    if (active && cell.w - 16 - text_w >= HEADER_CHEVRON)
        appui_icon(pixels, w, h,
                   sort_descending ? UI_ICON_CHEVRON_DOWN : UI_ICON_CHEVRON_UP,
                   (struct appui_rect){cell.x + 10 + text_w, cell.y,
                                       HEADER_CHEVRON, cell.h},
                   HEADER_CHEVRON, UI_ACCENT_FILL);
}

static int table_content_px(void) {
    return process_count * ROW_H;
}

static int table_viewport_px(void) {
    return visible_rows() * ROW_H;
}

static int table_scrollable(void) {
    return table_content_px() > table_viewport_px();
}

static struct appui_rect table_scroll_track(void) {
    return (struct appui_rect){w - 8 - APPUI_SCROLL_W,
                               table_y() + TABLE_HEADER_H, APPUI_SCROLL_W,
                               table_viewport_px()};
}

static struct appui_rect table_scroll_thumb(void) {
    return appui_scroll_thumb(table_scroll_track(), 1, table_content_px(),
                              table_viewport_px(), scroll_row * ROW_H);
}

static void draw_process_table(void) {
    int y = table_y();
    appui_fill(pixels, w, h,
               (struct appui_rect){8, y, w - 16, TABLE_HEADER_H},
               UI_BG_LAYER);
    header_label(SORT_PID, "PID");
    header_label(SORT_NAME, "Process");
    header_label(SORT_STATE, "State");
    header_label(SORT_CPU, "CPU");
    header_label(SORT_MEMORY, "Memory");
    appui_separator(pixels, w, h, 8, y + TABLE_HEADER_H - 1, w - 16, 0);

    int rows = visible_rows();
    int body_y = y + TABLE_HEADER_H;
    int scroll_w = table_scrollable() ? APPUI_SCROLL_W : 0;
    int state_x = header_cell(SORT_STATE).x;
    int cpu_x = header_cell(SORT_CPU).x;
    int memory_x = header_cell(SORT_MEMORY).x;
    struct appui_rect body = {8, body_y, w - 16, rows * ROW_H};
    appui_fill(pixels, w, h, body, UI_BG_SOLID);
    for (int shown = 0; shown < rows; shown++) {
        int index = scroll_row + shown;
        if (index >= process_count)
            break;
        struct process_row *row = &processes[index];
        int row_y = body_y + shown * ROW_H;
        struct appui_rect row_rect = {8, row_y, w - 16 - scroll_w, ROW_H};
        int state = appui_pointer_state(row_rect, pointer_x, pointer_y,
                                        pointer_buttons);
        if (row->pid == selected_pid)
            state |= APPUI_STATE_SELECTED;
        /* Empty label: the table paints its own columns, but the row still
         * owns the selection fill, accent bar and hover state. */
        appui_list_row(pixels, w, h, row_rect, "", -1, state);
        char pid[16], cpu[20], memory[24];
        pid[0] = 0;
        appui_append_int(pid, row->pid, sizeof(pid));
        format_percent(cpu, row->cpu_tenths, sizeof(cpu));
        format_memory(memory, row->rss_kb, sizeof(memory));
        uint32_t fg = (state & APPUI_STATE_SELECTED) ? UI_TEXT_PRIMARY
                                                     : UI_TEXT_SECONDARY;
        appui_label(pixels, w, h,
                    (struct appui_rect){12, row_y, 58, ROW_H}, pid,
                    UI_FONT_BODY, UI_TEXT_TERTIARY, UI_ALIGN_RIGHT);
        appui_label(pixels, w, h,
                    (struct appui_rect){82, row_y,
                                        appui_max(20, state_x - 90), ROW_H},
                    row->name, UI_FONT_BODY, fg, UI_ALIGN_LEFT);
        appui_label(pixels, w, h,
                    (struct appui_rect){state_x + 8, row_y,
                                        appui_max(20, cpu_x - state_x - 16),
                                        ROW_H},
                    row->state, UI_FONT_BODY, UI_TEXT_TERTIARY,
                    UI_ALIGN_LEFT);
        /* CPU cell: a hairline usage bar under the number reads faster than
         * the number alone when scanning a full table. */
        struct appui_rect cpu_cell = {cpu_x + 8, row_y,
                                      appui_max(20, memory_x - cpu_x - 16),
                                      ROW_H};
        appui_label(pixels, w, h, cpu_cell, cpu, UI_FONT_BODY, fg,
                    UI_ALIGN_RIGHT);
        if (cpu_cell.w >= CPU_BAR_MIN_W)
            appui_progress(pixels, w, h,
                           (struct appui_rect){cpu_cell.x, row_y + ROW_H - 7,
                                               cpu_cell.w, 3},
                           row->cpu_tenths, 1000);
        appui_label(pixels, w, h,
                    (struct appui_rect){memory_x + 8, row_y,
                                        appui_max(20, w - memory_x - 16 -
                                                      scroll_w),
                                        ROW_H},
                    memory, UI_FONT_BODY, fg, UI_ALIGN_RIGHT);
    }

    appui_scrollbar(pixels, w, h, table_scroll_track(), 1, table_content_px(),
                    table_viewport_px(), scroll_row * ROW_H,
                    scroll_dragging ||
                    appui_inside(pointer_x, pointer_y, table_scroll_track()));
}

static void draw_line(struct appui_rect clip, int x0, int y0,
                      int x1, int y1, uint32_t color) {
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
                               uint32_t color) {
    appui_card(pixels, w, h, area);
    struct appui_rect plot = {area.x + 10, area.y + 8,
                              area.w - 20, area.h - 16};
    for (int i = 1; i < 4; i++) {
        int gy = plot.y + plot.h * i / 4;
        appui_separator(pixels, w, h, plot.x, gy, plot.w, 0);
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
    struct appui_rect cpu_head = {16, top, appui_max(1, w - 32), 24};
    format_percent(value, system_cpu_tenths, sizeof(value));
    appui_label(pixels, w, h, cpu_head, "CPU history", UI_FONT_BODY_LG,
                UI_TEXT_PRIMARY, UI_ALIGN_LEFT);
    appui_label(pixels, w, h, cpu_head, value, UI_FONT_BODY_LG,
                UI_ACCENT_FILL, UI_ALIGN_RIGHT);
    draw_history_graph((struct appui_rect){12, top + 30, w - 24, graph_h},
                       cpu_history, UI_ACCENT_FILL);

    int memory_y = top + 42 + graph_h;
    struct appui_rect mem_head = {16, memory_y, appui_max(1, w - 32), 24};
    value[0] = 0;
    format_memory(value, memory_used_kb, sizeof(value));
    appui_append_text(value, " of ", sizeof(value));
    char total[24];
    format_memory(total, memory_total_kb, sizeof(total));
    appui_append_text(value, total, sizeof(value));
    appui_label(pixels, w, h, mem_head, "Memory history", UI_FONT_BODY_LG,
                UI_TEXT_PRIMARY, UI_ALIGN_LEFT);
    appui_label(pixels, w, h, mem_head, value, UI_FONT_BODY_LG,
                UI_SYS_SUCCESS, UI_ALIGN_RIGHT);
    draw_history_graph((struct appui_rect){12, memory_y + 30,
                                           w - 24, graph_h},
                       memory_history, UI_SYS_SUCCESS);
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
    appui_scrim(pixels, w, h);
    struct appui_rect box = confirm_box();
    appui_card(pixels, w, h, box);
    appui_stroke_round(pixels, w, h, box, UI_RADIUS_OVERLAY,
                       UI_SYS_CRITICAL);
    appui_label(pixels, w, h,
                (struct appui_rect){box.x + 18, box.y + 14, box.w - 36, 30},
                "End this process?", UI_FONT_TITLE, UI_TEXT_PRIMARY,
                UI_ALIGN_LEFT);
    char message[112] = "Terminate PID ";
    appui_append_int(message, confirm_pid, sizeof(message));
    appui_append_text(message, " and all of its threads?", sizeof(message));
    appui_label(pixels, w, h,
                (struct appui_rect){box.x + 18, box.y + 56, box.w - 36, 24},
                message, UI_FONT_BODY, UI_TEXT_SECONDARY, UI_ALIGN_LEFT);
    appui_label(pixels, w, h,
                (struct appui_rect){box.x + 18, box.y + 80, box.w - 36, 24},
                "Unsaved work may be lost.", UI_FONT_BODY,
                UI_TEXT_SECONDARY, UI_ALIGN_LEFT);
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
    appui_fill(pixels, w, h, (struct appui_rect){0, 0, w, h}, UI_BG_SOLID);
    appui_toolbar(pixels, w, h, (struct appui_rect){0, 0, w, TOOLBAR_H});
    appui_label(pixels, w, h,
                (struct appui_rect){16, 12, appui_max(1, w - 256), 34},
                "System Monitor", UI_FONT_TITLE, UI_TEXT_PRIMARY,
                UI_ALIGN_LEFT);
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
               (struct appui_rect){0, TOOLBAR_H, w, TABS_H}, UI_BG_SOLID);
    appui_separator(pixels, w, h, 0, TOOLBAR_H + TABS_H - 1, w, 0);
    appui_tab(pixels, w, h, tab_rect(TAB_PROCESSES), "Processes",
              active_tab == TAB_PROCESSES,
              appui_pointer_state(tab_rect(TAB_PROCESSES), pointer_x,
                                  pointer_y, pointer_buttons));
    appui_tab(pixels, w, h, tab_rect(TAB_RESOURCES), "Resources",
              active_tab == TAB_RESOURCES,
              appui_pointer_state(tab_rect(TAB_RESOURCES), pointer_x,
                                  pointer_y, pointer_buttons));

    if (active_tab == TAB_PROCESSES) {
        draw_summary();
        draw_process_table();
        appui_fill(pixels, w, h,
                   (struct appui_rect){0, h - FOOTER_H, w, FOOTER_H},
                   UI_BG_LAYER);
        appui_separator(pixels, w, h, 0, h - FOOTER_H, w, 0);
        appui_label(pixels, w, h,
                    (struct appui_rect){14, h - FOOTER_H,
                                        appui_max(1, w - 220), FOOTER_H},
                    status_text, UI_FONT_BODY, UI_TEXT_SECONDARY,
                    UI_ALIGN_LEFT);
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
        appui_label(pixels, w, h,
                    (struct appui_rect){14, h - FOOTER_H,
                                        appui_max(1, w - 28), FOOTER_H},
                    footer, UI_FONT_BODY, UI_TEXT_SECONDARY, UI_ALIGN_LEFT);
    }
    draw_confirmation();
}

/* ------------------------------------------------------------------ */
/* GPU display list                                                    */
/* ------------------------------------------------------------------ */

/*
 * When the desktop owns a virgl context it renders this window from a bounded
 * command list instead of a pixel buffer.  Layout and hit-testing are shared:
 * the same *_rect() helpers drive both paths, so a click always lands where
 * the GPU drew.  Only painting differs, and in two ways worth knowing:
 *
 *   - Commands carry no alpha, so shadows, the modal scrim and hairline
 *     strokes are approximated with opaque tones.
 *   - The list is bounded, and guiapp drops commands silently once it is
 *     full.  Table rows are therefore capped against the remaining budget:
 *     losing the tail of a long process list is recoverable, losing the
 *     confirmation dialog would not be.
 */
enum {
    /* appui scales a 28-row glyph cell by ui_font_scale_pct and crops to the
     * ink; the GPU atlas scales the whole cell to the requested size.  A CPU
     * font token therefore maps to 28 * pct / 100 pixels, which lands the
     * same ink height on both paths. */
    GPU_FONT_CAPTION = 16,  /* UI_FONT_CAPTION,  58% */
    GPU_FONT_BODY = 19,     /* UI_FONT_BODY,     68% */
    GPU_FONT_BODY_LG = 22,  /* UI_FONT_BODY_LG,  79% */
    GPU_FONT_TITLE = 35,    /* UI_FONT_TITLE,   125% */
    /* Budget accounting for the process table.  A row is five labels plus the
     * two rects of its CPU bar; selection and hover add a fill and an accent
     * bar to at most two rows.  The tail covers everything canvas_row_budget()
     * cannot see because it is emitted after the cap is chosen: the column
     * header, the scrollbar, the footer and those per-row extras. */
    GPU_ROW_COMMANDS = 7,
    GPU_ROW_BYTES = 52,
    GPU_TAIL_COMMANDS = 24,
    GPU_TAIL_BYTES = 160,
    GPU_DIALOG_COMMANDS = 12,
    GPU_DIALOG_BYTES = 192,
    GPU_CARET_H = 6,
};

static void canvas_box(struct guiapp_canvas *canvas, struct appui_rect r,
                       int radius, uint32_t color) {
    (void)guiapp_canvas_rect(canvas, r.x, r.y, r.w, r.h, radius, color);
}

/* appui centres the glyph ink in its box; the GPU centres the full cell, whose
 * descender space sits below the ink.  Growing a tight box to the cell height
 * keeps the same optical centre and stops the GPU scissor from shearing the
 * bottom off large text. */
static void canvas_label(struct guiapp_canvas *canvas, struct appui_rect r,
                         const char *text, int size, uint32_t color,
                         uint16_t flags) {
    if (r.h < size) {
        r.y -= (size - r.h) / 2;
        r.h = size;
    }
    (void)guiapp_canvas_text(canvas, r.x, r.y, r.w, r.h, text, size, color,
                             flags);
}

static void canvas_separator(struct guiapp_canvas *canvas, int x, int y,
                             int extent) {
    canvas_box(canvas, (struct appui_rect){x, y, extent, 1}, 0,
               UI_STROKE_DIVIDER);
}

/* Ring plus fill: there is no stroke primitive, so the outer rect in the edge
 * colour is what remains visible as a 1px border. */
static void canvas_frame(struct guiapp_canvas *canvas, struct appui_rect r,
                         int radius, uint32_t fill, uint32_t edge) {
    canvas_box(canvas, r, radius, edge);
    canvas_box(canvas,
               (struct appui_rect){r.x + 1, r.y + 1, r.w - 2, r.h - 2},
               radius > 0 ? radius - 1 : 0, fill);
}

static void canvas_card(struct guiapp_canvas *canvas, struct appui_rect r) {
    canvas_frame(canvas, r, UI_RADIUS_OVERLAY, UI_BG_LAYER,
                 UI_STROKE_CONTROL);
}

/* Colour table copied from appui_button_ex so both paths agree on state. */
static void canvas_button(struct guiapp_canvas *canvas, struct appui_rect r,
                          const char *label, int variant, int state) {
    int disabled = (state & APPUI_STATE_DISABLED) != 0;
    int hovered = (state & APPUI_STATE_HOVERED) != 0 && !disabled;
    int pressed = (state & APPUI_STATE_PRESSED) != 0 && !disabled;
    uint32_t bg, edge, fg;

    if (disabled) {
        bg = UI_CTRL_DISABLED;
        edge = UI_STROKE_CONTROL;
        fg = UI_TEXT_DISABLED;
    } else if (variant == APPUI_BTN_PRIMARY) {
        bg = pressed ? UI_ACCENT_FILL_PRESS
                     : (hovered ? UI_ACCENT_FILL_HOVER : UI_ACCENT_FILL);
        edge = UI_ACCENT_DARK1;
        fg = UI_TEXT_ON_ACCENT;
    } else if (variant == APPUI_BTN_DANGER) {
        bg = pressed ? plt_shade(UI_SYS_CRITICAL, 70)
                     : (hovered ? UI_SYS_CRITICAL
                                : plt_shade(UI_SYS_CRITICAL, 85));
        edge = UI_SYS_CRITICAL;
        fg = UI_TEXT_ON_ACCENT;
    } else {
        bg = pressed ? UI_CTRL_PRESSED
                     : (hovered ? UI_CTRL_HOVER : UI_CTRL_REST);
        edge = UI_STROKE_CONTROL;
        fg = pressed ? UI_TEXT_SECONDARY : UI_TEXT_PRIMARY;
    }
    canvas_frame(canvas, r, UI_RADIUS_CONTROL, bg, edge);
    canvas_label(canvas, r, label, GPU_FONT_BODY, fg,
                 GUIAPP_CANVAS_ALIGN_CENTER);
}

static void canvas_pointer_button(struct guiapp_canvas *canvas,
                                  struct appui_rect r, const char *label,
                                  int variant, int extra_state) {
    canvas_button(canvas, r, label, variant,
                  appui_pointer_state(r, pointer_x, pointer_y,
                                      pointer_buttons) | extra_state);
}

static void canvas_tab(struct guiapp_canvas *canvas, int tab,
                       const char *label) {
    struct appui_rect r = tab_rect(tab);
    int active = active_tab == tab;
    int hovered = (appui_pointer_state(r, pointer_x, pointer_y,
                                       pointer_buttons) &
                   APPUI_STATE_HOVERED) != 0;
    if (active)
        canvas_box(canvas, r, UI_RADIUS_CONTROL, UI_BG_LAYER);
    else if (hovered)
        canvas_box(canvas, r, UI_RADIUS_CONTROL, UI_SUBTLE_PRESSED);
    canvas_label(canvas, r, label, GPU_FONT_BODY,
                 active ? UI_TEXT_PRIMARY : UI_TEXT_SECONDARY,
                 GUIAPP_CANVAS_ALIGN_CENTER);
    if (active)
        canvas_box(canvas,
                   (struct appui_rect){r.x + r.w / 4, r.y + r.h - 3,
                                       r.w / 2, 3},
                   1, UI_ACCENT_FILL);
}

static void canvas_chrome(struct guiapp_canvas *canvas) {
    canvas_box(canvas, (struct appui_rect){0, 0, w, h}, 0, UI_BG_SOLID);
    canvas_box(canvas, (struct appui_rect){0, 0, w, TOOLBAR_H}, 0,
               UI_BG_LAYER);
    canvas_separator(canvas, 0, TOOLBAR_H - 1, w);
    canvas_label(canvas,
                 (struct appui_rect){16, 12, appui_max(1, w - 256), 34},
                 "System Monitor", GPU_FONT_TITLE, UI_TEXT_PRIMARY,
                 GUIAPP_CANVAS_ALIGN_LEFT);
    canvas_pointer_button(canvas, refresh_button_rect(), "Refresh",
                          APPUI_BTN_DEFAULT, 0);
    canvas_pointer_button(canvas, pause_button_rect(),
                          paused ? "Resume" : "Pause",
                          paused ? APPUI_BTN_PRIMARY : APPUI_BTN_DEFAULT, 0);
    canvas_separator(canvas, 0, TOOLBAR_H + TABS_H - 1, w);
    canvas_tab(canvas, TAB_PROCESSES, "Processes");
    canvas_tab(canvas, TAB_RESOURCES, "Resources");
}

static void canvas_summary_card(struct guiapp_canvas *canvas,
                                struct appui_rect area, const char *label,
                                const char *value, uint32_t accent,
                                int tenths) {
    canvas_card(canvas, area);
    canvas_label(canvas,
                 (struct appui_rect){area.x + 12, area.y + 6, area.w - 24, 20},
                 label, GPU_FONT_CAPTION, UI_TEXT_TERTIARY,
                 GUIAPP_CANVAS_ALIGN_LEFT);
    canvas_label(canvas,
                 (struct appui_rect){area.x + 12, area.y + 26, area.w - 24,
                                     24},
                 value, GPU_FONT_TITLE, UI_TEXT_PRIMARY,
                 GUIAPP_CANVAS_ALIGN_LEFT);
    if (tenths >= 0) {
        struct appui_rect track = {area.x + 12, area.y + area.h - 12,
                                   area.w - 24, 4};
        canvas_box(canvas, track, track.h / 2, UI_CTRL_REST);
        canvas_box(canvas,
                   (struct appui_rect){track.x, track.y,
                                       track.w * clamp_int(tenths, 0, 1000) /
                                           1000,
                                       track.h},
                   track.h / 2, accent);
    }
}

static void canvas_summary(struct guiapp_canvas *canvas) {
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
    canvas_summary_card(canvas, (struct appui_rect){12, y, card_w, 66}, "CPU",
                        cpu, UI_ACCENT_FILL, system_cpu_tenths);
    canvas_summary_card(canvas,
                        (struct appui_rect){12 + card_w + gap, y, card_w, 66},
                        "Memory", memory, UI_SYS_SUCCESS, memory_tenths);
    canvas_summary_card(canvas,
                        (struct appui_rect){12 + (card_w + gap) * 2, y,
                                            w - 24 - (card_w + gap) * 2, 66},
                        "Processes", count, UI_SYS_CAUTION, -1);
}

/* Stand-in for UI_ICON_CHEVRON_*: three tapered bars read as a caret at this
 * size, and rectangles are all the display list offers. */
static void canvas_sort_caret(struct guiapp_canvas *canvas, int x, int y,
                              int down) {
    for (int i = 0; i < 3; i++) {
        int step = down ? i : 2 - i;
        canvas_box(canvas,
                   (struct appui_rect){x + step * 3 / 2, y + i * 2,
                                       HEADER_CHEVRON - step * 3, 2},
                   0, UI_ACCENT_FILL);
    }
}

static void canvas_header_label(struct guiapp_canvas *canvas, int column,
                                const char *label) {
    struct appui_rect cell = header_cell(column);
    int active = sort_column == column;
    int label_w = appui_label_width(label, UI_FONT_CAPTION);
    int text_w = appui_min(label_w, appui_max(1, cell.w - 16));
    canvas_label(canvas,
                 (struct appui_rect){cell.x + 8, cell.y, text_w, cell.h},
                 label, GPU_FONT_CAPTION,
                 active ? UI_TEXT_PRIMARY : UI_TEXT_TERTIARY,
                 GUIAPP_CANVAS_ALIGN_LEFT);
    if (active && cell.w - 16 - text_w >= HEADER_CHEVRON)
        canvas_sort_caret(canvas, cell.x + 10 + text_w,
                          cell.y + (cell.h - GPU_CARET_H) / 2,
                          sort_descending);
}

static void canvas_scrollbar(struct guiapp_canvas *canvas) {
    struct appui_rect track = table_scroll_track();
    struct appui_rect thumb;
    int hot, inset;
    if (!table_scrollable())
        return;
    hot = scroll_dragging || appui_inside(pointer_x, pointer_y, track);
    thumb = table_scroll_thumb();
    if (hot)
        canvas_box(canvas, track, track.w / 2, UI_BG_MICA_ALT);
    inset = hot ? 2 : 3;
    canvas_box(canvas,
               (struct appui_rect){thumb.x + inset, thumb.y + 2,
                                   thumb.w - 2 * inset, thumb.h - 4},
               3, hot ? UI_TEXT_SECONDARY : UI_TEXT_TERTIARY);
}

static void canvas_process_table(struct guiapp_canvas *canvas, int rows) {
    int y = table_y();
    int body_y = y + TABLE_HEADER_H;
    int scroll_w = table_scrollable() ? APPUI_SCROLL_W : 0;
    int state_x = header_cell(SORT_STATE).x;
    int cpu_x = header_cell(SORT_CPU).x;
    int memory_x = header_cell(SORT_MEMORY).x;

    canvas_box(canvas, (struct appui_rect){8, y, w - 16, TABLE_HEADER_H}, 0,
               UI_BG_LAYER);
    canvas_header_label(canvas, SORT_PID, "PID");
    canvas_header_label(canvas, SORT_NAME, "Process");
    canvas_header_label(canvas, SORT_STATE, "State");
    canvas_header_label(canvas, SORT_CPU, "CPU");
    canvas_header_label(canvas, SORT_MEMORY, "Memory");
    canvas_separator(canvas, 8, y + TABLE_HEADER_H - 1, w - 16);
    canvas_box(canvas,
               (struct appui_rect){8, body_y, w - 16,
                                   visible_rows() * ROW_H},
               0, UI_BG_SOLID);

    for (int shown = 0; shown < rows; shown++) {
        int index = scroll_row + shown;
        if (index >= process_count)
            break;
        struct process_row *row = &processes[index];
        int row_y = body_y + shown * ROW_H;
        struct appui_rect row_rect = {8, row_y, w - 16 - scroll_w, ROW_H};
        int state = appui_pointer_state(row_rect, pointer_x, pointer_y,
                                        pointer_buttons);
        int selected = row->pid == selected_pid;
        uint32_t fg = selected ? UI_TEXT_PRIMARY : UI_TEXT_SECONDARY;
        struct appui_rect cpu_cell = {cpu_x + 8, row_y,
                                      appui_max(20, memory_x - cpu_x - 16),
                                      ROW_H};
        char pid[16], cpu[20], memory[24];

        if (selected)
            canvas_box(canvas, row_rect, UI_RADIUS_CONTROL, UI_SUBTLE_HOVER);
        else if (state & (APPUI_STATE_HOVERED | APPUI_STATE_PRESSED))
            canvas_box(canvas, row_rect, UI_RADIUS_CONTROL,
                       UI_SUBTLE_PRESSED);
        if (selected)
            canvas_box(canvas,
                       (struct appui_rect){row_rect.x + 1, row_y + ROW_H / 4,
                                           3, ROW_H / 2},
                       1, UI_ACCENT_FILL);

        pid[0] = 0;
        appui_append_int(pid, row->pid, sizeof(pid));
        format_percent(cpu, row->cpu_tenths, sizeof(cpu));
        format_memory(memory, row->rss_kb, sizeof(memory));
        canvas_label(canvas, (struct appui_rect){12, row_y, 58, ROW_H}, pid,
                     GPU_FONT_BODY, UI_TEXT_TERTIARY,
                     GUIAPP_CANVAS_ALIGN_RIGHT);
        canvas_label(canvas,
                     (struct appui_rect){82, row_y,
                                         appui_max(20, state_x - 90), ROW_H},
                     row->name, GPU_FONT_BODY, fg, GUIAPP_CANVAS_ALIGN_LEFT);
        canvas_label(canvas,
                     (struct appui_rect){state_x + 8, row_y,
                                         appui_max(20,
                                                   cpu_x - state_x - 16),
                                         ROW_H},
                     row->state, GPU_FONT_BODY, UI_TEXT_TERTIARY,
                     GUIAPP_CANVAS_ALIGN_LEFT);
        canvas_label(canvas, cpu_cell, cpu, GPU_FONT_BODY, fg,
                     GUIAPP_CANVAS_ALIGN_RIGHT);
        if (cpu_cell.w >= CPU_BAR_MIN_W) {
            struct appui_rect bar = {cpu_cell.x, row_y + ROW_H - 7,
                                     cpu_cell.w, 3};
            canvas_box(canvas, bar, bar.h / 2, UI_CTRL_REST);
            canvas_box(canvas,
                       (struct appui_rect){bar.x, bar.y,
                                           bar.w *
                                               clamp_int(row->cpu_tenths, 0,
                                                         1000) / 1000,
                                           bar.h},
                       bar.h / 2, UI_ACCENT_FILL);
        }
        canvas_label(canvas,
                     (struct appui_rect){memory_x + 8, row_y,
                                         appui_max(20, w - memory_x - 16 -
                                                       scroll_w),
                                         ROW_H},
                     memory, GPU_FONT_BODY, fg, GUIAPP_CANVAS_ALIGN_RIGHT);
    }
    canvas_scrollbar(canvas);
}

static void canvas_footer(struct guiapp_canvas *canvas) {
    canvas_box(canvas, (struct appui_rect){0, h - FOOTER_H, w, FOOTER_H}, 0,
               UI_BG_LAYER);
    canvas_separator(canvas, 0, h - FOOTER_H, w);
    canvas_label(canvas,
                 (struct appui_rect){14, h - FOOTER_H,
                                     appui_max(1, w - 220), FOOTER_H},
                 status_text, GPU_FONT_BODY, UI_TEXT_SECONDARY,
                 GUIAPP_CANVAS_ALIGN_LEFT);
    canvas_pointer_button(canvas, end_button_rect(), "End Process",
                          APPUI_BTN_DANGER,
                          can_end_selected() ? 0 : APPUI_STATE_DISABLED);
}

/* The CPU path strokes a polyline; without a line primitive the same samples
 * become a filled column chart, which is also how the reference monitor
 * presents them. */
static void canvas_history_graph(struct guiapp_canvas *canvas,
                                 struct appui_rect area, const int *history,
                                 uint32_t color) {
    struct appui_rect plot = {area.x + 10, area.y + 8, area.w - 20,
                              area.h - 16};
    canvas_card(canvas, area);
    if (plot.w <= 0 || plot.h <= 0)
        return;
    for (int i = 1; i < 4; i++)
        canvas_separator(canvas, plot.x, plot.y + plot.h * i / 4, plot.w);
    for (int i = 0; i < HISTORY_SAMPLES; i++) {
        int x0 = plot.x + i * plot.w / HISTORY_SAMPLES;
        int x1 = plot.x + (i + 1) * plot.w / HISTORY_SAMPLES;
        int bar_h = history[i] * plot.h / 1000;
        if (x1 <= x0)
            x1 = x0 + 1;
        if (bar_h < 2)
            bar_h = 2; /* keep a visible baseline at 0% */
        canvas_box(canvas,
                   (struct appui_rect){x0, plot.y + plot.h - bar_h,
                                       x1 - x0, bar_h},
                   0, color);
    }
}

static void canvas_resources(struct guiapp_canvas *canvas) {
    int top = TOOLBAR_H + TABS_H + 10;
    int graph_h = appui_max(36, (h - top - 120) / 2);
    int memory_y = top + 42 + graph_h;
    struct appui_rect cpu_head = {16, top, appui_max(1, w - 32), 24};
    struct appui_rect mem_head = {16, memory_y, appui_max(1, w - 32), 24};
    char value[64], total[24], footer[80] = "Uptime ";

    format_percent(value, system_cpu_tenths, sizeof(value));
    canvas_label(canvas, cpu_head, "CPU history", GPU_FONT_BODY_LG,
                 UI_TEXT_PRIMARY, GUIAPP_CANVAS_ALIGN_LEFT);
    canvas_label(canvas, cpu_head, value, GPU_FONT_BODY_LG, UI_ACCENT_FILL,
                 GUIAPP_CANVAS_ALIGN_RIGHT);
    canvas_history_graph(canvas,
                         (struct appui_rect){12, top + 30, w - 24, graph_h},
                         cpu_history, UI_ACCENT_FILL);

    value[0] = 0;
    format_memory(value, memory_used_kb, sizeof(value));
    appui_append_text(value, " of ", sizeof(value));
    format_memory(total, memory_total_kb, sizeof(total));
    appui_append_text(value, total, sizeof(value));
    canvas_label(canvas, mem_head, "Memory history", GPU_FONT_BODY_LG,
                 UI_TEXT_PRIMARY, GUIAPP_CANVAS_ALIGN_LEFT);
    canvas_label(canvas, mem_head, value, GPU_FONT_BODY_LG, UI_SYS_SUCCESS,
                 GUIAPP_CANVAS_ALIGN_RIGHT);
    canvas_history_graph(canvas,
                         (struct appui_rect){12, memory_y + 30, w - 24,
                                             graph_h},
                         memory_history, UI_SYS_SUCCESS);

    format_uptime(value, sizeof(value));
    appui_append_text(footer, value, sizeof(footer));
    canvas_label(canvas,
                 (struct appui_rect){14, h - FOOTER_H, appui_max(1, w - 28),
                                     FOOTER_H},
                 footer, GPU_FONT_BODY, UI_TEXT_SECONDARY,
                 GUIAPP_CANVAS_ALIGN_LEFT);
}

/* No scrim: commands are opaque, so dimming the page would mean hiding it.
 * The critical ring plus the fact that nothing behind the dialog reacts to
 * input carries the modality instead. */
static void canvas_confirmation(struct guiapp_canvas *canvas) {
    struct appui_rect box = confirm_box();
    char message[112] = "Terminate PID ";

    canvas_frame(canvas, box, UI_RADIUS_OVERLAY, UI_BG_LAYER,
                 UI_SYS_CRITICAL);
    canvas_label(canvas,
                 (struct appui_rect){box.x + 18, box.y + 14, box.w - 36, 30},
                 "End this process?", GPU_FONT_TITLE, UI_TEXT_PRIMARY,
                 GUIAPP_CANVAS_ALIGN_LEFT);
    appui_append_int(message, confirm_pid, sizeof(message));
    appui_append_text(message, " and all of its threads?", sizeof(message));
    canvas_label(canvas,
                 (struct appui_rect){box.x + 18, box.y + 56, box.w - 36, 24},
                 message, GPU_FONT_BODY, UI_TEXT_SECONDARY,
                 GUIAPP_CANVAS_ALIGN_LEFT);
    canvas_label(canvas,
                 (struct appui_rect){box.x + 18, box.y + 80, box.w - 36, 24},
                 "Unsaved work may be lost.", GPU_FONT_BODY,
                 UI_TEXT_SECONDARY, GUIAPP_CANVAS_ALIGN_LEFT);
    canvas_pointer_button(canvas, confirm_button(0), "Cancel",
                          APPUI_BTN_DEFAULT, 0);
    canvas_pointer_button(canvas, confirm_button(1), "End Process",
                          APPUI_BTN_DANGER, 0);
}

/* Rows this frame can still afford, in both commands and string bytes. */
static int canvas_row_budget(const struct guiapp_canvas *canvas) {
    int reserve = GPU_TAIL_COMMANDS +
                  (confirm_pid >= 0 ? GPU_DIALOG_COMMANDS : 0);
    int reserve_bytes = GPU_TAIL_BYTES +
                        (confirm_pid >= 0 ? GPU_DIALOG_BYTES : 0);
    int by_command = GUIAPP_CANVAS_MAX_COMMANDS - (int)canvas->count -
                     reserve;
    int by_bytes = GUIAPP_CANVAS_STRING_BYTES - (int)canvas->string_bytes -
                   reserve_bytes;
    int rows = appui_min(by_command / GPU_ROW_COMMANDS,
                         by_bytes / GPU_ROW_BYTES);
    return appui_min(appui_max(0, rows), visible_rows());
}

static int render_canvas_frame(struct guiapp_ctx *ctx) {
    struct guiapp_canvas canvas;
    if (guiapp_canvas_begin(ctx, &canvas, w, h) < 0)
        return -1;
    canvas_chrome(&canvas);
    if (active_tab == TAB_PROCESSES) {
        canvas_summary(&canvas);
        canvas_process_table(&canvas, canvas_row_budget(&canvas));
        canvas_footer(&canvas);
    } else {
        canvas_resources(&canvas);
    }
    if (confirm_pid >= 0)
        canvas_confirmation(&canvas);
    return guiapp_canvas_present(&canvas, "System Monitor");
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
    if (table_scrollable() && appui_inside(x, y, table_scroll_track()))
        return;
    /* Bound the hit-test to the painted rows.  visible_rows() truncates, so
     * the strip between the last row and the footer used to map to
     * scroll_row + rows -- selecting a process that was never drawn there. */
    int body_y = table_y() + TABLE_HEADER_H;
    int scroll_w = table_scrollable() ? APPUI_SCROLL_W : 0;
    if (x >= 8 && x < w - 8 - scroll_w && y >= body_y &&
        y < body_y + table_viewport_px()) {
        int index = scroll_row + (y - body_y) / ROW_H;
        if (index >= 0 && index < process_count)
            selected_pid = processes[index].pid;
    }
}

/* Scroll offset in pixels, clamped, then converted back to whole rows. */
static void set_scroll_px(int offset_px) {
    int max_px = appui_max(0, table_content_px() - table_viewport_px());
    scroll_row = clamp_int(offset_px, 0, max_px) / ROW_H;
    clamp_scroll();
}

/* Both the press test and the drag use appui_scroll_thumb, the same geometry
 * appui_scrollbar paints, so the grabbable thumb cannot drift from it. */
static void handle_scroll_press(int x, int y) {
    struct appui_rect track = table_scroll_track();
    if (!table_scrollable() || !appui_inside(x, y, track))
        return;
    if (!appui_inside(x, y, table_scroll_thumb()))
        set_scroll_px(appui_scroll_offset_at(track, 1, table_content_px(),
                                             table_viewport_px(), y));
    scroll_dragging = 1;
    scroll_drag_mouse = y;
    scroll_drag_start_px = scroll_row * ROW_H;
}

/*
 * Return the pointer-driven visual region at a coordinate.  The canvas has
 * no retained widgets: publishing it rebuilds the whole display list, so a
 * raw mouse event is not by itself a reason to redraw.  Coordinates moving
 * inside one region leave every painted hover/pressed state unchanged.
 */
static int pointer_visual_region(int x, int y) {
    enum {
        POINTER_REGION_NONE = 0,
        POINTER_REGION_REFRESH = 1,
        POINTER_REGION_PAUSE = 2,
        POINTER_REGION_TAB_BASE = 10,
        POINTER_REGION_HEADER_BASE = 20,
        POINTER_REGION_END = 30,
        POINTER_REGION_SCROLL = 31,
        POINTER_REGION_ROW_BASE = 64,
        POINTER_REGION_DIALOG_BASE = 256,
    };
    int region = POINTER_REGION_NONE;

    if (appui_inside(x, y, refresh_button_rect()))
        region = POINTER_REGION_REFRESH;
    else if (appui_inside(x, y, pause_button_rect()))
        region = POINTER_REGION_PAUSE;
    else {
        for (int tab = TAB_PROCESSES; tab <= TAB_RESOURCES; tab++) {
            if (appui_inside(x, y, tab_rect(tab))) {
                region = POINTER_REGION_TAB_BASE + tab;
                break;
            }
        }
    }

    if (region == POINTER_REGION_NONE && active_tab == TAB_PROCESSES) {
        for (int column = SORT_PID; column <= SORT_MEMORY; column++) {
            if (appui_inside(x, y, header_cell(column))) {
                region = POINTER_REGION_HEADER_BASE + column;
                break;
            }
        }
        if (region == POINTER_REGION_NONE &&
            appui_inside(x, y, end_button_rect())) {
            region = POINTER_REGION_END;
        } else if (region == POINTER_REGION_NONE && table_scrollable() &&
                   (scroll_dragging ||
                    appui_inside(x, y, table_scroll_track()))) {
            region = POINTER_REGION_SCROLL;
        } else if (region == POINTER_REGION_NONE) {
            int body_y = table_y() + TABLE_HEADER_H;
            int scroll_w = table_scrollable() ? APPUI_SCROLL_W : 0;
            if (x >= 8 && x < w - 8 - scroll_w && y >= body_y &&
                y < body_y + table_viewport_px()) {
                int index = scroll_row + (y - body_y) / ROW_H;
                if (index >= 0 && index < process_count)
                    region = POINTER_REGION_ROW_BASE + index;
            }
        }
    }

    /* The modal is painted over the page, but the page is still generated
     * with pointer-aware controls.  Include both layers in the fingerprint
     * so entering a dialog button cannot leave either hover stale. */
    if (confirm_pid >= 0) {
        int dialog_region = 0;
        if (appui_inside(x, y, confirm_button(0)))
            dialog_region = 1;
        else if (appui_inside(x, y, confirm_button(1)))
            dialog_region = 2;
        region += POINTER_REGION_DIALOG_BASE * dialog_region;
    }
    return region;
}

static int handle_mouse(int x, int y, int buttons, int wheel) {
    int old_region = pointer_visual_region(pointer_x, pointer_y);
    int old_left = pointer_buttons & 1;
    int old_scroll_row = scroll_row;
    int old_dragging = scroll_dragging;

    pointer_x = x;
    pointer_y = y;
    pointer_buttons = buttons;
    if (active_tab == TAB_PROCESSES && confirm_pid < 0 && wheel) {
        scroll_row -= wheel * 3;
        clamp_scroll();
    }
    if ((buttons & 1) && !(previous_buttons & 1)) {
        if (active_tab == TAB_PROCESSES && confirm_pid < 0)
            handle_scroll_press(x, y);
        if (!scroll_dragging)
            handle_click(x, y);
    }
    if ((buttons & 1) && scroll_dragging) {
        struct appui_rect track = table_scroll_track();
        struct appui_rect thumb = table_scroll_thumb();
        int span = appui_max(1, track.h - thumb.h);
        int max_px = appui_max(0, table_content_px() - table_viewport_px());
        set_scroll_px(scroll_drag_start_px +
                      (y - scroll_drag_mouse) * max_px / span);
    }
    if (!(buttons & 1))
        scroll_dragging = 0;
    previous_buttons = buttons;

    /* Clicks can refresh data, select rows or change tabs.  Drags redraw
     * only after crossing a whole-row scroll boundary.  Ordinary motion is
     * reduced to hover-region transitions instead of one full GPU canvas
     * submission per input report. */
    return old_region != pointer_visual_region(pointer_x, pointer_y) ||
           old_left != (buttons & 1) || old_scroll_row != scroll_row ||
           old_dragging != scroll_dragging || wheel != 0;
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
    int gpu_mode;
    if (guiapp_parse_args(argc, argv, &ctx) < 0)
        return 1;
    own_pid = getpid();
    refresh_data();
    gpu_mode = guiapp_has_capability(&ctx, GUIAPP_CAP_GPU_CANVAS);
    for (;;) {
        int needs_render = 0;
        if (guiapp_read_event(&ctx, &event) < 0 ||
            event.type == GUIAPP_EVT_CLOSE)
            break;
        if (event.type == GUIAPP_EVT_INIT ||
            event.type == GUIAPP_EVT_RESIZE) {
            w = clamp_int(event.width, 420, MAX_W);
            h = clamp_int(event.height, 300, MAX_H);
            clamp_scroll();
            needs_render = 1;
        } else if (event.type == GUIAPP_EVT_MOUSE) {
            needs_render = handle_mouse(event.x, event.y, event.buttons,
                                        event.wheel);
        } else if (event.type == GUIAPP_EVT_KEY && event.buttons) {
            handle_key(event.key);
            needs_render = 1;
        } else if (event.type == GUIAPP_EVT_TICK && !paused &&
                   (uint32_t)(monotonic_ms() - last_refresh_ms) >= 1000u) {
            refresh_data();
            clamp_scroll();
            needs_render = 1;
        }
        /* Capability changes arrive as an ordinary wake-up.  They still
         * force one frame so the backing representation switches promptly,
         * but unrelated wake-ups no longer rebuild an unchanged canvas. */
        int next_gpu_mode =
            guiapp_has_capability(&ctx, GUIAPP_CAP_GPU_CANVAS);
        if (next_gpu_mode != gpu_mode) {
            gpu_mode = next_gpu_mode;
            needs_render = 1;
        }
        if (!needs_render)
            continue;
        if (gpu_mode) {
            /* The CPU surface is dead weight while the desktop rasterises. */
            if (pixels) {
                free(pixels);
                pixels = 0;
                pixels_cap = 0;
            }
            if (render_canvas_frame(&ctx) < 0)
                break;
        } else {
            if (appui_pixels_ensure(&pixels, &pixels_cap, w, h, MAX_W,
                                    MAX_H) < 0)
                break;
            render();
            if (guiapp_send_frame(&ctx, "System Monitor", w, h, pixels) < 0)
                break;
        }
    }
    free(pixels);
    return 0;
}
