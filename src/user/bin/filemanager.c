#include "appui.h"
#include "guiapp.h"
#include "libc.h"

enum {
    MAX_W = GUIAPP_MAX_W,
    MAX_H = GUIAPP_MAX_H,
    MAX_ENTRIES = 128,
    PATH_CAP = GUIAPP_PATH_MAX,
    ROW_H = 34,
    TOOLBAR_H = 94,
    FOOTER_H = 32,
    LIST_HEADER_H = 34,
    SIDEBAR_ROW_STEP = 38,
    SIDEBAR_ROW_H = 34,
    CONTROL_H = 34,
    DIALOG_H = 144,
    DIALOG_FIELD_Y = 44,
    DIALOG_ACTION_Y = 96,
    MODE_NONE = 0,
    MODE_NEW_DIR,
    MODE_NEW_FILE,
    MODE_RENAME,
    MODE_DELETE,
    MAX_HISTORY = 16,
};

struct file_entry {
    char name[24];
    uint32_t type;
    uint32_t size;
};

static uint8_t pixels[MAX_W * MAX_H];
static struct file_entry entries[MAX_ENTRIES];
static int entry_count;
static int selected = -1;
static int scroll_row;
static int previous_click = -1;
static int prev_buttons;
static int pointer_x = -1;
static int pointer_y = -1;
static int pointer_buttons;
static int w = 560;
static int h = 360;
static char current_path[PATH_CAP] = "/fs";
static char history[MAX_HISTORY][PATH_CAP] = {"/fs"};
static int history_count = 1;
static int history_pos;
static char status[96] = "Ready";
static int input_mode;
static char input_text[24];
static int input_len;

