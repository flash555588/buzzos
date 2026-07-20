#include "appui.h"
#include "guiapp.h"
#include "libc.h"

enum {
    MAX_W = GUIAPP_MAX_W,
    MAX_H = GUIAPP_MAX_H,
    URL_CAP = 256,
    HOST_CAP = 96,
    PATH_CAP = 256,
    RESPONSE_CAP = 32768,
    TEXT_CAP = 24576,
    HISTORY_MAX = 12,
};

static uint8_t pixels[MAX_W * MAX_H];
static char response[RESPONSE_CAP];
static char page_text[TEXT_CAP];
static char url[URL_CAP] = "http://example.com/";
static char history[HISTORY_MAX][URL_CAP];
static char status[96] = "Enter an http:// URL";
static char title[GUIAPP_TITLE_MAX] = "Browser";
static int url_len;
static int text_len;
static int response_len;
static int scroll_y;
static int w = 640;
static int h = 420;
static int prev_buttons;
static int history_pos = -1;
static int enter_armed = 1;

static int clamp_int(int value, int lo, int hi) {
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static int starts_with(const char *s, const char *prefix) {
    while (*prefix)
        if (*s++ != *prefix++)
            return 0;
    return 1;
}

static void set_status(const char *text) {
    appui_copy_text(status, text, sizeof(status));
}

static int parse_ipv4(const char *s, uint32_t *out) {
    uint32_t parts[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') return -1;
        while (*s >= '0' && *s <= '9') {
            parts[i] = parts[i] * 10u + (uint32_t)(*s++ - '0');
            if (parts[i] > 255u) return -1;
        }
        if (i < 3) {
            if (*s++ != '.') return -1;
        } else if (*s) {
            return -1;
        }
    }
    *out = parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
    return 0;
}

static int parse_url(const char *input, char *host, int *port, char *path) {
    const char *p = input;
    if (starts_with(p, "https://")) {
        set_status("HTTPS unavailable: TLS is not implemented");
        return -1;
    }
    if (starts_with(p, "http://"))
        p += 7;
    if (!p[0]) {
        set_status("URL has no host");
        return -1;
    }
    int hn = 0;
    while (*p && *p != '/' && *p != ':' && hn < HOST_CAP - 1)
        host[hn++] = *p++;
    host[hn] = 0;
    if (!host[0] || (*p && *p != '/' && *p != ':')) {
        set_status("Host is too long");
        return -1;
    }
    *port = 80;
    if (*p == ':') {
        p++;
        int value = 0;
        if (*p < '0' || *p > '9') {
            set_status("Invalid port");
            return -1;
        }
        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (*p++ - '0');
            if (value > 65535) break;
        }
        if (value <= 0 || value > 65535) {
            set_status("Invalid port");
            return -1;
        }
        *port = value;
    }
    if (!*p)
        appui_copy_text(path, "/", PATH_CAP);
    else if (*p == '/')
        appui_copy_text(path, p, PATH_CAP);
    else {
        set_status("Invalid URL path");
        return -1;
    }
    return 0;
}

static void append_port(char *buffer, int *length, int cap, int port) {
    char reversed[8];
    int n = 0;
    do {
        reversed[n++] = (char)('0' + port % 10);
        port /= 10;
    } while (port && n < (int)sizeof(reversed));
    while (n > 0 && *length < cap)
        buffer[(*length)++] = reversed[--n];
}

static int send_all(int socket_fd, const char *buffer, int length) {
    int sent = 0;
    while (sent < length) {
        int n = send(socket_fd, buffer + sent, (size_t)(length - sent), 0);
        if (n <= 0)
            return -1;
        sent += n;
    }
    return 0;
}

