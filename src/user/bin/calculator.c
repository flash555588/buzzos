#include "appui.h"
#include "guiapp.h"
#include "libc.h"

enum { W = 360, H = 360, EXPR_CAP = 96 };

static uint8_t pixels[W * H];
static char expr[EXPR_CAP];
static char display[EXPR_CAP];
static int expr_len;
static int parse_error;
static int prev_buttons;
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

static struct appui_rect button_rect(int i) {
    if (i < 20)
        return (struct appui_rect){18 + (i % 4) * 82, 96 + (i / 4) * 42, 72, 34};
    return (struct appui_rect){18 + (i - 20) * 164, 316, 154, 34};
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
    appui_fill(pixels, W, H, (struct appui_rect){0, 0, W, H}, appui_gray(3));
    appui_fill(pixels, W, H, (struct appui_rect){16, 18, W - 32, 72}, 15);
    appui_border(pixels, W, H, (struct appui_rect){16, 18, W - 32, 72}, appui_gray(8), appui_gray(1));
    appui_text(pixels, W, H, 28, 43, display[0] ? display : "0", 0, -1,
               (struct appui_rect){26, 26, W - 52, 52});
    for (int i = 0; i < (int)(sizeof(labels) / sizeof(labels[0])); i++)
        appui_button(pixels, W, H, button_rect(i), labels[i], strcmp(labels[i], "=") == 0);
}

static void mouse(int x, int y, int buttons) {
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
        if (ev.type == GUIAPP_EVT_MOUSE)
            mouse(ev.x, ev.y, ev.buttons);
        else if (ev.type == GUIAPP_EVT_KEY)
            key(ev.key);
        else if (ev.type == GUIAPP_EVT_TEXT)
            text_input(ev.text);
        else if (ev.type == GUIAPP_EVT_COMMAND)
            command(&ctx, ev.key);
        render();
        if (guiapp_send_frame(&ctx, "Calculator", W, H, pixels) < 0)
            break;
    }
    return 0;
}
