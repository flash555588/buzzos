#include "appui.h"
#include "guiapp.h"
#include "libc.h"

enum {
    MAX_W = GUIAPP_MAX_W,
    MAX_H = GUIAPP_MAX_H,
    MAX_ENTRIES = 128,
    PATH_CAP = GUIAPP_PATH_MAX,
    ROW_H = 26,
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
    int rows = (h - 78 - 24 - 24) / ROW_H;
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
    appui_copy_text(status, "Loaded ", sizeof(status));
    appui_append_int(status, entry_count, sizeof(status));
    appui_append_text(status, " items", sizeof(status));
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
    return (struct appui_rect){0, 78, w, appui_max(1, h - 102)};
}

static int sidebar_width(void) {
    return w >= 480 ? 104 : 0;
}

static struct appui_rect list_rect(void) {
    struct appui_rect c = content_rect();
    int side = sidebar_width();
    return (struct appui_rect){side + 8, c.y, w - side - 16, c.h};
}

static void draw_icon(int x, int y, int type, int selected_row) {
    int edge = selected_row ? appui_rgb6(2, 5, 5) : appui_gray(8);
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

static void draw_sidebar(struct appui_rect content) {
    int side = sidebar_width();
    if (!side)
        return;
    appui_fill(pixels, w, h, (struct appui_rect){0, content.y, side, content.h},
               appui_gray(2));
    appui_text(pixels, w, h, 12, content.y + 10, "Places", appui_gray(12), -1,
               (struct appui_rect){8, content.y + 6, side - 16, 22});
    static const char *labels[] = {"Root", "Files", "Apps", "Devices", "System"};
    static const char *paths[] = {"/", "/fs", "/fs/apps", "/dev", "/proc"};
    for (int i = 0; i < 5; i++) {
        struct appui_rect r = {8, content.y + 34 + i * 30, side - 16, 25};
        appui_button(pixels, w, h, r, labels[i], strcmp(current_path, paths[i]) == 0);
    }
}

static void draw_list(void) {
    struct appui_rect list = list_rect();
    int header_h = 24;
    appui_fill(pixels, w, h, list, 15);
    appui_border(pixels, w, h, list, appui_gray(8), appui_gray(1));
    appui_fill(pixels, w, h, (struct appui_rect){list.x + 1, list.y + 1,
               list.w - 2, header_h - 1}, appui_gray(4));
    appui_text(pixels, w, h, list.x + 30, list.y + 5, "Name", 0, -1, list);
    if (list.w > 340)
        appui_text(pixels, w, h, list.x + list.w - 160, list.y + 5,
                   "Type", 0, -1, list);
    if (list.w > 440)
        appui_text(pixels, w, h, list.x + list.w - 78, list.y + 5,
                   "Size", 0, -1, list);

    int visible = visible_rows();
    for (int row = 0; row < visible; row++) {
        int index = scroll_row + row;
        if (index >= entry_count)
            break;
        int y = list.y + header_h + row * ROW_H;
        int active = index == selected;
        if (active)
            appui_fill(pixels, w, h, (struct appui_rect){list.x + 2, y,
                       list.w - 4, ROW_H}, appui_rgb6(0, 3, 5));
        else if (row & 1)
            appui_fill(pixels, w, h, (struct appui_rect){list.x + 2, y,
                       list.w - 4, ROW_H}, appui_gray(14));
        draw_icon(list.x + 7, y + 3, entries[index].type, active);
        int fg = active ? 15 : 0;
        struct appui_rect name_clip = {list.x + 30, y + 3,
            list.w > 340 ? list.w - 200 : list.w - 36, ROW_H - 4};
        appui_text(pixels, w, h, name_clip.x, y + 5, entries[index].name,
                   fg, -1, name_clip);
        if (list.w > 340)
            appui_text(pixels, w, h, list.x + list.w - 160, y + 5,
                       entries[index].type == DT_DIR ? "Folder" : "File",
                       fg, -1, (struct appui_rect){list.x + list.w - 160, y,
                                                 76, ROW_H});
        if (list.w > 440 && entries[index].type != DT_DIR) {
            char size[20];
            draw_size(size, entries[index].size);
            appui_text(pixels, w, h, list.x + list.w - 78, y + 5, size,
                       fg, -1, (struct appui_rect){list.x + list.w - 78, y,
                                                 72, ROW_H});
        }
    }
    if (!entry_count)
        appui_text(pixels, w, h, list.x + 30, list.y + 52,
                   "This folder is empty", appui_gray(7), -1, list);
}

static void draw_dialog(void) {
    if (input_mode == MODE_NONE)
        return;
    int dw = appui_min(420, w - 24);
    int dh = 118;
    struct appui_rect dialog = {(w - dw) / 2, (h - dh) / 2, dw, dh};
    appui_fill(pixels, w, h, dialog, appui_gray(4));
    appui_border(pixels, w, h, dialog, 15, appui_gray(0));
    const char *prompt = input_mode == MODE_NEW_DIR ? "New folder name" :
                         input_mode == MODE_NEW_FILE ? "New file name" :
                         input_mode == MODE_RENAME ? "Rename item" :
                         "Delete selected item?";
    appui_text(pixels, w, h, dialog.x + 14, dialog.y + 13, prompt, 15, -1,
               (struct appui_rect){dialog.x + 10, dialog.y + 8, dw - 20, 24});
    if (input_mode != MODE_DELETE) {
        struct appui_rect field = {dialog.x + 14, dialog.y + 40, dw - 28, 28};
        appui_fill(pixels, w, h, field, 15);
        appui_border(pixels, w, h, field, appui_rgb6(0, 4, 5), appui_gray(1));
        appui_text(pixels, w, h, field.x + 6, field.y + 6, input_text, 0, -1,
                   (struct appui_rect){field.x + 4, field.y + 3, field.w - 8, 22});
        int caret_x = field.x + 6 + appui_text_width(input_text);
        appui_fill(pixels, w, h, (struct appui_rect){caret_x, field.y + 5, 2,
                   KFONT_HEIGHT + 2}, appui_rgb6(0, 3, 5));
    } else if (selected >= 0) {
        appui_text(pixels, w, h, dialog.x + 14, dialog.y + 43,
                   entries[selected].name, appui_rgb6(5, 3, 0), -1,
                   (struct appui_rect){dialog.x + 10, dialog.y + 38, dw - 20, 24});
    }
    appui_button(pixels, w, h,
                 (struct appui_rect){dialog.x + dw - 166, dialog.y + 80, 70, 26},
                 input_mode == MODE_DELETE ? "Delete" : "OK", 1);
    appui_button(pixels, w, h,
                 (struct appui_rect){dialog.x + dw - 88, dialog.y + 80, 70, 26},
                 "Cancel", 0);
}

static int fitted_button_width(const char *label) {
    return (int)strlen(label) * KFONT_WIDTH + 20;
}

static struct appui_rect nav_button_rect(int index) {
    static const char *labels[] = {"Back", "Up", "Refresh", "Open"};
    int x = 10;
    for (int i = 0; i < index; i++)
        x += fitted_button_width(labels[i]) + 6;
    return (struct appui_rect){x, 8, fitted_button_width(labels[index]), 26};
}

static struct appui_rect address_rect(void) {
    struct appui_rect open = nav_button_rect(3);
    int x = open.x + open.w + 6;
    return (struct appui_rect){x, 8, appui_max(1, w - x - 10), 26};
}

static struct appui_rect action_button_rect(int index) {
    static const char *labels[] = {"New Folder", "New File", "Rename", "Delete"};
    int x = 10;
    for (int i = 0; i < index; i++)
        x += fitted_button_width(labels[i]) + 6;
    return (struct appui_rect){x, 42, fitted_button_width(labels[index]), 26};
}

static void render(void) {
    clamp_selection();
    appui_fill(pixels, w, h, (struct appui_rect){0, 0, w, h}, appui_gray(3));
    appui_fill(pixels, w, h, (struct appui_rect){0, 0, w, 78}, appui_gray(2));
    appui_button(pixels, w, h, nav_button_rect(0), "Back", 0);
    appui_button(pixels, w, h, nav_button_rect(1), "Up", 0);
    appui_button(pixels, w, h, nav_button_rect(2), "Refresh", 0);
    appui_button(pixels, w, h, nav_button_rect(3), "Open", 1);
    struct appui_rect address = address_rect();
    appui_fill(pixels, w, h, address, 15);
    appui_border(pixels, w, h, address, appui_gray(8), appui_gray(1));
    appui_text(pixels, w, h, address.x + 7, address.y + 6, current_path, 0, -1,
               (struct appui_rect){address.x + 6, address.y + 2,
                                   appui_max(1, address.w - 12), 22});
    appui_button(pixels, w, h, action_button_rect(0), "New Folder", 0);
    appui_button(pixels, w, h, action_button_rect(1), "New File", 0);
    appui_button(pixels, w, h, action_button_rect(2), "Rename", 0);
    appui_button(pixels, w, h, action_button_rect(3), "Delete", 0);
    struct appui_rect last_action = action_button_rect(3);
    int status_x = last_action.x + last_action.w + 8;
    appui_text(pixels, w, h, status_x, 48, status, appui_gray(12), -1,
               (struct appui_rect){status_x, 43, appui_max(1, w - status_x - 8), 23});
    struct appui_rect content = content_rect();
    draw_sidebar(content);
    draw_list();
    appui_fill(pixels, w, h, (struct appui_rect){0, h - 24, w, 24}, appui_gray(2));
    char footer[96];
    footer[0] = 0;
    appui_append_int(footer, entry_count, sizeof(footer));
    appui_append_text(footer, " items", sizeof(footer));
    if (selected >= 0) {
        appui_append_text(footer, "  |  ", sizeof(footer));
        appui_append_text(footer, entries[selected].name, sizeof(footer));
    }
    appui_text(pixels, w, h, 10, h - 18, footer, appui_gray(12), -1,
               (struct appui_rect){8, h - 21, w - 16, 20});
    draw_dialog();
}

static int handle_dialog_click(int x, int y) {
    int dw = appui_min(420, w - 24);
    struct appui_rect dialog = {(w - dw) / 2, (h - 118) / 2, dw, 118};
    if (appui_inside(x, y, (struct appui_rect){dialog.x + dw - 166,
                                              dialog.y + 80, 70, 26})) {
        finish_input();
        return 1;
    }
    if (appui_inside(x, y, (struct appui_rect){dialog.x + dw - 88,
                                              dialog.y + 80, 70, 26})) {
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
    if (appui_inside(x, y, nav_button_rect(0))) {
        go_back();
        return;
    }
    if (appui_inside(x, y, nav_button_rect(1))) {
        go_parent();
        return;
    }
    if (appui_inside(x, y, nav_button_rect(2))) {
        (void)scan_directory();
        return;
    }
    if (appui_inside(x, y, nav_button_rect(3))) {
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
    if (appui_inside(x, y, action_button_rect(2))) {
        begin_input(MODE_RENAME);
        return;
    }
    if (appui_inside(x, y, action_button_rect(3))) {
        begin_input(MODE_DELETE);
        return;
    }
    int side = sidebar_width();
    struct appui_rect content = content_rect();
    if (side) {
        static const char *paths[] = {"/", "/fs", "/fs/apps", "/dev", "/proc"};
        for (int i = 0; i < 5; i++) {
            if (appui_inside(x, y, (struct appui_rect){8, content.y + 34 + i * 30,
                                                       side - 16, 25})) {
                go_to(paths[i]);
                return;
            }
        }
    }
    struct appui_rect list = list_rect();
    int rel = y - (list.y + 24);
    if (x >= list.x && x < list.x + list.w && rel >= 0) {
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
        } else if (event.type == GUIAPP_EVT_KEY) {
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
