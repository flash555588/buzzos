#include "appui.h"
#include "guiapp.h"
#include "libc.h"

enum {
    MAX_W = GUIAPP_MAX_W,
    MAX_H = GUIAPP_MAX_H,
    MAX_ENTRIES = 128,
    PATH_CAP = GUIAPP_PATH_MAX,
    ROW_H = 34,
    /* appui_list_row insets its icon by 14 and its label by 42; rows are
     * themselves inset by 2, so column text starts here. */
    ROW_TEXT_X = 44,
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
    CRUMB_MAX = 16,
    CRUMB_LABEL_CAP = 24,
    CRUMB_SEP_W = 14,
    SIDEBAR_COUNT = 5,
};

struct file_entry {
    char name[24];
    uint32_t type;
    uint32_t size;
};

static uint32_t *pixels;
static size_t pixels_cap;
static struct file_entry entries[MAX_ENTRIES];
static int entry_count;
static int selected = -1;
static int scroll_row;
static int scroll_dragging;
static int scroll_drag_mouse;
static int scroll_drag_start_px;
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
/* 0=normal, 1=open-for app, 2=new-for app (desktop file picker). */
static int pick_mode;
static char pick_app[PATH_CAP];
static char window_title[GUIAPP_TITLE_MAX] = "Files";

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

static int starts_with(const char *text, const char *prefix) {
    while (*prefix) {
        if (*text++ != *prefix++)
            return 0;
    }
    return 1;
}