static int clamp_int(int value, int lo, int hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static void set_status(const char *text) {
    appui_copy_text(status, text, sizeof(status));
}

static int is_valid_name(const char *name) {
    if (!name || !name[0] || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return 0;
    for (int i = 0; name[i]; i++)
        if (name[i] == '/')
            return 0;
    return 1;
}

static int join_path(char *out, const char *dir, const char *name) {
    int need = (int)strlen(dir) + (strcmp(dir, "/") == 0 ? 0 : 1) +
               (int)strlen(name) + 1;
    if (need > PATH_CAP)
        return -1;
    appui_copy_text(out, dir, PATH_CAP);
    if (strcmp(dir, "/") != 0)
        appui_append_text(out, "/", PATH_CAP);
    appui_append_text(out, name, PATH_CAP);
    return 0;
}

static void parent_path(char *path) {
    int len = (int)strlen(path);
    if (len <= 1)
        return;
    while (len > 1 && path[len - 1] == '/')
        path[--len] = 0;
    while (len > 1 && path[len - 1] != '/')
        path[--len] = 0;
    if (len > 1)
        path[len - 1] = 0;
    else
        path[1] = 0;
}

static int entry_before(const struct file_entry *a, const struct file_entry *b) {
    int ad = a->type == DT_DIR;
    int bd = b->type == DT_DIR;
    if (ad != bd)
        return ad > bd;
    return strcmp(a->name, b->name) < 0;
}

static void sort_entries(void) {
    for (int i = 1; i < entry_count; i++) {
        struct file_entry value = entries[i];
        int j = i;
        while (j > 0 && entry_before(&value, &entries[j - 1])) {
            entries[j] = entries[j - 1];
            j--;
        }
        entries[j] = value;
    }
}

static int visible_rows(void) {
    int rows = (h - TOOLBAR_H - LIST_HEADER_H - FOOTER_H) / ROW_H;
    return rows > 0 ? rows : 1;
}

static void clamp_selection(void) {
    if (entry_count <= 0) {
        selected = -1;
        scroll_row = 0;
        return;
    }
    selected = clamp_int(selected, 0, entry_count - 1);
    int visible = visible_rows();
    int max_scroll = appui_max(0, entry_count - visible);
    scroll_row = clamp_int(scroll_row, 0, max_scroll);
    if (selected < scroll_row)
        scroll_row = selected;
    if (selected >= scroll_row + visible)
        scroll_row = selected - visible + 1;
}

static int scan_directory(void) {
    int fd = open(current_path, O_RDONLY);
    if (fd < 0) {
        set_status("Cannot open directory");
        return -1;
    }
    entry_count = 0;
    struct dirent block[8];
    for (;;) {
        int bytes = getdents(fd, block, sizeof(block));
        if (bytes <= 0)
            break;
        int count = bytes / (int)sizeof(block[0]);
        for (int i = 0; i < count && entry_count < MAX_ENTRIES; i++) {
            if (!block[i].d_name[0] || strcmp(block[i].d_name, ".") == 0 ||
                strcmp(block[i].d_name, "..") == 0)
                continue;
            appui_copy_text(entries[entry_count].name, block[i].d_name,
                            sizeof(entries[entry_count].name));
            entries[entry_count].type = block[i].d_type;
            entries[entry_count].size = block[i].d_size;
            entry_count++;
        }
    }
    close(fd);
    sort_entries();
    selected = entry_count > 0 ? 0 : -1;
    scroll_row = 0;
    previous_click = -1;
    set_status("Ready");
    return 0;
}

static void show_path(const char *path, int remember) {
    struct stat st;
    if (!path || stat(path, &st) < 0 || (st.st_mode & S_IFMT) != S_IFDIR) {
        set_status("Directory not found");
        return;
    }
    appui_copy_text(current_path, path, sizeof(current_path));
    if (remember && strcmp(history[history_pos], current_path) != 0) {
        if (history_pos + 1 < MAX_HISTORY) {
            history_pos++;
        } else {
            for (int i = 1; i < MAX_HISTORY; i++)
                appui_copy_text(history[i - 1], history[i], PATH_CAP);
        }
        appui_copy_text(history[history_pos], current_path, PATH_CAP);
        history_count = history_pos + 1;
    }
    (void)scan_directory();
}

static void go_to(const char *path) {
    show_path(path, 1);
}

static void go_back(void) {
    if (history_pos <= 0) {
        set_status("No previous location");
        return;
    }
    history_pos--;
    show_path(history[history_pos], 0);
}

static void go_parent(void) {
    char path[PATH_CAP];
    appui_copy_text(path, current_path, sizeof(path));
    parent_path(path);
    go_to(path);
}

static void selected_path(char *path) {
    path[0] = 0;
    if (selected >= 0 && selected < entry_count)
        (void)join_path(path, current_path, entries[selected].name);
}

static int is_elf_file(const char *path) {
    uint8_t magic[4];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    int n = read(fd, magic, sizeof(magic));
    close(fd);
    return n == 4 && magic[0] == 0x7Fu && magic[1] == 'E' &&
           magic[2] == 'L' && magic[3] == 'F';
}

static int has_gui_manifest(const char *path) {
    char manifest[PATH_CAP];
    struct stat st;
    appui_copy_text(manifest, path, sizeof(manifest));
    appui_append_text(manifest, ".app", sizeof(manifest));
    return stat(manifest, &st) == 0 && st.st_type == DT_REG;
}

static int has_extension(const char *path, const char *extension) {
    size_t path_len = strlen(path);
    size_t extension_len = strlen(extension);
    if (path_len < extension_len) return 0;
    const char *tail = path + path_len - extension_len;
    for (size_t i = 0; i < extension_len; i++) {
        char a = tail[i], b = extension[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

static void open_selected(struct guiapp_ctx *ctx) {
    if (selected < 0 || selected >= entry_count)
        return;
    char path[PATH_CAP];
    if (join_path(path, current_path, entries[selected].name) < 0) {
        set_status("Path is too long");
        return;
    }
    if (entries[selected].type == DT_DIR) {
        go_to(path);
        return;
    }
    if (entries[selected].type != DT_REG) {
        set_status("This item cannot be opened");
        return;
    }
    /* Match by extension before ELF sniffing so ROM/audio assets never go
     * through the generic exec path (even if a header looks odd). */
    if (has_extension(path, ".gb") || has_extension(path, ".gbc")) {
        if (guiapp_request_launch(ctx, "/fs/apps/gameboy", path) < 0)
            set_status("Could not launch Game Boy");
        else
            set_status("Opened in Game Boy");
        return;
    }
    if (has_extension(path, ".mp3") || has_extension(path, ".wav")) {
        if (guiapp_request_launch(ctx, "/fs/apps/music", path) < 0)
            set_status("Could not launch Music");
        else
            set_status("Opened in Music");
        return;
    }
    if (is_elf_file(path)) {
        if (strcmp(current_path, "/fs/apps") == 0 && has_gui_manifest(path)) {
            if (guiapp_request_launch(ctx, path, 0) < 0)
                set_status("Could not launch GUI app");
            else
                set_status("Launched GUI app");
        } else {
            if (guiapp_request_exec(ctx, path) < 0)
                set_status("Could not execute program");
            else
                set_status("Running in Terminal");
        }
        return;
    }
    if (guiapp_request_launch(ctx, "/fs/apps/textedit", path) < 0) {
        set_status("Could not launch TextEdit");
        return;
    }
    appui_copy_text(status, "Opened in TextEdit: ", sizeof(status));
    appui_append_text(status, entries[selected].name, sizeof(status));
}

static void begin_input(int mode) {
    if ((mode == MODE_RENAME || mode == MODE_DELETE) &&
        (selected < 0 || selected >= entry_count)) {
        set_status("Select an item first");
        return;
    }
    input_mode = mode;
    input_len = 0;
    input_text[0] = 0;
    if (mode == MODE_RENAME) {
        appui_copy_text(input_text, entries[selected].name, sizeof(input_text));
        input_len = (int)strlen(input_text);
    }
}

static void finish_input(void) {
    char path[PATH_CAP];
    char old_path[PATH_CAP];
    int rc = -1;
    const char *result = "Operation failed";
    if (input_mode != MODE_DELETE && !is_valid_name(input_text)) {
        set_status("Enter a valid name");
        return;
    }
    if (input_mode == MODE_NEW_DIR) {
        if (join_path(path, current_path, input_text) == 0)
            rc = mkdir(path);
        result = rc == 0 ? "Folder created" : "Could not create folder";
    } else if (input_mode == MODE_NEW_FILE) {
        if (join_path(path, current_path, input_text) == 0)
            rc = create(path);
        result = rc == 0 ? "File created" : "Could not create file";
    } else if (input_mode == MODE_RENAME) {
        selected_path(old_path);
        if (old_path[0] && join_path(path, current_path, input_text) == 0)
            rc = rename(old_path, path);
        result = rc == 0 ? "Item renamed" : "Could not rename item";
    } else if (input_mode == MODE_DELETE) {
        selected_path(path);
        if (path[0])
            rc = entries[selected].type == DT_DIR ? rmdir(path) : unlink(path);
        result = rc == 0 ? "Item deleted" : "Could not delete item";
    }
    input_mode = MODE_NONE;
    (void)scan_directory();
    set_status(result);
}

static struct appui_rect content_rect(void) {
    return (struct appui_rect){0, TOOLBAR_H, w,
                               appui_max(1, h - TOOLBAR_H - FOOTER_H)};
}

static int sidebar_width(void) {
    /* The 15px-wide font needs more room for labels such as "Devices".
     * Hide the rail before squeezing it: the file list is the primary
     * content and remains more useful than a clipped sidebar. */
    return w >= 520 ? 132 : 0;
}

static struct appui_rect list_rect(void) {
    struct appui_rect c = content_rect();
    int side = sidebar_width();
    return (struct appui_rect){side + 8, c.y, w - side - 16, c.h};
}

static void draw_icon(int x, int y, int type, int selected_row) {
    int edge = selected_row ? THEME_ACCENT : appui_gray(8);
    if (type == DT_DIR) {
        appui_fill(pixels, w, h, (struct appui_rect){x + 1, y + 3, 8, 4},
                   appui_rgb6(5, 4, 0));
        appui_fill(pixels, w, h, (struct appui_rect){x, y + 6, 18, 13},
                   appui_rgb6(5, 4, 0));
        appui_border(pixels, w, h, (struct appui_rect){x, y + 6, 18, 13},
                     edge, appui_gray(2));
    } else {
        appui_fill(pixels, w, h, (struct appui_rect){x + 2, y + 1, 14, 18}, 15);
        appui_border(pixels, w, h, (struct appui_rect){x + 2, y + 1, 14, 18},
                     edge, appui_gray(3));
        appui_fill(pixels, w, h, (struct appui_rect){x + 5, y + 6, 8, 1},
                   appui_gray(6));
        appui_fill(pixels, w, h, (struct appui_rect){x + 5, y + 10, 8, 1},
                   appui_gray(6));
    }
}

static void draw_size(char *out, uint32_t size) {
    out[0] = 0;
    if (size >= 1024u) {
        appui_append_int(out, (int)(size / 1024u), 20);
        appui_append_text(out, " KiB", 20);
    } else {
        appui_append_int(out, (int)size, 20);
        appui_append_text(out, " B", 20);
    }
}

static void draw_text_tail(int x, int y, const char *text, int color,
                           struct appui_rect clip) {
    int available = clip.x + clip.w - x;
    int width = appui_text_width(text);
    if (available <= 0)
        return;
    if (width <= available) {
        appui_text(pixels, w, h, x, y, text, color, -1, clip);
        return;
    }

    const char *ellipsis = "...";
    int ellipsis_width = appui_text_width(ellipsis);
    const char *tail = text;
    while (*tail && width + ellipsis_width > available) {
        const char *next = tail;
        uint32_t codepoint = appui_utf8_next(&next);
        width -= appui_codepoint_width(codepoint);
        tail = next;
    }
    if (ellipsis_width >= available) {
        appui_text(pixels, w, h, x, y, ellipsis, color, -1, clip);
        return;
    }
    appui_text(pixels, w, h, x, y, ellipsis, color, -1, clip);
    appui_text(pixels, w, h, x + ellipsis_width, y, tail,
               color, -1, clip);
}

static void draw_sidebar(struct appui_rect content) {
    int side = sidebar_width();
    if (!side)
        return;
    appui_fill(pixels, w, h, (struct appui_rect){0, content.y, side, content.h},
               THEME_PANEL_RAISED);
    appui_text(pixels, w, h, 12, content.y + 10, "Places", THEME_TEXT_DIM, -1,
               (struct appui_rect){8, content.y + 4, side - 16, 30});
    static const char *labels[] = {"Root", "Files", "Apps", "Devices", "System"};
    static const char *paths[] = {"/", "/fs", "/fs/apps", "/dev", "/proc"};
    for (int i = 0; i < 5; i++) {
        struct appui_rect r = {8, content.y + 38 + i * SIDEBAR_ROW_STEP,
                                side - 16, SIDEBAR_ROW_H};
        int state = appui_pointer_state(r, pointer_x, pointer_y, pointer_buttons);
        if (strcmp(current_path, paths[i]) == 0)
            state |= APPUI_STATE_SELECTED;
        appui_button_ex(pixels, w, h, r, labels[i], APPUI_BTN_GHOST, state);
    }
}

static void draw_list(void) {
    struct appui_rect list = list_rect();
    int header_h = LIST_HEADER_H;
    int show_type = list.w >= 360;
    int show_size = list.w >= 520;
    int size_w = 112;
    int type_w = 104;
    int size_x = list.x + list.w - 8 - size_w;
    int type_x = show_size ? size_x - 8 - type_w
                           : list.x + list.w - 8 - type_w;
    int name_right = show_type ? type_x - 8 : list.x + list.w - 8;
    appui_fill(pixels, w, h, list, THEME_LIST_BG);
    appui_border(pixels, w, h, list, THEME_FIELD_BORDER, THEME_DIVIDER);
    appui_fill(pixels, w, h, (struct appui_rect){list.x + 1, list.y + 1,
               list.w - 2, header_h - 1}, THEME_LIST_HEADER);
    appui_text(pixels, w, h, list.x + 30, list.y + 5, "Name",
               THEME_LIST_TEXT, -1, list);
    if (show_type)
        appui_text(pixels, w, h, type_x, list.y + 5,
                   "Type", THEME_LIST_TEXT, -1, list);
    if (show_size)
        appui_text(pixels, w, h, size_x, list.y + 5,
                   "Size", THEME_LIST_TEXT, -1, list);

    int visible = visible_rows();
    for (int row = 0; row < visible; row++) {
        int index = scroll_row + row;
        if (index >= entry_count)
            break;
        int y = list.y + header_h + row * ROW_H;
        int active = index == selected;
        if (active)
            appui_fill(pixels, w, h, (struct appui_rect){list.x + 2, y,
                       list.w - 4, ROW_H}, THEME_SELECTION_BG);
        else if (row & 1)
            appui_fill(pixels, w, h, (struct appui_rect){list.x + 2, y,
                       list.w - 4, ROW_H}, THEME_LIST_ALT);
        draw_icon(list.x + 7, y + 7, entries[index].type, active);
        int fg = active ? THEME_SELECTION_TEXT : THEME_LIST_TEXT;
        struct appui_rect name_clip = {list.x + 30, y + 4,
                                      appui_max(1, name_right - (list.x + 30)),
                                      ROW_H - 4};
        appui_text(pixels, w, h, name_clip.x, y + 7, entries[index].name,
                   fg, -1, name_clip);
        if (show_type)
            appui_text(pixels, w, h, type_x, y + 7,
                       entries[index].type == DT_DIR ? "Folder" : "File",
                       fg, -1, (struct appui_rect){type_x, y, type_w, ROW_H});
        if (show_size && entries[index].type != DT_DIR) {
            char size[20];
            draw_size(size, entries[index].size);
            appui_text(pixels, w, h, size_x, y + 7, size,
                       fg, -1, (struct appui_rect){size_x, y,
                                                 size_w, ROW_H});
        }
    }
    if (!entry_count)
        appui_text(pixels, w, h, list.x + 30, list.y + 52,
                   "This folder is empty", THEME_TEXT_FAINT, -1, list);
}

static int fitted_button_width(const char *label) {
    return appui_text_width(label) + 16;
}

static struct appui_rect dialog_rect(void) {
    int dw = appui_min(420, w - 24);
    return (struct appui_rect){(w - dw) / 2, (h - DIALOG_H) / 2,
                              dw, DIALOG_H};
}

static struct appui_rect dialog_button_rect(int index) {
    struct appui_rect dialog = dialog_rect();
    const char *confirm_label = input_mode == MODE_DELETE ? "Delete" : "OK";
    int gap = 8;
    int pad = 14;
    int confirm_w = fitted_button_width(confirm_label);
    int cancel_w = fitted_button_width("Cancel");
    int available = appui_max(2, dialog.w - pad * 2);
    if (confirm_w + gap + cancel_w > available) {
        confirm_w = appui_max(1, (available - gap) / 2);
        cancel_w = appui_max(1, available - gap - confirm_w);
    }
    int x = dialog.x + dialog.w - pad - confirm_w - gap - cancel_w;
    if (index == 0)
        return (struct appui_rect){x, dialog.y + DIALOG_ACTION_Y,
                                  confirm_w, CONTROL_H};
    return (struct appui_rect){x + confirm_w + gap,
                              dialog.y + DIALOG_ACTION_Y,
                              cancel_w, CONTROL_H};
}

static void draw_dialog(void) {
    if (input_mode == MODE_NONE)
        return;
    struct appui_rect dialog = dialog_rect();
    int dw = dialog.w;
    appui_fill_blend(pixels, w, h, (struct appui_rect){0, 0, w, h}, 0, 105);
    appui_fill_round(pixels, w, h, dialog, THEME_FIELD_BORDER);
    appui_fill_round(pixels, w, h,
                     (struct appui_rect){dialog.x + 1, dialog.y + 1,
                                         dialog.w - 2, dialog.h - 2},
                     THEME_PANEL_RAISED);
    const char *prompt = input_mode == MODE_NEW_DIR ? "New folder name" :
                         input_mode == MODE_NEW_FILE ? "New file name" :
                         input_mode == MODE_RENAME ? "Rename item" :
                         "Delete selected item?";
    struct appui_rect prompt_area = {dialog.x + 14, dialog.y + 8,
                                    dw - 28, 32};
    int prompt_y = prompt_area.y + (prompt_area.h - KFONT_HEIGHT) / 2 +
                   PLT_FONT_Y_SHIFT;
    appui_text(pixels, w, h, prompt_area.x, prompt_y, prompt, THEME_TEXT, -1,
               prompt_area);
    if (input_mode != MODE_DELETE) {
        struct appui_rect field = {dialog.x + 14,
                                   dialog.y + DIALOG_FIELD_Y,
                                   dw - 28, CONTROL_H};
        appui_field_frame(pixels, w, h, field, 1);
        int field_y = field.y + (field.h - KFONT_HEIGHT) / 2 +
                      PLT_FONT_Y_SHIFT;
        appui_text(pixels, w, h, field.x + 6, field_y,
                   input_text, THEME_FIELD_TEXT, -1,
                   (struct appui_rect){field.x + 4, field.y + 3,
                                       field.w - 8, field.h - 6});
        int caret_x = field.x + 6 + appui_text_width(input_text);
        appui_fill(pixels, w, h, (struct appui_rect){caret_x, field.y + 3, 2,
                   field.h - 6}, THEME_FOCUS);
    } else if (selected >= 0) {
        struct appui_rect name_area = {dialog.x + 14,
                                       dialog.y + DIALOG_FIELD_Y,
                                       dw - 28, CONTROL_H};
        int name_y = name_area.y + (name_area.h - KFONT_HEIGHT) / 2 +
                     PLT_FONT_Y_SHIFT;
        appui_text(pixels, w, h, name_area.x, name_y,
                   entries[selected].name, THEME_CLOSE_RED, -1,
                   name_area);
    }
    struct appui_rect confirm = dialog_button_rect(0);
    struct appui_rect cancel = dialog_button_rect(1);
    const char *confirm_label = input_mode == MODE_DELETE ? "Delete" : "OK";
    const char *cancel_label = "Cancel";
    if (fitted_button_width(confirm_label) > confirm.w)
        confirm_label = input_mode == MODE_DELETE ? "Yes" : "OK";
    if (fitted_button_width(cancel_label) > cancel.w)
        cancel_label = "Back";
    appui_button_ex(pixels, w, h, confirm,
                    confirm_label,
                    input_mode == MODE_DELETE ? APPUI_BTN_DANGER : APPUI_BTN_PRIMARY,
                    appui_pointer_state(confirm, pointer_x, pointer_y,
                                        pointer_buttons));
    appui_button_ex(pixels, w, h, cancel, cancel_label, APPUI_BTN_DEFAULT,
                    appui_pointer_state(cancel, pointer_x, pointer_y,
                                        pointer_buttons));
}

static int button_row_width(const char *const labels[4]) {
    int width = 3 * 6;
    for (int i = 0; i < 4; i++)
        width += fitted_button_width(labels[i]);
    return width;
}

static int nav_layout_mode(void) {
    static const char *full[] = {"Back", "Up", "Refresh", "Open"};
    static const char *compact[] = {"Back", "Up", "R", "Open"};
    int fixed = 10 + 6 + 64 + 10;
    if (fixed + button_row_width(full) <= w)
        return 0;
    if (fixed + button_row_width(compact) <= w)
        return 1;
    return 2;
}

static const char *nav_button_label(int index) {
    static const char *full[] = {"Back", "Up", "Refresh", "Open"};
    static const char *compact[] = {"Back", "Up", "R", "Open"};
    static const char *tiny[] = {"<", "^", "R", "O"};
    int mode = nav_layout_mode();
    return mode == 0 ? full[index] : mode == 1 ? compact[index] : tiny[index];
}

static struct appui_rect nav_button_rect(int index) {
    int x = 10;
    for (int i = 0; i < index; i++)
        x += fitted_button_width(nav_button_label(i)) + 6;
    return (struct appui_rect){x, 8,
                               fitted_button_width(nav_button_label(index)),
                               CONTROL_H};
}

static int nav_button_enabled(int index) {
    if (index == 0)
        return history_pos > 0;
    if (index == 1)
        return strcmp(current_path, "/") != 0;
    if (index == 3)
        return selected >= 0;
    return 1;
}

static struct appui_rect address_rect(void) {
    struct appui_rect open = nav_button_rect(3);
    int x = open.x + open.w + 6;
    return (struct appui_rect){x, 8, appui_max(1, w - x - 10), CONTROL_H};
}

static int action_layout_mode(void) {
    static const char *full[] = {"New Folder", "New File", "Rename", "Delete"};
    static const char *compact[] = {"Folder", "File", "Rename", "Delete"};
    if (20 + button_row_width(full) <= w)
        return 0;
    if (20 + button_row_width(compact) <= w)
        return 1;
    return 2;
}

static const char *action_button_label(int index) {
    static const char *full[] = {"New Folder", "New File", "Rename", "Delete"};
    static const char *compact[] = {"Folder", "File", "Rename", "Delete"};
    static const char *narrow[] = {"Dir", "File", "Ren", "Del"};
    static const char *tiny[] = {"D", "F", "R", "X"};
    int mode = action_layout_mode();
    if (mode == 0)
        return full[index];
    if (mode == 1)
        return compact[index];
    return w >= 300 ? narrow[index] : tiny[index];
}

static struct appui_rect action_button_rect(int index) {
    int y = 8 + CONTROL_H + 8;
    if (action_layout_mode() == 2) {
        int left = 8;
        int gap = 4;
        int available = appui_max(4, w - left * 2 - gap * 3);
        int cell = appui_max(1, available / 4);
        int x = left + index * (cell + gap);
        int right = index == 3 ? w - left : x + cell;
        return (struct appui_rect){x, y, appui_max(1, right - x), CONTROL_H};
    }
    int x = 10;
    for (int i = 0; i < index; i++)
        x += fitted_button_width(action_button_label(i)) + 6;
    return (struct appui_rect){x, y,
                               fitted_button_width(action_button_label(index)),
                               CONTROL_H};
}

static int action_button_enabled(int index) {
    return index < 2 || selected >= 0;
}

static void render(void) {
    clamp_selection();
    appui_fill(pixels, w, h, (struct appui_rect){0, 0, w, h}, THEME_APP_BG);
    appui_fill(pixels, w, h, (struct appui_rect){0, 0, w, TOOLBAR_H},
               THEME_TOOLBAR_BG);
    for (int i = 0; i < 4; i++) {
        struct appui_rect r = nav_button_rect(i);
        int state = appui_pointer_state(r, pointer_x, pointer_y, pointer_buttons);
        if (!nav_button_enabled(i))
            state |= APPUI_STATE_DISABLED;
        appui_button_ex(pixels, w, h, r, nav_button_label(i),
                        i == 3 ? APPUI_BTN_PRIMARY : APPUI_BTN_DEFAULT,
                        state);
    }
    struct appui_rect address = address_rect();
    appui_field_frame(pixels, w, h, address, 0);
    int address_y = address.y + (address.h - KFONT_HEIGHT) / 2 +
                    PLT_FONT_Y_SHIFT;
    draw_text_tail(address.x + 7, address_y, current_path,
                   THEME_FIELD_TEXT,
                   (struct appui_rect){address.x + 6, address.y + 3,
                                       appui_max(1, address.w - 12),
                                       address.h - 6});
    for (int i = 0; i < 4; i++) {
        struct appui_rect r = action_button_rect(i);
        int state = appui_pointer_state(r, pointer_x, pointer_y, pointer_buttons);
        if (!action_button_enabled(i))
            state |= APPUI_STATE_DISABLED;
        appui_button_ex(pixels, w, h, r, action_button_label(i),
                        i == 3 ? APPUI_BTN_DANGER : APPUI_BTN_DEFAULT,
                        state);
    }
    struct appui_rect content = content_rect();
    draw_sidebar(content);
    draw_list();
    struct appui_rect footer_area = {0, h - FOOTER_H, w, FOOTER_H};
    appui_fill(pixels, w, h, footer_area, THEME_TOOLBAR_BG);
    char footer[96];
    footer[0] = 0;
    appui_append_int(footer, entry_count, sizeof(footer));
    appui_append_text(footer, " items", sizeof(footer));
    if (selected >= 0) {
        appui_append_text(footer, "  |  ", sizeof(footer));
        appui_append_text(footer, entries[selected].name, sizeof(footer));
    }
    if (status[0] && strcmp(status, "Ready") != 0) {
        appui_append_text(footer, "  |  ", sizeof(footer));
        appui_append_text(footer, status, sizeof(footer));
    }
    int footer_y = footer_area.y + (footer_area.h - KFONT_HEIGHT) / 2 +
                   PLT_FONT_Y_SHIFT;
    appui_text(pixels, w, h, 10, footer_y, footer, THEME_TEXT_DIM, -1,
               (struct appui_rect){8, footer_area.y + 2,
                                   w - 16, footer_area.h - 4});
    draw_dialog();
}

static int handle_dialog_click(int x, int y) {
    if (appui_inside(x, y, dialog_button_rect(0))) {
        finish_input();
        return 1;
    }
    if (appui_inside(x, y, dialog_button_rect(1))) {
        input_mode = MODE_NONE;
        set_status("Cancelled");
        return 1;
    }
    return 1;
}

static void click(struct guiapp_ctx *ctx, int x, int y) {
    if (input_mode != MODE_NONE) {
        (void)handle_dialog_click(x, y);
        return;
    }
    if (appui_inside(x, y, nav_button_rect(0)) && nav_button_enabled(0)) {
        go_back();
        return;
    }
    if (appui_inside(x, y, nav_button_rect(1)) && nav_button_enabled(1)) {
        go_parent();
        return;
    }
    if (appui_inside(x, y, nav_button_rect(2))) {
        if (scan_directory() == 0)
            set_status("Refreshed");
        return;
    }
    if (appui_inside(x, y, nav_button_rect(3)) && nav_button_enabled(3)) {
        open_selected(ctx);
        return;
    }
    if (appui_inside(x, y, action_button_rect(0))) {
        begin_input(MODE_NEW_DIR);
        return;
    }
    if (appui_inside(x, y, action_button_rect(1))) {
        begin_input(MODE_NEW_FILE);
        return;
    }
    if (appui_inside(x, y, action_button_rect(2)) && action_button_enabled(2)) {
        begin_input(MODE_RENAME);
        return;
    }
    if (appui_inside(x, y, action_button_rect(3)) && action_button_enabled(3)) {
        begin_input(MODE_DELETE);
        return;
    }
    int side = sidebar_width();
    struct appui_rect content = content_rect();
    if (side) {
        static const char *paths[] = {"/", "/fs", "/fs/apps", "/dev", "/proc"};
        for (int i = 0; i < 5; i++) {
            if (appui_inside(x, y,
                (struct appui_rect){8,
                                    content.y + 38 + i * SIDEBAR_ROW_STEP,
                                    side - 16, SIDEBAR_ROW_H})) {
                go_to(paths[i]);
                return;
            }
        }
    }
    struct appui_rect list = list_rect();
    int rel = y - (list.y + LIST_HEADER_H);
    if (x >= list.x && x < list.x + list.w &&
        y < list.y + list.h && rel >= 0) {
        int index = scroll_row + rel / ROW_H;
        if (index >= 0 && index < entry_count) {
            if (index == selected && index == previous_click) {
                previous_click = -1;
                open_selected(ctx);
            } else {
                selected = index;
                previous_click = index;
            }
        }
    }
}

static void handle_mouse(struct guiapp_ctx *ctx, int x, int y, int buttons, int wheel) {
    pointer_x = x;
    pointer_y = y;
    pointer_buttons = buttons;
    if (wheel && input_mode == MODE_NONE) {
        scroll_row -= wheel * 3;
        previous_click = -1;
        clamp_selection();
    }
    if ((buttons & 1) && !(prev_buttons & 1))
        click(ctx, x, y);
    prev_buttons = buttons;
}

static void handle_key(struct guiapp_ctx *ctx, int key) {
    if (input_mode != MODE_NONE) {
        if (key == '\r' || key == '\n') {
            finish_input();
        } else if (key == GUIAPP_KEY_BACKSPACE || key == 127) {
            if (input_mode != MODE_DELETE && input_len > 0)
                input_text[input_len = appui_utf8_prev(input_text, input_len)] = 0;
        } else if (input_mode != MODE_DELETE && key >= 32 && key < 127 &&
                   input_len + 1 < (int)sizeof(input_text)) {
            input_text[input_len++] = (char)key;
            input_text[input_len] = 0;
        }
        return;
    }
    if (key == GUIAPP_KEY_UP && selected > 0)
        selected--;
    else if (key == GUIAPP_KEY_DOWN && selected + 1 < entry_count)
        selected++;
    else if (key == GUIAPP_KEY_LEFT || key == GUIAPP_KEY_BACKSPACE)
        go_parent();
    else if (key == GUIAPP_KEY_RIGHT || key == '\r' || key == '\n')
        open_selected(ctx);
    else if (key == 'r' || key == 'R')
        (void)scan_directory();
    else if (key == 'n' || key == 'N')
        begin_input(MODE_NEW_FILE);
    clamp_selection();
}

static void handle_text(const char *value) {
    if (input_mode == MODE_NONE || input_mode == MODE_DELETE)
        return;
    int n = (int)strlen(value);
    if (n <= 0 || input_len + n >= (int)sizeof(input_text))
        return;
    for (int i = 0; i < n; i++)
        input_text[input_len++] = value[i];
    input_text[input_len] = 0;
}

static void handle_command(struct guiapp_ctx *ctx, int command) {
    if (command != GUIAPP_CMD_COPY && command != GUIAPP_CMD_CUT)
        return;
    if (input_mode != MODE_NONE && input_mode != MODE_DELETE) {
        (void)guiapp_set_clipboard(ctx, input_text);
        if (command == GUIAPP_CMD_CUT) {
            input_len = 0;
            input_text[0] = 0;
        }
    } else if (selected >= 0 && selected < entry_count) {
        (void)guiapp_set_clipboard(ctx, entries[selected].name);
    }
}

int main(int argc, char **argv) {
    struct guiapp_ctx ctx;
    struct guiapp_event event;
    if (guiapp_parse_args(argc, argv, &ctx) < 0)
        return 1;
    (void)scan_directory();
    for (;;) {
        if (guiapp_read_event(&ctx, &event) < 0 || event.type == GUIAPP_EVT_CLOSE)
            break;
        if (event.type == GUIAPP_EVT_INIT || event.type == GUIAPP_EVT_RESIZE) {
            w = clamp_int(event.width, 220, MAX_W);
            h = clamp_int(event.height, 160, MAX_H);
            clamp_selection();
        } else if (event.type == GUIAPP_EVT_KEY && event.buttons) {
            handle_key(&ctx, event.key);
        } else if (event.type == GUIAPP_EVT_TEXT) {
            handle_text(event.text);
        } else if (event.type == GUIAPP_EVT_COMMAND) {
            handle_command(&ctx, event.key);
        } else if (event.type == GUIAPP_EVT_MOUSE) {
            handle_mouse(&ctx, event.x, event.y, event.buttons, event.wheel);
        }
        render();
        if (guiapp_send_frame(&ctx, "Files", w, h, pixels) < 0)
            break;
    }
    return 0;
}
