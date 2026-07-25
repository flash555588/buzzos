/* LuaIDE — simple desktop Lua editor with highlight, complete, save, REPL. */
#include "appui.h"
#include "guiapp.h"
#include "libc.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

enum {
    MAX_W = GUIAPP_MAX_W,
    MAX_H = GUIAPP_MAX_H,
    TEXT_CAP = 12288,
    OUT_CAP = 8192,
    REPL_CAP = 512,
    CHUNK_CAP = 4096,
    TOOLBAR_H = 50,
    STATUS_H = 24,
    REPL_BAR_H = 36,
    TOOL_BUTTON_H = 34,
    TOOL_BUTTON_GAP = 6,
    TOOL_BUTTON_PAD = 20,
    COMPLETE_MAX = 10,
    FOCUS_EDITOR = 0,
    FOCUS_REPL = 1,
    TB_OPEN = 0,
    TB_NEW = 1,
    TB_SAVE = 2,
    TB_RUN = 3,
    TB_REPL = 4,
    TB_CLEAR = 5,
    TB_COUNT = 6,
};

static const char *const TB_LABELS[TB_COUNT] = {
    "Open", "New", "Save", "Run", "REPL", "Clear"
};

static uint8_t pixels[MAX_W * MAX_H];
static char textbuf[TEXT_CAP];
static uint8_t tok_color[TEXT_CAP];
static int text_len;
static int cursor;
static int scroll_x;
static int scroll_y;
static int out_scroll_x;
static int out_scroll_y;
/* -1 none, 0 editor H, 1 editor V, 2 output H, 3 output V */
static int drag_scroll_axis = -1;
static int drag_mouse_start;
static int drag_scroll_start;
static int prev_buttons;
static int pointer_x = -1;
static int pointer_y = -1;
static int pointer_buttons;
static int w = 900;
static int h = 560;
static int focus = FOCUS_EDITOR;
static char status[80] = "Ready";
static char file_path[GUIAPP_PATH_MAX] = "/fs/luaide.lua";
static char window_title[GUIAPP_TITLE_MAX] = "LuaIDE";
static char output[OUT_CAP];
static int output_len;
static char repl_line[REPL_CAP];
static int repl_len;
static char repl_chunk[CHUNK_CAP];
static int repl_chunk_len;
static int complete_count;
static int complete_index;
static char complete_items[COMPLETE_MAX][32];
static int dirty_color = 1;
static lua_State *L;

/* Syntax palette (document is light bg, so use dark-ish hues). */
enum {
    COL_DEFAULT = 0,   /* black-ish */
    COL_KEYWORD = 1,   /* blue */
    COL_STRING  = 2,   /* green */
    COL_COMMENT = 3,   /* gray */
    COL_NUMBER  = 4,   /* brown/orange */
    COL_SYMBOL  = 5,
};

static int token_paint_color(int kind) {
    switch (kind) {
    case COL_KEYWORD: return 1;              /* blue */
    case COL_STRING:  return 2;              /* green */
    case COL_COMMENT: return plt_gray(8);
    case COL_NUMBER:  return 6;              /* brown */
    case COL_SYMBOL:  return plt_gray(4);
    default:          return THEME_DOCUMENT_TEXT;
    }
}

static const char *const LUA_WORDS[] = {
    "and", "break", "do", "else", "elseif", "end", "false", "for",
    "function", "goto", "if", "in", "local", "nil", "not", "or",
    "repeat", "return", "then", "true", "until", "while",
    "print", "pairs", "ipairs", "next", "type", "tostring", "tonumber",
    "require", "error", "assert", "pcall", "xpcall", "select",
    "rawget", "rawset", "rawequal", "setmetatable", "getmetatable",
    "math", "string", "table", "os", "io", "coroutine", "package",
    "utf8", "debug", "self", "module",
    0
};

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
    appui_copy_text(window_title, "LuaIDE - ", sizeof(window_title));
    appui_append_text(window_title, name, sizeof(window_title));
}

