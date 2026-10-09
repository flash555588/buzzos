#include "appui.h"
#include "guiapp.h"
#include "libc.h"

enum { EXPR_CAP = 96, SHORT_CANVAS_H = 520 };

static int W = 360, H = 360;
static uint32_t *pixels;
static size_t pixel_capacity;
static char expr[EXPR_CAP];
static char display[EXPR_CAP];
static int expr_len;
static int parse_error;
static int prev_buttons;
static int pointer_x = -1;
static int pointer_y = -1;
static int pointer_buttons;
static const char *parse_p;

static void set_display(const char *s) {
    appui_copy_text(display, s, sizeof(display));
}

static void append_char(char ch) {
    if (expr_len + 1 >= EXPR_CAP)
        return;
    expr[expr_len++] = ch;
    expr[expr_len] = 0;
    set_display(expr);
}

static void clear(void) {
    expr_len = 0;
    expr[0] = 0;
    set_display("0");
}

static void backspace(void) {
    if (expr_len > 0) {
        expr[--expr_len] = 0;
        set_display(expr_len ? expr : "0");
    }
}

static void skip_spaces(void) {
    while (*parse_p == ' ')
        parse_p++;
}

static double parse_expr(void);

static double parse_number(void) {
    skip_spaces();
    int neg = 0;
    if (*parse_p == '-') {
        neg = 1;
        parse_p++;
    }
    double v = 0.0;
    int any = 0;
    while (*parse_p >= '0' && *parse_p <= '9') {
        v = v * 10.0 + (double)(*parse_p - '0');
        parse_p++;
        any = 1;
    }
    if (*parse_p == '.') {
        double place = 0.1;
        parse_p++;
        while (*parse_p >= '0' && *parse_p <= '9') {
            v += place * (double)(*parse_p - '0');
            place *= 0.1;
            parse_p++;
            any = 1;
        }
    }
    if (!any)
        parse_error = 1;
    return neg ? -v : v;
}

static int starts_with(const char *s) {
    for (int i = 0; s[i]; i++)
        if (parse_p[i] != s[i])
            return 0;
    return 1;
}

static double parse_factor(void) {
    skip_spaces();
    double v;
    if (*parse_p == '(') {
        parse_p++;
        v = parse_expr();
        skip_spaces();
        if (*parse_p == ')')
            parse_p++;
        else
            parse_error = 1;
    } else if (starts_with("sqrt")) {
        parse_p += 4;
        skip_spaces();
        if (*parse_p == '(') {
            parse_p++;
            v = parse_expr();
            skip_spaces();
            if (*parse_p == ')')
                parse_p++;
            else
                parse_error = 1;
        } else {
            v = parse_factor();
        }
        v = sqrt(v);
    } else {
        v = parse_number();
    }
    skip_spaces();
    if (*parse_p == '%') {
        v /= 100.0;
        parse_p++;
    }
    return v;
}

static double parse_term(void) {
    double v = parse_factor();
    for (;;) {
        skip_spaces();
        if (*parse_p == '*') {
            parse_p++;
            v *= parse_factor();
        } else if (*parse_p == '/') {
            parse_p++;
            double rhs = parse_factor();
            if (rhs == 0.0)
                parse_error = 1;
            else
                v /= rhs;
        } else {
            return v;
        }
    }
}

static double parse_expr(void) {
    double v = parse_term();
    for (;;) {
        skip_spaces();
        if (*parse_p == '+') {
            parse_p++;
            v += parse_term();
        } else if (*parse_p == '-') {
            parse_p++;
            v -= parse_term();
        } else {
            return v;
        }
    }
}