static int http_get(const char *host, int port, const char *path) {
    uint32_t ip;
    set_status("Resolving host...");
    if (parse_ipv4(host, &ip) < 0 && dns_resolve(host, &ip) < 0) {
        set_status("DNS lookup failed");
        return -1;
    }
    int sd = socket(AF_INET, SOCK_STREAM, 0);
    if (sd < 0) {
        set_status("Cannot create TCP socket");
        return -1;
    }
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr = ip;
    set_status("Connecting...");
    if (connect(sd, &address, sizeof(address)) < 0) {
        closesocket(sd);
        set_status("Connection failed");
        return -1;
    }
    char request[640];
    int n = 0;
    const char *pieces[] = {"GET ", path, " HTTP/1.0\r\nHost: ", host, 0};
    for (int i = 0; pieces[i]; i++)
        for (int j = 0; pieces[i][j] && n < (int)sizeof(request); j++)
            request[n++] = pieces[i][j];
    if (port != 80) {
        if (n < (int)sizeof(request)) request[n++] = ':';
        append_port(request, &n, sizeof(request), port);
    }
    /* Keep the transport request identical to the shell's proven wget path.
     * Rendering policy belongs above HTTP and must not change wire behavior. */
    const char *tail =
        "\r\nUser-Agent: BuzzOS-socket/1.0\r\nConnection: close\r\n\r\n";
    for (int i = 0; tail[i] && n < (int)sizeof(request); i++)
        request[n++] = tail[i];
    if (send_all(sd, request, n) < 0) {
        closesocket(sd);
        set_status("HTTP request failed");
        return -1;
    }
    set_status("Receiving...");
    response_len = 0;
    while (response_len < RESPONSE_CAP - 1) {
        int got = recv(sd, response + response_len,
                       (size_t)(RESPONSE_CAP - 1 - response_len), 0);
        if (got < 0) {
            closesocket(sd);
            set_status("Receive failed");
            return -1;
        }
        if (got == 0)
            break;
        response_len += got;
    }
    closesocket(sd);
    response[response_len] = 0;
    if (response_len == RESPONSE_CAP - 1)
        set_status("Loaded (response truncated)");
    else
        set_status("Loaded");
    return 0;
}