static int is_ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_ident(char c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static int is_keyword(const char *word, int len) {
    for (int i = 0; LUA_WORDS[i]; i++) {
        const char *k = LUA_WORDS[i];
        int j = 0;
        while (k[j] && j < len && k[j] == word[j])
            j++;
        if (k[j] == 0 && j == len)
            return 1;
    }
    return 0;
}

static void recolor(void) {
    int i = 0;
    while (i < text_len)
        tok_color[i++] = COL_DEFAULT;
    i = 0;
    while (i < text_len) {
        char c = textbuf[i];
        if (c == '-' && i + 1 < text_len && textbuf[i + 1] == '-') {
            int start = i;
            i += 2;
            if (i + 1 < text_len && textbuf[i] == '[' && textbuf[i + 1] == '[') {
                i += 2;
                while (i + 1 < text_len &&
                       !(textbuf[i] == ']' && textbuf[i + 1] == ']'))
                    i++;
                if (i + 1 < text_len)
                    i += 2;
            } else {
                while (i < text_len && textbuf[i] != '\n')
                    i++;
            }
            for (int j = start; j < i; j++)
                tok_color[j] = COL_COMMENT;
            continue;
        }
        if (c == '"' || c == '\'') {
            char quote = c;
            int start = i++;
            while (i < text_len && textbuf[i] != quote && textbuf[i] != '\n') {
                if (textbuf[i] == '\\' && i + 1 < text_len)
                    i++;
                i++;
            }
            if (i < text_len && textbuf[i] == quote)
                i++;
            for (int j = start; j < i; j++)
                tok_color[j] = COL_STRING;
            continue;
        }
        if (c == '[' && i + 1 < text_len && textbuf[i + 1] == '[') {
            int start = i;
            i += 2;
            while (i + 1 < text_len &&
                   !(textbuf[i] == ']' && textbuf[i + 1] == ']'))
                i++;
            if (i + 1 < text_len)
                i += 2;
            for (int j = start; j < i; j++)
                tok_color[j] = COL_STRING;
            continue;
        }
        if ((c >= '0' && c <= '9') ||
            (c == '.' && i + 1 < text_len &&
             textbuf[i + 1] >= '0' && textbuf[i + 1] <= '9')) {
            int start = i++;
            while (i < text_len &&
                   ((textbuf[i] >= '0' && textbuf[i] <= '9') ||
                    textbuf[i] == '.' || textbuf[i] == 'x' ||
                    textbuf[i] == 'X' || textbuf[i] == 'e' ||
                    textbuf[i] == 'E' || textbuf[i] == '+' ||
                    textbuf[i] == '-' ||
                    (textbuf[i] >= 'a' && textbuf[i] <= 'f') ||
                    (textbuf[i] >= 'A' && textbuf[i] <= 'F')))
                i++;
            for (int j = start; j < i; j++)
                tok_color[j] = COL_NUMBER;
            continue;
        }
        if (is_ident_start(c)) {
            int start = i++;
            while (i < text_len && is_ident(textbuf[i]))
                i++;
            int kind = is_keyword(textbuf + start, i - start) ? COL_KEYWORD
                                                              : COL_DEFAULT;
            for (int j = start; j < i; j++)
                tok_color[j] = (uint8_t)kind;
            continue;
        }
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            tok_color[i] = COL_SYMBOL;
        i++;
    }
    dirty_color = 0;
}

static void ensure_colors(void) {
    if (dirty_color)
        recolor();
}

static void out_clear(void) {
    output_len = 0;
    output[0] = 0;
    out_scroll_x = 0;
    out_scroll_y = 0;
}

static int max_out_scroll_y(void); /* defined with panel geometry below */

static void out_append(const char *s) {
    if (!s)
        return;
    while (*s && output_len + 1 < OUT_CAP)
        output[output_len++] = *s++;
    output[output_len] = 0;
    out_scroll_y = max_out_scroll_y();
}

static void out_append_n(const char *s, size_t n) {
    for (size_t i = 0; i < n && output_len + 1 < OUT_CAP; i++)
        output[output_len++] = s[i];
    output[output_len] = 0;
}

static int l_print(lua_State *state) {
    int n = lua_gettop(state);
    lua_getglobal(state, "tostring");
    for (int i = 1; i <= n; i++) {
        lua_pushvalue(state, -1);
        lua_pushvalue(state, i);
        if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
            const char *err = lua_tostring(state, -1);
            out_append(err ? err : "print error");
            out_append("\n");
            return 0;
        }
        const char *s = lua_tostring(state, -1);
        /* Standard Lua print separator is HT; the text pipeline advances
         * tabs to the next stop instead of drawing a glyph. */
        if (i > 1)
            out_append("\t");
        out_append(s ? s : "");
        lua_pop(state, 1);
    }
    out_append("\n");
    return 0;
}

static void lua_bind_print(void) {
    if (!L)
        return;
    lua_pushcfunction(L, l_print);
    lua_setglobal(L, "print");
}

static int lua_init(void) {
    if (L)
        return 0;
    L = luaL_newstate();
    if (!L) {
        set_status("Lua init failed");
        return -1;
    }
    luaL_openlibs(L);
    lua_bind_print();
    return 0;
}

static void lua_report_error(void) {
    const char *err = lua_tostring(L, -1);
    out_append(err ? err : "error");
    out_append("\n");
    lua_pop(L, 1);
}