static void format_result(double v, char *out, int cap) {
    if (cap <= 0)
        return;
    out[0] = 0;
    if (v < 0.0) {
        appui_append_text(out, "-", cap);
        v = -v;
    }
    int whole = (int)v;
    appui_append_int(out, whole, cap);
    double frac = v - (double)whole;
    if (frac < 0.000001)
        return;
    appui_append_text(out, ".", cap);
    for (int i = 0; i < 6; i++) {
        frac *= 10.0;
        int digit = (int)frac;
        char s[2] = {(char)('0' + digit), 0};
        appui_append_text(out, s, cap);
        frac -= (double)digit;
    }
}

static void evaluate(void) {
    char out[EXPR_CAP];
    parse_error = 0;
    parse_p = expr;
    double v = parse_expr();
    skip_spaces();
    if (*parse_p)
        parse_error = 1;
    if (parse_error) {
        set_display("Error");
        return;
    }
    format_result(v, out, sizeof(out));
    appui_copy_text(expr, out, sizeof(expr));
    expr_len = (int)strlen(expr);
    set_display(expr);
}

static const char *labels[] = {
    "C", "Back", "(", ")",
    "7", "8", "9", "/",
    "4", "5", "6", "*",
    "1", "2", "3", "-",
    "0", ".", "%", "+",
    "sqrt", "=",
};

static int layout_pad(void) {
    return H < SHORT_CANVAS_H ? 12 : appui_max(8, appui_min(20, W / 24));
}

static int layout_readout(void) {
    return H < SHORT_CANVAS_H ? appui_max(48, appui_min(72, H / 6))
                   : appui_max(32, appui_min(120, H / 4));
}

static struct appui_rect button_rect(int i) {
    int pad = layout_pad();
    int readout = layout_readout();
    int top = pad + readout + 8;
    int step_y = appui_max(2, (H - top - pad - (H < SHORT_CANVAS_H ? 0 : 22)) / 6);
    int gap = appui_min(8, step_y / 6);
    int columns = i < 20 ? 4 : 2;
    int step_x = (W - pad * 2 + gap) / columns;
    int column = i < 20 ? i % 4 : i - 20;
    int row = i < 20 ? i / 4 : 5;
    return (struct appui_rect){pad + column * step_x, top + row * step_y,
                               appui_max(1, step_x - gap),
                               appui_max(1, step_y - gap)};
}

static void press_label(const char *s) {
    if (strcmp(s, "C") == 0)
        clear();
    else if (strcmp(s, "Back") == 0)
        backspace();
    else if (strcmp(s, "=") == 0)
        evaluate();
    else if (strcmp(s, "sqrt") == 0) {
        appui_append_text(expr, "sqrt(", sizeof(expr));
        expr_len = (int)strlen(expr);
        set_display(expr);
    } else {
        append_char(s[0]);
    }
}

static void render(void) {
    appui_fill(pixels, W, H, (struct appui_rect){0, 0, W, H}, THEME_APP_BG);
    int pad = layout_pad();
    struct appui_rect display_rect = {pad, pad, W - pad * 2,
                                       layout_readout()};
    appui_card(pixels, W, H, display_rect);
    int label_h = display_rect.h >= 64 ? 20 : 0;
    if (label_h) {
        appui_label(pixels, W, H,
                    (struct appui_rect){pad + 12, pad + 4, display_rect.w - 24, 20},
                    "Standard", UI_FONT_CAPTION, UI_TEXT_TERTIARY, UI_ALIGN_LEFT);
    }
    const char *value = display[0] ? display : "0";
    struct appui_rect value_rect = {display_rect.x + 12,
        display_rect.y + label_h + 4, display_rect.w - 24,
        display_rect.h - label_h - 8};
    int value_font = UI_FONT_DISPLAY;
    while (value_font > UI_FONT_CAPTION &&
           (ui_font_height(value_font) > value_rect.h ||
            ui_text_width(value, value_font) > value_rect.w))
        value_font--;
    /* The result is the one piece of type that should dominate the window,
     * so it gets the title size and hugs the right edge like a calculator
     * readout rather than sitting on the native glyph grid. */
    appui_label(pixels, W, H,
                value_rect, value, value_font, THEME_FIELD_TEXT, UI_ALIGN_RIGHT);
    for (int i = 0; i < (int)(sizeof(labels) / sizeof(labels[0])); i++) {
        struct appui_rect r = button_rect(i);
        int variant = APPUI_BTN_DEFAULT;
        if (strcmp(labels[i], "=") == 0)
            variant = APPUI_BTN_PRIMARY;
        else if (strcmp(labels[i], "C") == 0)
            variant = APPUI_BTN_DANGER;
        appui_button_ex(pixels, W, H, r, labels[i], variant,
                        appui_pointer_state(r, pointer_x, pointer_y,
                                            pointer_buttons));
    }
    if (H >= SHORT_CANVAS_H)
        appui_label(pixels, W, H, (struct appui_rect){pad, H - 22, W - pad * 2, 20},
                "Enter to calculate  /  Backspace to undo", UI_FONT_CAPTION,
                UI_TEXT_TERTIARY, UI_ALIGN_CENTER);
}