static void launch_picked(struct guiapp_ctx *ctx, const char *path) {
    if (!pick_app[0] || !path || !path[0]) {
        set_status("No target app for picker");
        return;
    }
    if (guiapp_request_launch(ctx, pick_app, path) < 0) {
        set_status("Could not open in target app");
        return;
    }
    /* Temporary picker windows close themselves after handing off. */
    if (pick_mode == 1 || pick_mode == 2)
        exit(0);
    appui_copy_text(status, "Opened: ", sizeof(status));
    appui_append_text(status, path, sizeof(status));
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
    /* Picker mode: hand the chosen file back to the requesting app. */
    if (pick_mode == 1 || pick_mode == 2) {
        launch_picked(ctx, path);
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
    if (has_extension(path, ".lua")) {
        if (guiapp_request_launch(ctx, "/fs/apps/luaide", path) < 0)
            set_status("Could not launch LuaIDE");
        else
            set_status("Opened in LuaIDE");
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

static void finish_input(struct guiapp_ctx *ctx) {
    char path[PATH_CAP];
    char old_path[PATH_CAP];
    int rc = -1;
    const char *result = "Operation failed";
    path[0] = 0;
    if (input_mode != MODE_DELETE && !is_valid_name(input_text)) {
        set_status("Enter a valid name");
        return;
    }
    if (input_mode == MODE_NEW_DIR) {
        if (join_path(path, current_path, input_text) == 0) {
            struct stat st;
            if (stat(path, &st) == 0) {
                set_status("Name already exists");
                return;
            }
            rc = mkdir(path);
        }
        result = rc == 0 ? "Folder created" : "Could not create folder";
    } else if (input_mode == MODE_NEW_FILE) {
        if (join_path(path, current_path, input_text) == 0) {
            struct stat st;
            if (stat(path, &st) == 0) {
                set_status("Name already exists");
                return;
            }
            rc = create(path);
        }
        result = rc == 0 ? "File created" : "Could not create file";
        /* New-file picker: open the created document in the target app. */
        if (rc == 0 && pick_mode == 2 && path[0]) {
            input_mode = MODE_NONE;
            (void)scan_directory();
            launch_picked(ctx, path);
            return;
        }
    } else if (input_mode == MODE_RENAME) {
        selected_path(old_path);
        if (old_path[0] && join_path(path, current_path, input_text) == 0) {
            struct stat st;
            /* Allow rename to the same path; otherwise refuse collisions. */
            if (strcmp(old_path, path) != 0 && stat(path, &st) == 0) {
                set_status("Name already exists");
                return;
            }
            rc = rename(old_path, path);
        }
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

/* Icon per entry.  Extension only: matching the launcher's own dispatch would
 * mean an ELF sniff per visible row per frame. */
static int entry_icon(const struct file_entry *entry) {
    if (entry->type == DT_DIR)
        return UI_ICON_FOLDER;
    if (has_extension(entry->name, ".gb") || has_extension(entry->name, ".gbc"))
        return UI_ICON_GAMEPAD;
    if (has_extension(entry->name, ".mp3") || has_extension(entry->name, ".wav"))
        return UI_ICON_MUSIC;
    if (has_extension(entry->name, ".lua") || has_extension(entry->name, ".c") ||
        has_extension(entry->name, ".h") || has_extension(entry->name, ".asm"))
        return UI_ICON_CODE;
    if (has_extension(entry->name, ".png") || has_extension(entry->name, ".bmp"))
        return UI_ICON_IMAGE;
    return UI_ICON_DOCUMENT;
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

/* Split a path into its components, recording offsets into `path` rather than
 * copying, so PATH_CAP-long paths cost nothing here. */
static int split_path(const char *path, int *starts, int *lens, int cap) {
    int count = 0;
    int i = 0;
    while (path[i] && count < cap) {
        while (path[i] == '/')
            i++;
        if (!path[i])
            break;
        starts[count] = i;
        while (path[i] && path[i] != '/')
            i++;
        lens[count] = i - starts[count];
        count++;
    }
    return count;
}

static void crumb_label(char *out, int cap, int start, int len) {
    int n = 0;
    while (n < cap - 1 && n < len)
        out[n] = current_path[start + n], n++;
    out[n] = 0;
}

static int crumbs_width(int first, int count, const int *starts,
                        const int *lens) {
    char label[CRUMB_LABEL_CAP];
    int total = first > 0 ? appui_label_width("...", UI_FONT_BODY) + CRUMB_SEP_W
                          : 0;
    for (int i = first; i < count; i++) {
        crumb_label(label, sizeof(label), starts[i], lens[i]);
        total += appui_label_width(label, UI_FONT_BODY);
        if (i > first)
            total += CRUMB_SEP_W;
    }
    return total;
}

/* Breadcrumb path bar.  When the path is too long the *leading* segments are
 * dropped, matching the old tail-ellipsis behaviour: the deepest directory is
 * the part worth showing. */
static void draw_breadcrumbs(struct appui_rect field) {
    int starts[CRUMB_MAX], lens[CRUMB_MAX];
    char label[CRUMB_LABEL_CAP];
    int count = split_path(current_path, starts, lens, CRUMB_MAX);
    int icon_side = 16;
    int x = field.x + 8;
    int right = field.x + field.w - 8;
    int first = 0;

    appui_icon(pixels, w, h, UI_ICON_FOLDER,
               (struct appui_rect){x, field.y, icon_side, field.h}, icon_side,
               UI_TEXT_TERTIARY);
    x += icon_side + 6;
    if (x >= right)
        return;

    if (count <= 0) {
        appui_label(pixels, w, h,
                    (struct appui_rect){x, field.y, right - x, field.h}, "/",
                    UI_FONT_BODY, UI_TEXT_PRIMARY, UI_ALIGN_LEFT);
        return;
    }
    while (first < count - 1 &&
           crumbs_width(first, count, starts, lens) > right - x)
        first++;
    if (first > 0) {
        int ellipsis_w = appui_label_width("...", UI_FONT_BODY);
        appui_label(pixels, w, h,
                    (struct appui_rect){x, field.y, ellipsis_w, field.h}, "...",
                    UI_FONT_BODY, UI_TEXT_TERTIARY, UI_ALIGN_LEFT);
        x += ellipsis_w + CRUMB_SEP_W;
    }
    for (int i = first; i < count && x < right; i++) {
        int segment_w;
        if (i > first) {
            appui_icon(pixels, w, h, UI_ICON_CHEVRON_RIGHT,
                       (struct appui_rect){x, field.y, CRUMB_SEP_W, field.h},
                       10, UI_TEXT_TERTIARY);
            x += CRUMB_SEP_W;
        }
        crumb_label(label, sizeof(label), starts[i], lens[i]);
        segment_w = appui_label_width(label, UI_FONT_BODY);
        if (segment_w > right - x)
            segment_w = right - x;
        appui_label(pixels, w, h,
                    (struct appui_rect){x, field.y, segment_w, field.h}, label,
                    UI_FONT_BODY,
                    i == count - 1 ? UI_TEXT_PRIMARY : UI_TEXT_SECONDARY,
                    UI_ALIGN_LEFT);
        x += segment_w;
    }
}

static const char *const sidebar_labels[SIDEBAR_COUNT] = {
    "Root", "Files", "Apps", "Devices", "System"
};
static const char *const sidebar_paths[SIDEBAR_COUNT] = {
    "/", "/fs", "/fs/apps", "/dev", "/proc"
};
static const int sidebar_icons[SIDEBAR_COUNT] = {
    UI_ICON_FOLDER, UI_ICON_DOCUMENT, UI_ICON_GRID, UI_ICON_NETWORK,
    UI_ICON_SETTINGS
};

static struct appui_rect sidebar_row_rect(struct appui_rect content, int i) {
    return (struct appui_rect){8, content.y + 38 + i * SIDEBAR_ROW_STEP,
                               sidebar_width() - 16, SIDEBAR_ROW_H};
}

static void draw_sidebar(struct appui_rect content) {
    int side = sidebar_width();
    if (!side)
        return;
    appui_fill(pixels, w, h, (struct appui_rect){0, content.y, side, content.h},
               UI_BG_LAYER);
    appui_separator(pixels, w, h, side - 1, content.y, content.h, 1);
    appui_label(pixels, w, h,
                (struct appui_rect){12, content.y + 6, side - 20, 26}, "Places",
                UI_FONT_CAPTION, UI_TEXT_TERTIARY, UI_ALIGN_LEFT);
    for (int i = 0; i < SIDEBAR_COUNT; i++) {
        struct appui_rect r = sidebar_row_rect(content, i);
        int state = appui_pointer_state(r, pointer_x, pointer_y, pointer_buttons);
        if (strcmp(current_path, sidebar_paths[i]) == 0)
            state |= APPUI_STATE_SELECTED;
        appui_list_row(pixels, w, h, r, sidebar_labels[i], sidebar_icons[i],
                       state);
    }
}

/* Row area of the list, below the column header. */
static struct appui_rect list_body_rect(void) {
    struct appui_rect list = list_rect();
    return (struct appui_rect){list.x + 2, list.y + LIST_HEADER_H,
                               appui_max(1, list.w - 4),
                               appui_max(1, visible_rows() * ROW_H)};
}

static int list_content_px(void) {
    return entry_count * ROW_H;
}

static int list_viewport_px(void) {
    return visible_rows() * ROW_H;
}

static int list_scrollable(void) {
    return list_content_px() > list_viewport_px();
}

static struct appui_rect list_scroll_track(void) {
    struct appui_rect body = list_body_rect();
    return (struct appui_rect){body.x + body.w - APPUI_SCROLL_W, body.y,
                               APPUI_SCROLL_W, body.h};
}

static struct appui_rect list_scroll_thumb(void) {
    return appui_scroll_thumb(list_scroll_track(), 1, list_content_px(),
                              list_viewport_px(), scroll_row * ROW_H);
}

static void draw_list(void) {
    struct appui_rect list = list_rect();
    struct appui_rect body = list_body_rect();
    int header_h = LIST_HEADER_H;
    int scroll_w = list_scrollable() ? APPUI_SCROLL_W : 0;
    int show_type = list.w >= 360;
    int show_size = list.w >= 520;
    int size_w = 112;
    int type_w = 104;
    int right = list.x + list.w - 8 - scroll_w;
    int size_x = right - size_w;
    int type_x = show_size ? size_x - 8 - type_w : right - type_w;
    int name_right = show_type ? type_x - 8 : right;

    appui_fill(pixels, w, h, list, UI_BG_SOLID);
    appui_stroke_round(pixels, w, h, list, UI_RADIUS_CONTROL,
                       UI_STROKE_CONTROL);
    appui_label(pixels, w, h,
                (struct appui_rect){list.x + ROW_TEXT_X, list.y,
                                    appui_max(1, name_right -
                                                 (list.x + ROW_TEXT_X)),
                                    header_h},
                "Name", UI_FONT_CAPTION, UI_TEXT_TERTIARY, UI_ALIGN_LEFT);
    if (show_type)
        appui_label(pixels, w, h,
                    (struct appui_rect){type_x, list.y, type_w, header_h},
                    "Type", UI_FONT_CAPTION, UI_TEXT_TERTIARY, UI_ALIGN_LEFT);
    if (show_size)
        appui_label(pixels, w, h,
                    (struct appui_rect){size_x, list.y, size_w, header_h},
                    "Size", UI_FONT_CAPTION, UI_TEXT_TERTIARY, UI_ALIGN_RIGHT);
    appui_separator(pixels, w, h, list.x + 1, list.y + header_h - 1,
                    list.w - 2, 0);

    int visible = visible_rows();
    for (int row = 0; row < visible; row++) {
        int index = scroll_row + row;
        if (index >= entry_count)
            break;
        int y = list.y + header_h + row * ROW_H;
        struct appui_rect row_rect = {body.x, y, body.w - scroll_w, ROW_H};
        struct appui_rect name_rect = {list.x + ROW_TEXT_X, y,
                                       appui_max(1, name_right -
                                                    (list.x + ROW_TEXT_X)),
                                       ROW_H};
        int state = appui_pointer_state(row_rect, pointer_x, pointer_y,
                                        pointer_buttons);
        if (index == selected)
            state |= APPUI_STATE_SELECTED;
        /* The row paints its fill, accent bar and icon across the whole width;
         * the name is drawn separately so it ellipsizes at the Type column
         * instead of running underneath it. */
        appui_list_row(pixels, w, h, row_rect, "",
                       entry_icon(&entries[index]), state);
        uint32_t fg = (state & APPUI_STATE_SELECTED) ? UI_TEXT_PRIMARY
                                                     : UI_TEXT_SECONDARY;
        appui_label(pixels, w, h, name_rect, entries[index].name, UI_FONT_BODY,
                    fg, UI_ALIGN_LEFT);
        if (show_type)
            appui_label(pixels, w, h,
                        (struct appui_rect){type_x, y, type_w, ROW_H},
                        entries[index].type == DT_DIR ? "Folder" : "File",
                        UI_FONT_BODY, UI_TEXT_TERTIARY, UI_ALIGN_LEFT);
        if (show_size && entries[index].type != DT_DIR) {
            char size[20];
            draw_size(size, entries[index].size);
            appui_label(pixels, w, h,
                        (struct appui_rect){size_x, y, size_w, ROW_H}, size,
                        UI_FONT_BODY, fg, UI_ALIGN_RIGHT);
        }
    }
    if (!entry_count)
        appui_label(pixels, w, h,
                    (struct appui_rect){list.x + ROW_TEXT_X, list.y + header_h,
                                        appui_max(1, list.w - ROW_TEXT_X - 8),
                                        ROW_H},
                    "This folder is empty", UI_FONT_BODY, UI_TEXT_TERTIARY,
                    UI_ALIGN_LEFT);
    appui_scrollbar(pixels, w, h, list_scroll_track(), 1, list_content_px(),
                    list_viewport_px(), scroll_row * ROW_H,
                    scroll_dragging ||
                    appui_inside(pointer_x, pointer_y, list_scroll_track()));
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
    appui_scrim(pixels, w, h);
    appui_card(pixels, w, h, dialog);
    const char *prompt = input_mode == MODE_NEW_DIR ? "New folder name" :
                         input_mode == MODE_NEW_FILE ? "New file name" :
                         input_mode == MODE_RENAME ? "Rename item" :
                         "Delete selected item?";
    struct appui_rect prompt_area = {dialog.x + 14, dialog.y + 8,
                                    dw - 28, 32};
    appui_label(pixels, w, h, prompt_area, prompt, UI_FONT_BODY_LG,
                UI_TEXT_PRIMARY, UI_ALIGN_LEFT);
    if (input_mode != MODE_DELETE) {
        struct appui_rect field = {dialog.x + 14,
                                   dialog.y + DIALOG_FIELD_Y,
                                   dw - 28, CONTROL_H};
        appui_field_frame(pixels, w, h, field, 1);
        /* The field is a text-entry caret grid, so it keeps native-size
         * appui_text: the caret x below is measured with appui_text_width. */
        int field_y = field.y + (field.h - KFONT_HEIGHT) / 2 +
                      PLT_FONT_Y_SHIFT;
        appui_text(pixels, w, h, field.x + 6, field_y,
                   input_text, UI_TEXT_PRIMARY, -1,
                   (struct appui_rect){field.x + 4, field.y + 3,
                                       field.w - 8, field.h - 6});
        int caret_x = field.x + 6 + appui_text_width(input_text);
        appui_fill(pixels, w, h, (struct appui_rect){caret_x, field.y + 3, 2,
                   field.h - 6}, UI_ACCENT_FILL);
    } else if (selected >= 0) {
        struct appui_rect name_area = {dialog.x + 14,
                                       dialog.y + DIALOG_FIELD_Y,
                                       dw - 28, CONTROL_H};
        appui_label(pixels, w, h, name_area, entries[selected].name,
                    UI_FONT_BODY, UI_SYS_CRITICAL, UI_ALIGN_LEFT);
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
    appui_fill(pixels, w, h, (struct appui_rect){0, 0, w, h}, UI_BG_SOLID);
    appui_toolbar(pixels, w, h, (struct appui_rect){0, 0, w, TOOLBAR_H});
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
    draw_breadcrumbs(address);
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
    appui_fill(pixels, w, h, footer_area, UI_BG_LAYER);
    appui_separator(pixels, w, h, 0, footer_area.y, w, 0);
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
    appui_label(pixels, w, h,
                (struct appui_rect){10, footer_area.y, appui_max(1, w - 20),
                                    footer_area.h},
                footer, UI_FONT_BODY, UI_TEXT_SECONDARY, UI_ALIGN_LEFT);
    draw_dialog();
}

static int handle_dialog_click(struct guiapp_ctx *ctx, int x, int y) {
    if (appui_inside(x, y, dialog_button_rect(0))) {
        finish_input(ctx);
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
        (void)handle_dialog_click(ctx, x, y);
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
        for (int i = 0; i < SIDEBAR_COUNT; i++) {
            if (appui_inside(x, y, sidebar_row_rect(content, i))) {
                go_to(sidebar_paths[i]);
                return;
            }
        }
    }
    struct appui_rect body = list_body_rect();
    if (list_scrollable() && appui_inside(x, y, list_scroll_track()))
        return;
    int rel = y - body.y;
    if (x >= body.x && x < body.x + body.w &&
        y < body.y + body.h && rel >= 0) {
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

/* Scroll offset in pixels, clamped, then converted back to whole rows. */
static void set_scroll_px(int offset_px) {
    int max_px = appui_max(0, list_content_px() - list_viewport_px());
    scroll_row = clamp_int(offset_px, 0, max_px) / ROW_H;
    clamp_selection();
}

/* Thumb hit-testing and dragging both go through appui_scroll_thumb, so the
 * grabbable thumb is exactly the painted one. */
static void handle_scroll_press(int x, int y) {
    struct appui_rect track = list_scroll_track();
    struct appui_rect thumb = list_scroll_thumb();
    if (!list_scrollable() || !appui_inside(x, y, track))
        return;
    if (!appui_inside(x, y, thumb)) {
        set_scroll_px(appui_scroll_offset_at(track, 1, list_content_px(),
                                             list_viewport_px(), y));
        thumb = list_scroll_thumb();
    }
    scroll_dragging = 1;
    scroll_drag_mouse = y;
    scroll_drag_start_px = scroll_row * ROW_H;
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
    if ((buttons & 1) && !(prev_buttons & 1)) {
        if (input_mode == MODE_NONE)
            handle_scroll_press(x, y);
        if (!scroll_dragging)
            click(ctx, x, y);
    }
    if ((buttons & 1) && scroll_dragging) {
        struct appui_rect track = list_scroll_track();
        struct appui_rect thumb = list_scroll_thumb();
        int span = appui_max(1, track.h - thumb.h);
        int max_px = appui_max(0, list_content_px() - list_viewport_px());
        set_scroll_px(scroll_drag_start_px +
                      (y - scroll_drag_mouse) * max_px / span);
    }
    if (!(buttons & 1))
        scroll_dragging = 0;
    prev_buttons = buttons;
}

static void handle_key(struct guiapp_ctx *ctx, int key) {
    if (input_mode != MODE_NONE) {
        if (key == '\r' || key == '\n') {
            finish_input(ctx);
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

static void configure_picker(const char *argument) {
    pick_mode = 0;
    pick_app[0] = 0;
    appui_copy_text(window_title, "Files", sizeof(window_title));
    if (!argument || !argument[0])
        return;
    if (starts_with(argument, "openfor:")) {
        pick_mode = 1;
        appui_copy_text(pick_app, argument + 8, sizeof(pick_app));
        appui_copy_text(window_title, "Open File", sizeof(window_title));
        set_status("Select a file, then Open");
    } else if (starts_with(argument, "newfor:")) {
        pick_mode = 2;
        appui_copy_text(pick_app, argument + 7, sizeof(pick_app));
        appui_copy_text(window_title, "New File", sizeof(window_title));
        set_status("Choose folder, then name the new file");
        begin_input(MODE_NEW_FILE);
        /* Prefer a sensible default name for the target app. */
        if (strstr(pick_app, "luaide")) {
            appui_copy_text(input_text, "untitled.lua", sizeof(input_text));
            input_len = (int)strlen(input_text);
        } else if (strstr(pick_app, "textedit")) {
            appui_copy_text(input_text, "untitled.txt", sizeof(input_text));
            input_len = (int)strlen(input_text);
        }
    }
}

int main(int argc, char **argv) {
    struct guiapp_ctx ctx;
    struct guiapp_event event;
    if (guiapp_parse_args(argc, argv, &ctx) < 0)
        return 1;
    if (argc > 4)
        configure_picker(argv[4]);
    (void)scan_directory();
    for (;;) {
        if (guiapp_read_event(&ctx, &event) < 0 || event.type == GUIAPP_EVT_CLOSE)
            break;
        if (event.type == GUIAPP_EVT_INIT || event.type == GUIAPP_EVT_RESIZE) {
            w = clamp_int(event.width, 220, MAX_W);
            h = clamp_int(event.height, 160, MAX_H);
            if (appui_pixels_ensure(&pixels, &pixels_cap, w, h, MAX_W, MAX_H) < 0)
                break;
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
        if (!pixels ||
            appui_pixels_ensure(&pixels, &pixels_cap, w, h, MAX_W, MAX_H) < 0)
            break;
        render();
        if (guiapp_send_frame(&ctx, window_title, w, h, pixels) < 0)
            break;
    }
    free(pixels);
    return 0;
}