static void run_buffer(void) {
    if (lua_init() < 0)
        return;
    out_append("=== Run ===\n");
    if (luaL_loadbuffer(L, textbuf, (size_t)text_len, file_path) != LUA_OK) {
        lua_report_error();
        set_status("Syntax error");
        return;
    }
    if (lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
        lua_report_error();
        set_status("Runtime error");
        return;
    }
    int n = lua_gettop(L);
    if (n > 0) {
        out_append("-- returns: ");
        lua_getglobal(L, "tostring");
        for (int i = 1; i <= n; i++) {
            lua_pushvalue(L, -1);
            lua_pushvalue(L, i);
            if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
                const char *s = lua_tostring(L, -1);
                if (i > 1)
                    out_append(", ");
                out_append(s ? s : "nil");
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        out_append("\n");
        lua_settop(L, 0);
    }
    set_status("Run ok");
}

static int load_looks_incomplete(void) {
    const char *msg = lua_tostring(L, -1);
    /* Lua reports unfinished chunks with near '<eof>'. */
    return msg && strstr(msg, "<eof>") != 0;
}

static void repl_eval(void) {
    if (lua_init() < 0)
        return;
    if (repl_len <= 0 && repl_chunk_len <= 0)
        return;

    out_append("> ");
    if (repl_chunk_len > 0) {
        out_append_n(repl_chunk, (size_t)repl_chunk_len);
        if (repl_chunk[repl_chunk_len - 1] != '\n')
            out_append("\n");
    }
    out_append_n(repl_line, (size_t)repl_len);
    out_append("\n");

    if (repl_chunk_len + repl_len + 2 >= CHUNK_CAP) {
        out_append("REPL chunk too large\n");
        repl_chunk_len = 0;
        repl_chunk[0] = 0;
        return;
    }
    if (repl_chunk_len > 0 && repl_chunk[repl_chunk_len - 1] != '\n')
        repl_chunk[repl_chunk_len++] = '\n';
    for (int i = 0; i < repl_len; i++)
        repl_chunk[repl_chunk_len++] = repl_line[i];
    repl_chunk[repl_chunk_len] = 0;

    /* Prefer expression mode: return <chunk> */
    char expr[CHUNK_CAP + 16];
    appui_copy_text(expr, "return ", sizeof(expr));
    appui_append_text(expr, repl_chunk, sizeof(expr));

    int status = luaL_loadbuffer(L, expr, strlen(expr), "=repl");
    if (status != LUA_OK) {
        lua_pop(L, 1);
        status = luaL_loadbuffer(L, repl_chunk, (size_t)repl_chunk_len, "=repl");
    }
    if (status != LUA_OK) {
        if (load_looks_incomplete()) {
            lua_pop(L, 1);
            set_status("... continue");
            repl_len = 0;
            repl_line[0] = 0;
            return;
        }
        lua_report_error();
        repl_chunk_len = 0;
        repl_chunk[0] = 0;
        set_status("REPL error");
        repl_len = 0;
        repl_line[0] = 0;
        return;
    }
    if (lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
        lua_report_error();
        set_status("REPL error");
    } else {
        int n = lua_gettop(L);
        if (n > 0) {
            lua_getglobal(L, "tostring");
            for (int i = 1; i <= n; i++) {
                lua_pushvalue(L, -1);
                lua_pushvalue(L, i);
                if (lua_pcall(L, 1, 1, 0) == LUA_OK) {
                    const char *s = lua_tostring(L, -1);
                    if (i > 1)
                        out_append("\t");
                    out_append(s ? s : "nil");
                }
                lua_pop(L, 1);
            }
            out_append("\n");
            lua_pop(L, 1);
            lua_settop(L, 0);
        }
        set_status("REPL ok");
    }
    repl_chunk_len = 0;
    repl_chunk[0] = 0;
    repl_len = 0;
    repl_line[0] = 0;
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
    dirty_color = 1;
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

/* Windows-style: browse with Files, then open the chosen path in LuaIDE. */
static void open_via_files(struct guiapp_ctx *ctx) {
    if (guiapp_request_launch(ctx, "/fs/apps/filemanager",
                              "openfor:/fs/apps/luaide") < 0)
        set_status("Could not open Files");
    else
        set_status("Pick a file in Files…");
}

static void new_via_files(struct guiapp_ctx *ctx) {
    if (guiapp_request_launch(ctx, "/fs/apps/filemanager",
                              "newfor:/fs/apps/luaide") < 0)
        set_status("Could not open Files");
    else
        set_status("Create a file in Files…");
}

static void insert_chars(const char *value, int n) {
    if (n <= 0 || text_len + n >= TEXT_CAP)
        return;
    for (int i = text_len; i >= cursor; i--)
        textbuf[i + n] = textbuf[i];
    for (int i = 0; i < n; i++)
        textbuf[cursor + i] = value[i];
    cursor += n;
    text_len += n;
    dirty_color = 1;
}

static void insert_char(char ch) {
    insert_chars(&ch, 1);
}

static void insert_text(const char *value) {
    insert_chars(value, (int)strlen(value));
}

static void backspace(void) {
    if (cursor <= 0)
        return;
    for (int i = cursor - 1; i < text_len; i++)
        textbuf[i] = textbuf[i + 1];
    cursor--;
    text_len--;
    dirty_color = 1;
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

static int width_between(int start, int end) {
    int width = 0;
    for (int i = start; i < end && i < text_len; i++) {
        if (textbuf[i] == '\n')
            break;
        width += appui_codepoint_advance((uint8_t)textbuf[i], width);
    }
    return width;
}

static int position_for_x(int start, int end, int target_x) {
    int x = 0;
    int pos = start;
    while (pos < end) {
        int glyph_w = appui_codepoint_advance((uint8_t)textbuf[pos], x);
        if (x + glyph_w / 2 >= target_x)
            break;
        x += glyph_w;
        pos++;
    }
    return pos;
}

static void cursor_up(void) {
    int start = line_start_of(cursor);
    int target_x = width_between(start, cursor);
    if (start == 0)
        return;
    int prev_end = start - 1;
    int prev_start = line_start_of(prev_end);
    cursor = position_for_x(prev_start, prev_end, target_x);
}

static void cursor_down(void) {
    int start = line_start_of(cursor);
    int target_x = width_between(start, cursor);
    int end = line_end_of(cursor);
    if (end >= text_len)
        return;
    int next_start = end + 1;
    int next_end = line_end_of(next_start);
    cursor = position_for_x(next_start, next_end, target_x);
}

static int line_count_of(const char *buf, int len) {
    int lines = 1;
    for (int i = 0; i < len; i++)
        if (buf[i] == '\n')
            lines++;
    return lines;
}

static int max_line_width(void) {
    int best = 0;
    int cur = 0;
    for (int i = 0; i < text_len; i++) {
        if (textbuf[i] == '\n') {
            if (cur > best)
                best = cur;
            cur = 0;
        } else {
            cur += appui_codepoint_advance((uint8_t)textbuf[i], cur);
        }
    }
    if (cur > best)
        best = cur;
    return best;
}

static int toolbar_button_width(int index) {
    int width = appui_text_width(TB_LABELS[index]) + TOOL_BUTTON_PAD;
    if (width < 52)
        width = 52;
    return width;
}

static struct appui_rect toolbar_button_rect(int index) {
    int x = 8;
    for (int i = 0; i < index; i++)
        x += toolbar_button_width(i) + TOOL_BUTTON_GAP;
    return (struct appui_rect){x, 8, toolbar_button_width(index), TOOL_BUTTON_H};
}

static int split_y(void) {
    int body = h - TOOLBAR_H - STATUS_H - REPL_BAR_H;
    int editor_h = body * 58 / 100;
    if (editor_h < 120)
        editor_h = 120;
    if (editor_h > body - 100)
        editor_h = body - 100;
    return TOOLBAR_H + editor_h;
}

static struct appui_rect editor_rect(void) {
    return (struct appui_rect){4, TOOLBAR_H + 2, w - 8, split_y() - TOOLBAR_H - 4};
}

static struct appui_rect text_clip_rect(void) {
    struct appui_rect e = editor_rect();
    /* Leave gutters for vertical + horizontal scrollbars. */
    return (struct appui_rect){e.x + 6, e.y + 4, e.w - 19, e.h - 15};
}

static struct appui_rect output_rect(void) {
    int y = split_y() + 2;
    int hh = h - y - STATUS_H - REPL_BAR_H - 2;
    if (hh < 40)
        hh = 40;
    return (struct appui_rect){4, y, w - 8, hh};
}

static struct appui_rect output_clip_rect(void) {
    struct appui_rect o = output_rect();
    return (struct appui_rect){o.x + 6, o.y + 4, o.w - 19, o.h - 15};
}

static struct appui_rect repl_rect(void) {
    return (struct appui_rect){4, h - STATUS_H - REPL_BAR_H, w - 8, REPL_BAR_H - 2};
}

static struct appui_rect status_rect(void) {
    return (struct appui_rect){0, h - STATUS_H, w, STATUS_H};
}

static int content_w(void) { return max_line_width() + 16; }
static int content_h(void) {
    return line_count_of(textbuf, text_len) * (KFONT_HEIGHT + 4) + 12;
}

static int out_line_step(void) { return KFONT_HEIGHT + 2; }

static int out_content_h(void) {
    return line_count_of(output, output_len) * out_line_step() + 8;
}

static int out_max_line_width(void) {
    int best = 0;
    int cur = 0;
    for (int i = 0; i < output_len; i++) {
        unsigned char c = (unsigned char)output[i];
        if (c == '\n') {
            if (cur > best)
                best = cur;
            cur = 0;
        } else {
            cur += appui_codepoint_advance(c, cur);
        }
    }
    if (cur > best)
        best = cur;
    return best;
}

static int out_content_w(void) { return out_max_line_width() + 16; }

static int max_scroll_x(void) {
    struct appui_rect c = text_clip_rect();
    return appui_max(0, content_w() - c.w);
}

static int max_scroll_y(void) {
    struct appui_rect c = text_clip_rect();
    return appui_max(0, content_h() - c.h);
}

static int max_out_scroll_x(void) {
    struct appui_rect c = output_clip_rect();
    return appui_max(0, out_content_w() - c.w);
}

static int max_out_scroll_y(void) {
    struct appui_rect c = output_clip_rect();
    return appui_max(0, out_content_h() - c.h);
}

static void clamp_scrolls(void) {
    scroll_x = clamp_int(scroll_x, 0, max_scroll_x());
    scroll_y = clamp_int(scroll_y, 0, max_scroll_y());
    out_scroll_x = clamp_int(out_scroll_x, 0, max_out_scroll_x());
    out_scroll_y = clamp_int(out_scroll_y, 0, max_out_scroll_y());
}

static struct appui_rect editor_vtrack(void) {
    struct appui_rect e = editor_rect();
    return (struct appui_rect){e.x + e.w - 11, e.y + 1, 10, e.h - 14};
}

static struct appui_rect editor_htrack(void) {
    struct appui_rect e = editor_rect();
    return (struct appui_rect){e.x + 1, e.y + e.h - 11, e.w - 14, 10};
}

static struct appui_rect editor_vthumb(void) {
    struct appui_rect t = editor_vtrack();
    int maxs = max_scroll_y();
    if (maxs <= 0)
        return t;
    int th = appui_max(22, t.h * t.h / appui_max(t.h, content_h()));
    if (th > t.h)
        th = t.h;
    return (struct appui_rect){t.x, t.y + scroll_y * (t.h - th) / maxs, t.w, th};
}

static struct appui_rect editor_hthumb(void) {
    struct appui_rect t = editor_htrack();
    int maxs = max_scroll_x();
    if (maxs <= 0)
        return t;
    int tw = appui_max(28, t.w * t.w / appui_max(t.w, content_w()));
    if (tw > t.w)
        tw = t.w;
    return (struct appui_rect){t.x + scroll_x * (t.w - tw) / maxs, t.y, tw, t.h};
}

static struct appui_rect output_vtrack(void) {
    struct appui_rect o = output_rect();
    return (struct appui_rect){o.x + o.w - 11, o.y + 1, 10, o.h - 14};
}

static struct appui_rect output_htrack(void) {
    struct appui_rect o = output_rect();
    return (struct appui_rect){o.x + 1, o.y + o.h - 11, o.w - 14, 10};
}

static struct appui_rect output_vthumb(void) {
    struct appui_rect t = output_vtrack();
    int maxs = max_out_scroll_y();
    if (maxs <= 0)
        return t;
    int th = appui_max(22, t.h * t.h / appui_max(t.h, out_content_h()));
    if (th > t.h)
        th = t.h;
    return (struct appui_rect){t.x, t.y + out_scroll_y * (t.h - th) / maxs, t.w,
                               th};
}

static struct appui_rect output_hthumb(void) {
    struct appui_rect t = output_htrack();
    int maxs = max_out_scroll_x();
    if (maxs <= 0)
        return t;
    int tw = appui_max(28, t.w * t.w / appui_max(t.w, out_content_w()));
    if (tw > t.w)
        tw = t.w;
    return (struct appui_rect){t.x + out_scroll_x * (t.w - tw) / maxs, t.y, tw,
                               t.h};
}

static void draw_scroll_thumb(struct appui_rect thumb, int vertical) {
    if (vertical) {
        thumb.x += 4;
        thumb.w -= 8;
    } else {
        thumb.y += 4;
        thumb.h -= 8;
    }
    appui_fill_round(pixels, w, h, thumb, THEME_WIN_HOVER);
}

static void draw_editor_scrollbars(void) {
    if (max_scroll_y() > 0)
        draw_scroll_thumb(editor_vthumb(), 1);
    if (max_scroll_x() > 0)
        draw_scroll_thumb(editor_hthumb(), 0);
}

static void draw_output_scrollbars(void) {
    if (max_out_scroll_y() > 0)
        draw_scroll_thumb(output_vthumb(), 1);
    if (max_out_scroll_x() > 0)
        draw_scroll_thumb(output_hthumb(), 0);
}

static void cursor_xy(int *x_out, int *y_out) {
    int x = 0;
    int y = 0;
    for (int pos = 0; pos < cursor && pos < text_len; pos++) {
        if (textbuf[pos] == '\n') {
            x = 0;
            y += KFONT_HEIGHT + 4;
        } else {
            x += appui_codepoint_advance((uint8_t)textbuf[pos], x);
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

static int position_at(int mx, int my) {
    struct appui_rect clip = text_clip_rect();
    int target_x = mx - clip.x + scroll_x;
    int target_y = my - clip.y + scroll_y;
    if (target_x < 0)
        target_x = 0;
    if (target_y < 0)
        target_y = 0;
    int wanted_line = target_y / (KFONT_HEIGHT + 4);
    int line = 0;
    int x = 0;
    int pos = 0;
    while (pos < text_len) {
        int start = pos;
        char c = textbuf[pos++];
        if (c == '\n') {
            if (line == wanted_line)
                return start;
            line++;
            x = 0;
            continue;
        }
        if (line == wanted_line) {
            int glyph_w = appui_codepoint_advance((uint8_t)c, x);
            if (target_x < x + glyph_w / 2)
                return start;
            x += glyph_w;
        } else if (line > wanted_line) {
            return start;
        }
    }
    return text_len;
}

static void word_prefix(char *prefix, int cap, int *start_out) {
    int start = cursor;
    while (start > 0 && is_ident(textbuf[start - 1]))
        start--;
    int len = cursor - start;
    if (len >= cap)
        len = cap - 1;
    for (int i = 0; i < len; i++)
        prefix[i] = textbuf[start + i];
    prefix[len] = 0;
    if (start_out)
        *start_out = start;
}

static void update_completions(void) {
    complete_count = 0;
    complete_index = 0;
    if (focus != FOCUS_EDITOR)
        return;
    char prefix[32];
    int start = 0;
    word_prefix(prefix, sizeof(prefix), &start);
    int plen = (int)strlen(prefix);
    if (plen <= 0)
        return;
    for (int i = 0; LUA_WORDS[i] && complete_count < COMPLETE_MAX; i++) {
        const char *k = LUA_WORDS[i];
        int j = 0;
        while (prefix[j] && k[j] && prefix[j] == k[j])
            j++;
        if (prefix[j] == 0 && k[j] != 0) {
            appui_copy_text(complete_items[complete_count], k, 32);
            complete_count++;
        }
    }
}

static void accept_completion(void) {
    if (complete_count <= 0)
        return;
    char prefix[32];
    int start = 0;
    word_prefix(prefix, sizeof(prefix), &start);
    const char *word = complete_items[complete_index];
    int plen = (int)strlen(prefix);
    int wlen = (int)strlen(word);
    if (wlen <= plen)
        return;
    insert_chars(word + plen, wlen - plen);
    complete_count = 0;
}

static void render_output(void) {
    struct appui_rect panel = output_rect();
    struct appui_rect clip = output_clip_rect();
    appui_fill(pixels, w, h, panel, THEME_FIELD_BG);
    appui_border(pixels, w, h, panel, THEME_FIELD_BORDER, THEME_DIVIDER);

    int line_origin = clip.x - out_scroll_x;
    int x = line_origin;
    int y = clip.y - out_scroll_y;
    for (int i = 0; i < output_len; i++) {
        unsigned char c = (unsigned char)output[i];
        if (c == '\n') {
            x = line_origin;
            y += out_line_step();
            continue;
        }
        int advance = appui_codepoint_advance(c, x - line_origin);
        if (y + KFONT_HEIGHT >= clip.y && y < clip.y + clip.h &&
            x + advance >= clip.x && x < clip.x + clip.w)
            (void)appui_draw_codepoint_at(pixels, w, h, x, y, c,
                                          THEME_FIELD_TEXT, -1, clip,
                                          line_origin);
        x += advance;
    }
    draw_output_scrollbars();
}

static void render_completions(void) {
    if (complete_count <= 0 || focus != FOCUS_EDITOR)
        return;
    int cx, cy;
    cursor_xy(&cx, &cy);
    struct appui_rect clip = text_clip_rect();
    int px = clip.x + cx - scroll_x;
    int py = clip.y + cy - scroll_y + KFONT_HEIGHT + 2;
    int box_h = complete_count * (KFONT_HEIGHT + 4) + 6;
    int box_w = 160;
    if (py + box_h > h - STATUS_H - REPL_BAR_H)
        py = clip.y + cy - scroll_y - box_h - 2;
    struct appui_rect box = {px, py, box_w, box_h};
    appui_fill(pixels, w, h, box, THEME_WIN_PANEL);
    appui_border(pixels, w, h, box, THEME_ACCENT, THEME_DIVIDER);
    for (int i = 0; i < complete_count; i++) {
        int row_y = py + 3 + i * (KFONT_HEIGHT + 4);
        if (i == complete_index)
            appui_fill(pixels, w, h,
                       (struct appui_rect){px + 2, row_y - 1, box_w - 4,
                                           KFONT_HEIGHT + 2},
                       THEME_SELECTION_BG);
        appui_text(pixels, w, h, px + 6, row_y, complete_items[i], THEME_TEXT, -1,
                   box);
    }
}

static void render(void) {
    ensure_colors();
    clamp_scrolls();
    appui_fill(pixels, w, h, (struct appui_rect){0, 0, w, h}, THEME_APP_BG);
    appui_fill(pixels, w, h, (struct appui_rect){0, 0, w, TOOLBAR_H},
               THEME_TOOLBAR_BG);

    for (int i = 0; i < TB_COUNT; i++) {
        struct appui_rect r = toolbar_button_rect(i);
        int variant = APPUI_BTN_DEFAULT;
        if (i == TB_RUN)
            variant = APPUI_BTN_PRIMARY;
        appui_button_ex(pixels, w, h, r, TB_LABELS[i], variant,
                        appui_pointer_state(r, pointer_x, pointer_y,
                                            pointer_buttons));
    }

    /* Editor */
    struct appui_rect editor = editor_rect();
    appui_fill(pixels, w, h, editor, THEME_DOCUMENT_BG);
    appui_border(pixels, w, h, editor,
                 focus == FOCUS_EDITOR ? THEME_ACCENT : THEME_FIELD_BORDER,
                 THEME_DIVIDER);
    struct appui_rect clip = text_clip_rect();
    int line_origin = clip.x - scroll_x;
    int x = line_origin;
    int y = clip.y - scroll_y;
    int cur_x = x;
    int cur_y = y;
    for (int pos = 0; pos <= text_len; pos++) {
        if (pos == cursor) {
            cur_x = x;
            cur_y = y;
        }
        if (pos == text_len)
            break;
        unsigned char c = (unsigned char)textbuf[pos];
        if (c == '\n') {
            x = line_origin;
            y += KFONT_HEIGHT + 4;
            continue;
        }
        int advance = appui_codepoint_advance(c, x - line_origin);
        int col = token_paint_color(tok_color[pos]);
        if (y + KFONT_HEIGHT >= clip.y && y < clip.y + clip.h &&
            x + advance >= clip.x && x < clip.x + clip.w)
            (void)appui_draw_codepoint_at(pixels, w, h, x, y, c, col, -1, clip,
                                          line_origin);
        x += advance;
    }
    if (focus == FOCUS_EDITOR)
        appui_fill(pixels, w, h,
                   (struct appui_rect){cur_x, cur_y - PLT_FONT_Y_SHIFT, 2,
                                       KFONT_HEIGHT + 2},
                   THEME_FOCUS);
    draw_editor_scrollbars();

    render_output();

    /* REPL bar */
    struct appui_rect rr = repl_rect();
    appui_fill(pixels, w, h, rr,
               focus == FOCUS_REPL ? THEME_FIELD_BG : THEME_WIN_PANEL);
    appui_border(pixels, w, h, rr,
                 focus == FOCUS_REPL ? THEME_ACCENT : THEME_FIELD_BORDER,
                 THEME_DIVIDER);
    appui_text(pixels, w, h, rr.x + 6, rr.y + 8, "lua>", THEME_ACCENT, -1, rr);
    int rx = rr.x + 6 + appui_text_width("lua> ");
    appui_text(pixels, w, h, rx, rr.y + 8, repl_line, THEME_FIELD_TEXT, -1, rr);
    if (focus == FOCUS_REPL) {
        int caret_x = rx + appui_text_width(repl_line);
        appui_fill(pixels, w, h,
                   (struct appui_rect){caret_x, rr.y + 6, 2, KFONT_HEIGHT},
                   THEME_FOCUS);
    }

    /* Status */
    struct appui_rect sr = status_rect();
    appui_fill(pixels, w, h, sr, THEME_TOOLBAR_BG);
    char lineinfo[96];
    appui_copy_text(lineinfo, status, sizeof(lineinfo));
    appui_append_text(lineinfo, "  |  ", sizeof(lineinfo));
    appui_append_text(lineinfo, file_path, sizeof(lineinfo));
    appui_text(pixels, w, h, 8, sr.y + 4, lineinfo, THEME_TEXT_DIM, -1, sr);

    render_completions();
}

static void toolbar_click(struct guiapp_ctx *ctx, int index) {
    if (index == TB_OPEN)
        open_via_files(ctx);
    else if (index == TB_NEW)
        new_via_files(ctx);
    else if (index == TB_SAVE)
        save_file();
    else if (index == TB_RUN) {
        run_buffer();
        focus = FOCUS_EDITOR;
    } else if (index == TB_REPL) {
        focus = FOCUS_REPL;
        set_status("REPL focus — Enter to eval");
    } else if (index == TB_CLEAR) {
        out_clear();
        set_status("Output cleared");
    }
}

static void mouse(struct guiapp_ctx *ctx, int mx, int my, int buttons, int wheel) {
    pointer_x = mx;
    pointer_y = my;
    pointer_buttons = buttons;
    int pressed = (buttons & 1) && !(prev_buttons & 1);

    if (wheel) {
        if (appui_inside(mx, my, output_rect()))
            out_scroll_y -= wheel * 40;
        else if (appui_inside(mx, my, editor_rect()))
            scroll_y -= wheel * 40;
    }

    if (pressed) {
        for (int i = 0; i < TB_COUNT; i++) {
            if (appui_inside(mx, my, toolbar_button_rect(i))) {
                toolbar_click(ctx, i);
                prev_buttons = buttons;
                return;
            }
        }
        if (max_scroll_y() > 0 && appui_inside(mx, my, editor_vtrack())) {
            struct appui_rect t = editor_vtrack();
            struct appui_rect th = editor_vthumb();
            if (!appui_inside(mx, my, th)) {
                int span = appui_max(1, t.h - th.h);
                scroll_y = (my - t.y - th.h / 2) * max_scroll_y() / span;
                clamp_scrolls();
            }
            drag_scroll_axis = 1;
            drag_mouse_start = my;
            drag_scroll_start = scroll_y;
        } else if (max_scroll_x() > 0 && appui_inside(mx, my, editor_htrack())) {
            struct appui_rect t = editor_htrack();
            struct appui_rect th = editor_hthumb();
            if (!appui_inside(mx, my, th)) {
                int span = appui_max(1, t.w - th.w);
                scroll_x = (mx - t.x - th.w / 2) * max_scroll_x() / span;
                clamp_scrolls();
            }
            drag_scroll_axis = 0;
            drag_mouse_start = mx;
            drag_scroll_start = scroll_x;
        } else if (max_out_scroll_y() > 0 &&
                   appui_inside(mx, my, output_vtrack())) {
            struct appui_rect t = output_vtrack();
            struct appui_rect th = output_vthumb();
            if (!appui_inside(mx, my, th)) {
                int span = appui_max(1, t.h - th.h);
                out_scroll_y = (my - t.y - th.h / 2) * max_out_scroll_y() / span;
                clamp_scrolls();
            }
            drag_scroll_axis = 3;
            drag_mouse_start = my;
            drag_scroll_start = out_scroll_y;
        } else if (max_out_scroll_x() > 0 &&
                   appui_inside(mx, my, output_htrack())) {
            struct appui_rect t = output_htrack();
            struct appui_rect th = output_hthumb();
            if (!appui_inside(mx, my, th)) {
                int span = appui_max(1, t.w - th.w);
                out_scroll_x = (mx - t.x - th.w / 2) * max_out_scroll_x() / span;
                clamp_scrolls();
            }
            drag_scroll_axis = 2;
            drag_mouse_start = mx;
            drag_scroll_start = out_scroll_x;
        } else if (appui_inside(mx, my, text_clip_rect())) {
            focus = FOCUS_EDITOR;
            cursor = position_at(mx, my);
            ensure_cursor_visible();
            update_completions();
        } else if (appui_inside(mx, my, repl_rect())) {
            focus = FOCUS_REPL;
            complete_count = 0;
        } else if (appui_inside(mx, my, output_clip_rect())) {
            complete_count = 0;
        }
    }
    if ((buttons & 1) && drag_scroll_axis >= 0) {
        if (drag_scroll_axis == 1) {
            struct appui_rect t = editor_vtrack();
            struct appui_rect th = editor_vthumb();
            int span = appui_max(1, t.h - th.h);
            scroll_y = drag_scroll_start +
                       (my - drag_mouse_start) * max_scroll_y() / span;
        } else if (drag_scroll_axis == 0) {
            struct appui_rect t = editor_htrack();
            struct appui_rect th = editor_hthumb();
            int span = appui_max(1, t.w - th.w);
            scroll_x = drag_scroll_start +
                       (mx - drag_mouse_start) * max_scroll_x() / span;
        } else if (drag_scroll_axis == 3) {
            struct appui_rect t = output_vtrack();
            struct appui_rect th = output_vthumb();
            int span = appui_max(1, t.h - th.h);
            out_scroll_y = drag_scroll_start +
                           (my - drag_mouse_start) * max_out_scroll_y() / span;
        } else if (drag_scroll_axis == 2) {
            struct appui_rect t = output_htrack();
            struct appui_rect th = output_hthumb();
            int span = appui_max(1, t.w - th.w);
            out_scroll_x = drag_scroll_start +
                           (mx - drag_mouse_start) * max_out_scroll_x() / span;
        }
    }
    if (!(buttons & 1))
        drag_scroll_axis = -1;
    prev_buttons = buttons;
    clamp_scrolls();
}

static void key_editor(int k) {
    if (k == GUIAPP_KEY_ESC) {
        complete_count = 0;
        return;
    }
    if (complete_count > 0) {
        if (k == GUIAPP_KEY_UP) {
            complete_index = (complete_index + complete_count - 1) % complete_count;
            return;
        }
        if (k == GUIAPP_KEY_DOWN) {
            complete_index = (complete_index + 1) % complete_count;
            return;
        }
        if (k == '\t') {
            accept_completion();
            ensure_cursor_visible();
            return;
        }
        if (k == '\r' || k == '\n') {
            accept_completion();
            ensure_cursor_visible();
            return;
        }
    }
    if (k == '\t') {
        update_completions();
        if (complete_count > 0)
            accept_completion();
        else
            insert_chars("    ", 4);
        ensure_cursor_visible();
        return;
    }
    if (k == GUIAPP_KEY_BACKSPACE || k == 127) {
        backspace();
        update_completions();
    } else if (k == GUIAPP_KEY_LEFT && cursor > 0) {
        cursor--;
        complete_count = 0;
    } else if (k == GUIAPP_KEY_RIGHT && cursor < text_len) {
        cursor++;
        complete_count = 0;
    } else if (k == GUIAPP_KEY_UP) {
        cursor_up();
        complete_count = 0;
    } else if (k == GUIAPP_KEY_DOWN) {
        cursor_down();
        complete_count = 0;
    } else if (k == '\r' || k == '\n') {
        insert_char('\n');
        complete_count = 0;
    } else if (k >= 32 && k < 127) {
        insert_char((char)k);
        update_completions();
    }
    ensure_cursor_visible();
}

static void key_repl(int k) {
    if (k == GUIAPP_KEY_ESC) {
        focus = FOCUS_EDITOR;
        return;
    }
    if (k == GUIAPP_KEY_BACKSPACE || k == 127) {
        if (repl_len > 0)
            repl_line[--repl_len] = 0;
        return;
    }
    if (k == '\r' || k == '\n') {
        repl_eval();
        return;
    }
    if (k >= 32 && k < 127 && repl_len + 1 < REPL_CAP) {
        repl_line[repl_len++] = (char)k;
        repl_line[repl_len] = 0;
    }
}

static void key(int k) {
    if (k == GUIAPP_KEY_ESC && focus == FOCUS_EDITOR && complete_count == 0)
        focus = FOCUS_REPL;
    if (focus == FOCUS_REPL)
        key_repl(k);
    else
        key_editor(k);
}

static void report_caret(struct guiapp_ctx *ctx) {
    if (focus == FOCUS_REPL) {
        struct appui_rect rr = repl_rect();
        int x = rr.x + 6 + appui_text_width("lua> ") + appui_text_width(repl_line);
        int y = rr.y + 8;
        (void)guiapp_send_caret(ctx, x, y);
        return;
    }
    int cx, cy;
    cursor_xy(&cx, &cy);
    struct appui_rect clip = text_clip_rect();
    (void)guiapp_send_caret(ctx, clip.x + cx - scroll_x, clip.y + cy - scroll_y);
}

int main(int argc, char **argv) {
    struct guiapp_ctx ctx;
    struct guiapp_event ev;
    if (guiapp_parse_args(argc, argv, &ctx) < 0)
        return 1;
    if (argc > 4)
        set_document_path(argv[4]);
    load_file();
    out_append("LuaIDE ready. Open/New use Files (like a desktop dialog).\n");
    out_append("Run executes the buffer; REPL evaluates lines. Tab completes.\n");

    for (;;) {
        if (guiapp_read_event(&ctx, &ev) < 0 || ev.type == GUIAPP_EVT_CLOSE)
            break;
        if (ev.type == GUIAPP_EVT_INIT || ev.type == GUIAPP_EVT_RESIZE) {
            w = clamp_int(ev.width, 480, MAX_W);
            h = clamp_int(ev.height, 320, MAX_H);
            ensure_cursor_visible();
        } else if (ev.type == GUIAPP_EVT_KEY && ev.buttons) {
            key(ev.key);
        } else if (ev.type == GUIAPP_EVT_TEXT) {
            if (focus == FOCUS_REPL) {
                for (int i = 0; ev.text[i] && repl_len + 1 < REPL_CAP; i++) {
                    if ((unsigned char)ev.text[i] >= 32)
                        repl_line[repl_len++] = ev.text[i];
                }
                repl_line[repl_len] = 0;
            } else {
                insert_text(ev.text);
                update_completions();
                ensure_cursor_visible();
            }
        } else if (ev.type == GUIAPP_EVT_MOUSE) {
            mouse(&ctx, ev.x, ev.y, ev.buttons, ev.wheel);
        }
        render();
        if (guiapp_send_frame(&ctx, window_title, w, h, pixels) < 0)
            break;
        report_caret(&ctx);
    }

    if (L) {
        lua_close(L);
        L = 0;
    }
    return 0;
}