static void mouse(int x, int y, int buttons) {
    pointer_x = x;
    pointer_y = y;
    pointer_buttons = buttons;
    int pressed = (buttons & 1) && !(prev_buttons & 1);
    prev_buttons = buttons;
    if (!pressed)
        return;
    for (int i = 0; i < (int)(sizeof(labels) / sizeof(labels[0])); i++) {
        if (appui_inside(x, y, button_rect(i))) {
            press_label(labels[i]);
            return;
        }
    }
}

static void key(int k) {
    if ((k >= '0' && k <= '9') || k == '+' || k == '-' ||
        k == '*' || k == '/' || k == '.' || k == '(' || k == ')' || k == '%')
        append_char((char)k);
    else if (k == GUIAPP_KEY_BACKSPACE || k == 127)
        backspace();
    else if (k == '\n' || k == '\r' || k == '=')
        evaluate();
    else if (k == 'c' || k == 'C')
        clear();
    else if (k == 's' || k == 'S')
        press_label("sqrt");
}

static void text_input(const char *value) {
    while (value && *value) {
        unsigned char ch = (unsigned char)*value++;
        if (ch < 0x80u)
            key(ch);
    }
}

static void command(struct guiapp_ctx *ctx, int value) {
    if (value != GUIAPP_CMD_COPY && value != GUIAPP_CMD_CUT)
        return;
    (void)guiapp_set_clipboard(ctx, display);
    if (value == GUIAPP_CMD_CUT)
        clear();
}

int main(int argc, char **argv) {
    struct guiapp_ctx ctx;
    struct guiapp_event ev;
    if (guiapp_parse_args(argc, argv, &ctx) < 0)
        return 1;
    clear();
    for (;;) {
        if (guiapp_read_event(&ctx, &ev) < 0 || ev.type == GUIAPP_EVT_CLOSE)
            break;
        if (ev.type == GUIAPP_EVT_INIT || ev.type == GUIAPP_EVT_RESIZE) {
            W = appui_max(1, appui_min(ev.width, GUIAPP_MAX_W));
            H = appui_max(1, appui_min(ev.height, GUIAPP_MAX_H));
        }
        if (appui_pixels_ensure(&pixels, &pixel_capacity, W, H,
                                GUIAPP_MAX_W, GUIAPP_MAX_H) < 0)
            break;
        if (ev.type == GUIAPP_EVT_MOUSE)
            mouse(ev.x, ev.y, ev.buttons);
        else if (ev.type == GUIAPP_EVT_KEY && ev.buttons)
            key(ev.key);
        else if (ev.type == GUIAPP_EVT_TEXT)
            text_input(ev.text);
        else if (ev.type == GUIAPP_EVT_COMMAND)
            command(&ctx, ev.key);
        render();
        if (guiapp_send_frame(&ctx, "Calculator", W, H, pixels) < 0)
            break;
    }
    free(pixels);
    return 0;
}