static int ascii_lower(int c) {
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static int tag_name_is(const char *tag, int len, const char *name) {
    int n = (int)strlen(name);
    if (len < n) return 0;
    for (int i = 0; i < n; i++)
        if (ascii_lower(tag[i]) != name[i])
            return 0;
    return len == n || tag[n] == ' ' || tag[n] == '\t' || tag[n] == '/';
}

static void text_char(char c) {
    if (text_len >= TEXT_CAP - 1)
        return;
    if (c == '\n') {
        while (text_len > 0 && page_text[text_len - 1] == ' ')
            text_len--;
        if (text_len == 0 || page_text[text_len - 1] != '\n')
            page_text[text_len++] = '\n';
        return;
    }
    if (c == ' ' && (text_len == 0 || page_text[text_len - 1] == ' ' ||
                     page_text[text_len - 1] == '\n'))
        return;
    page_text[text_len++] = c;
}

static int decode_entity(const char *p, int remaining, char *value) {
    struct entity { const char *name; char value; };
    static const struct entity entities[] = {
        {"amp;", '&'}, {"lt;", '<'}, {"gt;", '>'}, {"quot;", '"'},
        {"apos;", '\''}, {"nbsp;", ' '},
    };
    for (int i = 0; i < (int)(sizeof(entities) / sizeof(entities[0])); i++) {
        int n = (int)strlen(entities[i].name);
        if (remaining >= n) {
            int same = 1;
            for (int j = 0; j < n; j++)
                if (p[j] != entities[i].name[j]) same = 0;
            if (same) {
                *value = entities[i].value;
                return n;
            }
        }
    }
    return 0;
}

static void html_to_text(const char *body, int length) {
    text_len = 0;
    int skip = 0;
    for (int i = 0; i < length && text_len < TEXT_CAP - 1;) {
        if (body[i] == '<') {
            int end = i + 1;
            while (end < length && body[end] != '>') end++;
            int begin = i + 1;
            while (begin < end && (body[begin] == ' ' || body[begin] == '\t')) begin++;
            int closing = begin < end && body[begin] == '/';
            if (closing) begin++;
            int tag_len = end - begin;
            if (!closing && (tag_name_is(body + begin, tag_len, "script") ||
                             tag_name_is(body + begin, tag_len, "style")))
                skip++;
            else if (closing && (tag_name_is(body + begin, tag_len, "script") ||
                                 tag_name_is(body + begin, tag_len, "style"))) {
                if (skip > 0) skip--;
            } else if (!skip && (tag_name_is(body + begin, tag_len, "br") ||
                       tag_name_is(body + begin, tag_len, "p") ||
                       tag_name_is(body + begin, tag_len, "div") ||
                       tag_name_is(body + begin, tag_len, "li") ||
                       tag_name_is(body + begin, tag_len, "h1") ||
                       tag_name_is(body + begin, tag_len, "h2") ||
                       tag_name_is(body + begin, tag_len, "h3")))
                text_char('\n');
            i = end < length ? end + 1 : length;
            continue;
        }
        if (!skip && body[i] == '&') {
            char decoded;
            int used = decode_entity(body + i + 1, length - i - 1, &decoded);
            if (used) {
                text_char(decoded);
                i += used + 1;
                continue;
            }
        }
        if (!skip) {
            char c = body[i];
            text_char(c == '\r' || c == '\n' || c == '\t' ? ' ' : c);
        }
        i++;
    }
    while (text_len > 0 && (page_text[text_len - 1] == ' ' ||
                            page_text[text_len - 1] == '\n'))
        text_len--;
    page_text[text_len] = 0;
}

static struct appui_rect page_rect(void) {
    return (struct appui_rect){10, 58, w - 20, h - 84};
}

static int line_width(void) {
    struct appui_rect page = page_rect();
    int value = page.w - 16;
    return value > KFONT_WIDTH * 8 ? value : KFONT_WIDTH * 8;
}

static int visual_lines(void) {
    int x = 0;
    int lines = 1;
    int limit = line_width();
    int pos = 0;
    while (pos < text_len) {
        const char *p = page_text + pos;
        uint32_t cp = appui_utf8_next(&p);
        pos = (int)(p - page_text);
        if (cp == '\n') {
            lines++;
            x = 0;
        } else {
            int glyph_w = appui_codepoint_width(cp);
            if (x > 0 && x + glyph_w > limit) {
                lines++;
                x = 0;
            }
            x += glyph_w;
        }
    }
    return lines;
}

static int max_scroll(void) {
    struct appui_rect page = page_rect();
    int content = visual_lines() * (KFONT_HEIGHT + 4) + 16;
    return appui_max(0, content - page.h);
}

static void clamp_scroll(void) {
    scroll_y = clamp_int(scroll_y, 0, max_scroll());
}

static void draw_page(void) {
    struct appui_rect page = page_rect();
    struct appui_rect clip = {page.x + 8, page.y + 8, page.w - 16, page.h - 16};
    appui_fill(pixels, w, h, page, 15);
    appui_border(pixels, w, h, page, appui_gray(8), appui_gray(1));
    int x = clip.x;
    int y = clip.y - scroll_y;
    int pos = 0;
    while (pos < text_len) {
        const char *p = page_text + pos;
        uint32_t cp = appui_utf8_next(&p);
        pos = (int)(p - page_text);
        if (cp == '\n') {
            x = clip.x;
            y += KFONT_HEIGHT + 4;
            continue;
        }
        int glyph_w = appui_codepoint_width(cp);
        if (x > clip.x && x + glyph_w > clip.x + clip.w) {
            x = clip.x;
            y += KFONT_HEIGHT + 4;
        }
        if (y + KFONT_HEIGHT >= clip.y && y < clip.y + clip.h) {
            appui_draw_codepoint(pixels, w, h, x, y, cp, 0, -1, clip);
        }
        x += glyph_w;
        if (y >= clip.y + clip.h && scroll_y + clip.h < max_scroll())
            break;
    }
    if (max_scroll() > 0) {
        int track_h = page.h - 4;
        int thumb_h = appui_max(24, track_h * page.h /
                                appui_max(page.h, visual_lines() * (KFONT_HEIGHT + 4)));
        int thumb_y = page.y + 2 + scroll_y * (track_h - thumb_h) / max_scroll();
        appui_fill(pixels, w, h, (struct appui_rect){page.x + page.w - 7, page.y + 2, 5, track_h},
                   appui_gray(3));
        appui_fill(pixels, w, h, (struct appui_rect){page.x + page.w - 7, thumb_y, 5, thumb_h},
                   appui_gray(9));
    }
}

static void render(void) {
    clamp_scroll();
    appui_fill(pixels, w, h, (struct appui_rect){0, 0, w, h}, appui_gray(3));
    appui_fill(pixels, w, h, (struct appui_rect){0, 0, w, 50}, appui_gray(2));
    appui_button(pixels, w, h, (struct appui_rect){8, 10, 58, 28}, "Back", history_pos > 0);
    struct appui_rect address = {74, 10, w - 150, 28};
    appui_fill(pixels, w, h, address, 15);
    appui_border(pixels, w, h, address, appui_gray(8), appui_gray(1));
    appui_text(pixels, w, h, address.x + 6, address.y + 7, url, 0, -1,
               (struct appui_rect){address.x + 5, address.y + 3, address.w - 10, address.h - 6});
    int cursor_x = address.x + 6 + appui_text_width(url);
    if (cursor_x < address.x + address.w - 5)
        appui_fill(pixels, w, h, (struct appui_rect){cursor_x, address.y + 5, 1, 17},
                   appui_rgb6(0, 2, 5));
    appui_button(pixels, w, h, (struct appui_rect){w - 68, 10, 58, 28}, "Go", 1);
    draw_page();
    appui_fill(pixels, w, h, (struct appui_rect){0, h - 22, w, 22}, appui_gray(2));
    appui_text(pixels, w, h, 10, h - 18, status, appui_gray(12), -1,
               (struct appui_rect){8, h - 21, w - 16, 20});
}

static int find_body(void) {
    for (int i = 0; i + 3 < response_len; i++)
        if (response[i] == '\r' && response[i + 1] == '\n' &&
            response[i + 2] == '\r' && response[i + 3] == '\n')
            return i + 4;
    for (int i = 0; i + 1 < response_len; i++)
        if (response[i] == '\n' && response[i + 1] == '\n')
            return i + 2;
    return 0;
}

static void remember_url(void) {
    if (history_pos >= 0 && strcmp(history[history_pos], url) == 0)
        return;
    if (history_pos + 1 < HISTORY_MAX) {
        history_pos++;
    } else {
        for (int i = 1; i < HISTORY_MAX; i++)
            appui_copy_text(history[i - 1], history[i], URL_CAP);
        history_pos = HISTORY_MAX - 1;
    }
    appui_copy_text(history[history_pos], url, URL_CAP);
}

static void load_url(int remember) {
    char host[HOST_CAP];
    char path[PATH_CAP];
    int port;
    if (parse_url(url, host, &port, path) < 0)
        return;
    if (http_get(host, port, path) < 0)
        return;
    int body = find_body();
    html_to_text(response + body, response_len - body);
    scroll_y = 0;
    appui_copy_text(title, "Browser - ", sizeof(title));
    appui_append_text(title, host, sizeof(title));
    if (remember)
        remember_url();
    if (response_len >= 12 && starts_with(response, "HTTP/")) {
        char http_status[64] = "HTTP ";
        int j = 5;
        while (j < response_len && response[j] != ' ') j++;
        if (j < response_len) j++;
        int n = (int)strlen(http_status);
        while (j < response_len && response[j] != '\r' && response[j] != '\n' &&
               n < (int)sizeof(http_status) - 1)
            http_status[n++] = response[j++];
        http_status[n] = 0;
        set_status(http_status);
    }
}

static void go_back(void) {
    if (history_pos <= 0) {
        set_status("No previous page");
        return;
    }
    history_pos--;
    appui_copy_text(url, history[history_pos], sizeof(url));
    url_len = (int)strlen(url);
    load_url(0);
}

static void key(int value) {
    if (value == GUIAPP_KEY_BACKSPACE || value == 127) {
        if (url_len > 0) {
            url_len = appui_utf8_prev(url, url_len);
            url[url_len] = 0;
            enter_armed = 1;
        }
    } else if (value == '\r' || value == '\n') {
        if (enter_armed) {
            enter_armed = 0;
            load_url(1);
        }
    } else if (value == GUIAPP_KEY_UP) {
        scroll_y -= 32;
    } else if (value == GUIAPP_KEY_DOWN) {
        scroll_y += 32;
    } else if (value >= 32 && value < 127 && url_len < URL_CAP - 1) {
        enter_armed = 1;
        url[url_len++] = (char)value;
        url[url_len] = 0;
    }
    clamp_scroll();
}

static void text_input(const char *value) {
    int n = (int)strlen(value);
    if (n <= 0 || url_len + n >= URL_CAP)
        return;
    for (int i = 0; i < n; i++)
        url[url_len++] = value[i];
    url[url_len] = 0;
    enter_armed = 1;
}

static void command(struct guiapp_ctx *ctx, int value) {
    if (value != GUIAPP_CMD_COPY && value != GUIAPP_CMD_CUT)
        return;
    (void)guiapp_set_clipboard(ctx, url);
    if (value == GUIAPP_CMD_CUT) {
        url_len = 0;
        url[0] = 0;
        enter_armed = 1;
    }
}

static void mouse(int x, int y, int buttons, int wheel) {
    int pressed = (buttons & 1) && !(prev_buttons & 1);
    if (wheel)
        scroll_y -= wheel * 44;
    if (pressed) {
        if (appui_inside(x, y, (struct appui_rect){8, 10, 58, 28}))
            go_back();
        else if (appui_inside(x, y, (struct appui_rect){w - 68, 10, 58, 28}))
            load_url(1);
    }
    prev_buttons = buttons;
    clamp_scroll();
}

int main(int argc, char **argv) {
    struct guiapp_ctx ctx;
    struct guiapp_event event;
    if (guiapp_parse_args(argc, argv, &ctx) < 0)
        return 1;
    url_len = (int)strlen(url);
    appui_copy_text(page_text,
        "BuzzOS Browser\n\nType an http:// address above and press Enter or Go.\n"
        "This browser renders safe text from HTML. HTTPS, images, CSS, and JavaScript "
        "are not supported yet.", sizeof(page_text));
    text_len = (int)strlen(page_text);
    for (;;) {
        if (guiapp_read_event(&ctx, &event) < 0 || event.type == GUIAPP_EVT_CLOSE)
            break;
        if (event.type == GUIAPP_EVT_INIT || event.type == GUIAPP_EVT_RESIZE) {
            w = clamp_int(event.width, 300, MAX_W);
            h = clamp_int(event.height, 190, MAX_H);
        } else if (event.type == GUIAPP_EVT_KEY) {
            key(event.key);
        } else if (event.type == GUIAPP_EVT_TEXT) {
            text_input(event.text);
        } else if (event.type == GUIAPP_EVT_COMMAND) {
            command(&ctx, event.key);
        } else if (event.type == GUIAPP_EVT_MOUSE) {
            mouse(event.x, event.y, event.buttons, event.wheel);
        }
        render();
        if (guiapp_send_frame(&ctx, title, w, h, pixels) < 0)
            break;
    }
    return 0;
}
