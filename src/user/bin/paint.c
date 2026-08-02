#include "appui.h"
#include "guiapp.h"
#include "libc.h"

enum {
    MAX_W = GUIAPP_MAX_W,
    MAX_H = GUIAPP_MAX_H,
    CX = 20,
    CY = 82,
    CANVAS_MAX_W = GUIAPP_MAX_W - CX * 2,
    CANVAS_MAX_H = GUIAPP_MAX_H - CY - 16,
    FILL_QUEUE = 65536,
};

static uint32_t *pixels;
static size_t pixels_cap;
static uint32_t *canvas;
static int canvas_stride; /* allocated row width */
static int canvas_rows;   /* allocated height */
static uint16_t fill_qx[FILL_QUEUE];
static uint16_t fill_qy[FILL_QUEUE];
static int tool;
static uint32_t color = 0;
static int drawing;
static int sx;
static int sy;
static int lx;
static int ly;
static int w = 560;
static int h = 360;
static int pointer_x = -1;
static int pointer_y = -1;
static int pointer_buttons;

static const uint32_t palette[] = {
    0x000000u, 0xFFFFFFu, 0xCC6600u, 0x003300u, 0x207850u, 0xFF3300u,
    0xCC66FFu, 0xCCCC00u, 0x0033FFu, 0x3366FFu, 0x996600u, 0x9E9E9Eu,
};

static int palette_count(void) {
    return (int)(sizeof(palette) / sizeof(palette[0]));
}

static int toolbar_gap(void) {
    return w < 420 ? 4 : 6;
}

static int toolbar_button_w(void) {
    int gap = toolbar_gap();
    int total = w - 20;
    return appui_max(26, (total - gap * 5) / 6);
}

static struct appui_rect tool_rect(int index) {
    int bw = toolbar_button_w();
    int gap = toolbar_gap();
    return (struct appui_rect){10 + index * (bw + gap), 10, bw, 26};
}

static struct appui_rect clear_rect(void) {
    return tool_rect(5);
}

static struct appui_rect color_rect(int index) {
    int count = palette_count();
    int total = w - 20;
    int gap = w < 420 ? 4 : 6;
    int sw = (total - gap * (count - 1)) / count;
    if (sw < 12) sw = 12;
    if (sw > 24) {
        sw = 24;
        gap = count > 1 ? (total - sw * count) / (count - 1) : 0;
        if (gap < 4) gap = 4;
    }
    return (struct appui_rect){10 + index * (sw + gap), 46, sw, 18};
}

static int canvas_view_w(void) {
    int view_w = appui_max(1, w - CX * 2);
    return view_w > CANVAS_MAX_W ? CANVAS_MAX_W : view_w;
}

static int canvas_view_h(void) {
    int view_h = appui_max(1, h - CY - 16);
    return view_h > CANVAS_MAX_H ? CANVAS_MAX_H : view_h;
}

static void clear_canvas(void) {
    if (!canvas || canvas_stride <= 0 || canvas_rows <= 0)
        return;
    for (int y = 0; y < canvas_rows; y++)
        for (int x = 0; x < canvas_stride; x++)
            canvas[y * canvas_stride + x] = 0xFFFFFFu;
}

static int ensure_paint_buffers(void) {
    int vw = canvas_view_w();
    int vh = canvas_view_h();
    if (appui_pixels_ensure(&pixels, &pixels_cap, w, h, MAX_W, MAX_H) < 0)
        return -1;
    if (canvas && vw <= canvas_stride && vh <= canvas_rows)
        return 0;
    int nw = vw + vw / 4;
    int nh = vh + vh / 4;
    if (nw < vw) nw = vw;
    if (nh < vh) nh = vh;
    if (nw > CANVAS_MAX_W) nw = CANVAS_MAX_W;
    if (nh > CANVAS_MAX_H) nh = CANVAS_MAX_H;
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    uint32_t *nbuf = (uint32_t *)malloc((size_t)nw * (size_t)nh * sizeof(uint32_t));
    if (!nbuf)
        return -1;
    for (int i = 0; i < nw * nh; i++)
        nbuf[i] = 0xFFFFFFu;
    if (canvas) {
        int copy_w = canvas_stride < nw ? canvas_stride : nw;
        int copy_h = canvas_rows < nh ? canvas_rows : nh;
        for (int y = 0; y < copy_h; y++)
            for (int x = 0; x < copy_w; x++)
                nbuf[y * nw + x] = canvas[y * canvas_stride + x];
        free(canvas);
    }
    canvas = nbuf;
    canvas_stride = nw;
    canvas_rows = nh;
    return 0;
}

static void canvas_pixel(int x, int y, int c) {
    if (!canvas || x < 0 || y < 0 || x >= canvas_stride || y >= canvas_rows)
        return;
    canvas[y * canvas_stride + x] = c & 0x00FFFFFFu;
}

static void draw_line_canvas(int x0, int y0, int x1, int y1, int c) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sxv = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int syv = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        canvas_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sxv; }
        if (e2 <= dx) { err += dx; y0 += syv; }
    }
}

static void brush(int x, int y, int c) {
    for (int yy = -3; yy <= 3; yy++)
        for (int xx = -3; xx <= 3; xx++)
            if (xx * xx + yy * yy <= 9)
                canvas_pixel(x + xx, y + yy, c);
}

static void brush_line(int x0, int y0, int x1, int y1, int c) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sxv = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int syv = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        brush(x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sxv; }
        if (e2 <= dx) { err += dx; y0 += syv; }
    }
}

static void rect_canvas(int x0, int y0, int x1, int y1, int c) {
    int x = appui_min(x0, x1);
    int y = appui_min(y0, y1);
    int w = appui_max(x0, x1) - x + 1;
    int h = appui_max(y0, y1) - y + 1;
    for (int i = 0; i < w; i++) {
        canvas_pixel(x + i, y, c);
        canvas_pixel(x + i, y + h - 1, c);
    }
    for (int i = 0; i < h; i++) {
        canvas_pixel(x, y + i, c);
        canvas_pixel(x + w - 1, y + i, c);
    }
}

static void fill_canvas(int x, int y, int c) {
    int fill_w = canvas_view_w();
    int fill_h = canvas_view_h();
    if (!canvas || x < 0 || y < 0 || x >= fill_w || y >= fill_h ||
        x >= canvas_stride || y >= canvas_rows)
        return;
    uint32_t target = canvas[y * canvas_stride + x];
    if (target == (uint32_t)c)
        return;
    int head = 0;
    int tail = 0;
    int count = 0;
    fill_qx[tail] = (uint16_t)x;
    fill_qy[tail] = (uint16_t)y;
    tail = (tail + 1) % FILL_QUEUE;
    count++;
    canvas[y * canvas_stride + x] = c & 0x00FFFFFFu;
    while (count > 0) {
        int px = fill_qx[head];
        int py = fill_qy[head];
        head = (head + 1) % FILL_QUEUE;
        count--;
        const int nx[4] = {px - 1, px + 1, px, px};
        const int ny[4] = {py, py, py - 1, py + 1};
        for (int i = 0; i < 4; i++) {
            int xx = nx[i];
            int yy = ny[i];
            if (xx < 0 || yy < 0 || xx >= fill_w || yy >= fill_h)
                continue;
            if (canvas[yy * canvas_stride + xx] != target)
                continue;
            canvas[yy * canvas_stride + xx] = c & 0x00FFFFFFu;
            if (count < FILL_QUEUE) {
                fill_qx[tail] = (uint16_t)xx;
                fill_qy[tail] = (uint16_t)yy;
                tail = (tail + 1) % FILL_QUEUE;
                count++;
            }
        }
    }
}

/* Tools that have an honest glyph use one; brush and erase have no icon in the
 * set, and a wrong glyph reads worse than the word, so they keep their label. */
static const int tool_icon[5] = {
    -1, -1, UI_ICON_MINUS, UI_ICON_MAXIMIZE, UI_ICON_GRID,
};

static void render(void) {
    appui_fill(pixels, w, h, (struct appui_rect){0, 0, w, h}, UI_BG_SOLID);
    appui_toolbar(pixels, w, h, (struct appui_rect){0, 0, w, 70});
    const char *tools[] = {"Brush", "Erase", "Line", "Rect", "Fill"};
    const char *tools_short[] = {"B", "E", "Line", "Rect", "Fill"};
    int bw = toolbar_button_w();
    for (int i = 0; i < 5; i++) {
        struct appui_rect r = tool_rect(i);
        int state = appui_pointer_state(r, pointer_x, pointer_y, pointer_buttons);
        if (tool == i)
            state |= APPUI_STATE_SELECTED;
        if (tool_icon[i] >= 0)
            appui_icon_button(pixels, w, h, r, tool_icon[i], state);
        else
            appui_button_ex(pixels, w, h, r,
                            bw < 54 ? tools_short[i] : tools[i],
                            APPUI_BTN_DEFAULT, state);
    }
    struct appui_rect clear = clear_rect();
    appui_button_ex(pixels, w, h, clear, bw < 54 ? "Clr" : "Clear",
                    APPUI_BTN_DANGER,
                    appui_pointer_state(clear, pointer_x, pointer_y,
                                        pointer_buttons));
    for (int i = 0; i < palette_count(); i++) {
        struct appui_rect r = color_rect(i);
        int selected = (i == (int)color);
        /* Swatch keeps its own color; selection is a ring around it. */
        appui_fill_round_r(pixels, w, h, r, UI_RADIUS_CONTROL, palette[i]);
        appui_stroke_round(pixels, w, h, r, UI_RADIUS_CONTROL,
                           selected ? UI_ACCENT_FILL : UI_STROKE_CONTROL);
        if (selected)
            appui_stroke_round(pixels, w, h,
                               (struct appui_rect){r.x - 2, r.y - 2, r.w + 4,
                                                   r.h + 4},
                               UI_RADIUS_CONTROL + 2, UI_ACCENT_FILL);
    }

    int view_w = appui_max(1, w - CX * 2);
    int view_h = appui_max(1, h - CY - 16);
    if (view_w > CANVAS_MAX_W) view_w = CANVAS_MAX_W;
    if (view_h > CANVAS_MAX_H) view_h = CANVAS_MAX_H;
    appui_fill(pixels, w, h,
               (struct appui_rect){CX - 1, CY - 1, view_w + 2, view_h + 2},
               UI_STROKE_CONTROL);
    for (int y = 0; y < view_h; y++)
        for (int x = 0; x < view_w; x++)
            pixels[(CY + y) * w + CX + x] = canvas[y * canvas_stride + x];

    if (drawing && (tool == 2 || tool == 3)) {
        int ex = lx;
        int ey = ly;
        if (tool == 2) {
            int dx = ex > sx ? ex - sx : sx - ex;
            int sxv = sx < ex ? 1 : -1;
            int dy = ey > sy ? sy - ey : ey - sy;
            int syv = sy < ey ? 1 : -1;
            int err = dx + dy;
            int x = sx;
            int y = sy;
            for (;;) {
                if (x < view_w && y < view_h)
                    appui_pixel(pixels, w, h, CX + x, CY + y, palette[color]);
                if (x == ex && y == ey) break;
                int e2 = 2 * err;
                if (e2 >= dy) { err += dy; x += sxv; }
                if (e2 <= dx) { err += dx; y += syv; }
            }
        } else {
            int x0 = appui_min(sx, ex), y0 = appui_min(sy, ey);
            int x1 = appui_max(sx, ex), y1 = appui_max(sy, ey);
            appui_border(pixels, w, h, (struct appui_rect){CX + x0, CY + y0, x1 - x0 + 1, y1 - y0 + 1},
                         palette[color], palette[color]);
        }
    }
}

static void render_canvas_view(void) {
    int view_w = canvas_view_w();
    int view_h = canvas_view_h();
    appui_fill(pixels, w, h,
               (struct appui_rect){CX - 1, CY - 1, view_w + 2, view_h + 2},
               UI_STROKE_CONTROL);
    for (int y = 0; y < view_h; y++)
        for (int x = 0; x < view_w; x++)
            pixels[(CY + y) * w + CX + x] = canvas[y * canvas_stride + x];

    if (drawing && (tool == 2 || tool == 3)) {
        int ex = lx;
        int ey = ly;
        if (tool == 2) {
            int dx = ex > sx ? ex - sx : sx - ex;
            int sxv = sx < ex ? 1 : -1;
            int dy = ey > sy ? sy - ey : ey - sy;
            int syv = sy < ey ? 1 : -1;
            int err = dx + dy;
            int x = sx;
            int y = sy;
            for (;;) {
                if (x >= 0 && y >= 0 && x < view_w && y < view_h)
                    appui_pixel(pixels, w, h, CX + x, CY + y, palette[color]);
                if (x == ex && y == ey)
                    break;
                int e2 = 2 * err;
                if (e2 >= dy) { err += dy; x += sxv; }
                if (e2 <= dx) { err += dx; y += syv; }
            }
        } else {
            int x0 = appui_min(sx, ex), y0 = appui_min(sy, ey);
            int x1 = appui_max(sx, ex), y1 = appui_max(sy, ey);
            appui_border(pixels, w, h,
                         (struct appui_rect){CX + x0, CY + y0, x1 - x0 + 1, y1 - y0 + 1},
                         palette[color], palette[color]);
        }
    }
}

static int mouse(int x, int y, int buttons) {
    pointer_x = x;
    pointer_y = y;
    pointer_buttons = buttons;
    int canvas_dirty = 0;
    for (int i = 0; i < 5; i++)
        if ((buttons & 1) && appui_inside(x, y, tool_rect(i))) {
            tool = i;
            return 0;
        }
    if ((buttons & 1) && appui_inside(x, y, clear_rect())) {
        clear_canvas();
        return 1;
    }
    for (int i = 0; i < palette_count(); i++)
        if ((buttons & 1) && appui_inside(x, y, color_rect(i))) {
            color = i;
            return 0;
        }

    int cx = x - CX;
    int cy = y - CY;
    int view_w = canvas_view_w();
    int view_h = canvas_view_h();
    int on_canvas = cx >= 0 && cy >= 0 && cx < view_w && cy < view_h;
    if ((buttons & 1) && on_canvas && !drawing) {
        drawing = 1;
        sx = lx = cx;
        sy = ly = cy;
        if (tool == 0) brush(cx, cy, palette[color]);
        else if (tool == 1) brush(cx, cy, 0xFFFFFFu);
        else if (tool == 4) fill_canvas(cx, cy, palette[color]);
        canvas_dirty = 1;
    } else if ((buttons & 1) && on_canvas && drawing) {
        if (tool == 0) {
            brush_line(lx, ly, cx, cy, palette[color]);
            canvas_dirty = 1;
        } else if (tool == 1) {
            brush_line(lx, ly, cx, cy, 0xFFFFFFu);
            canvas_dirty = 1;
        } else if (tool == 2 || tool == 3) {
            canvas_dirty = 1;
        }
        lx = cx;
        ly = cy;
    } else if (!(buttons & 1) && drawing) {
        if (tool == 2)
            draw_line_canvas(sx, sy, lx, ly, palette[color]);
        else if (tool == 3)
            rect_canvas(sx, sy, lx, ly, palette[color]);
        drawing = 0;
        canvas_dirty = 1;
    }
    return canvas_dirty;
}

int main(int argc, char **argv) {
    struct guiapp_ctx ctx;
    struct guiapp_event ev;
    if (guiapp_parse_args(argc, argv, &ctx) < 0)
        return 1;
    if (ensure_paint_buffers() < 0) return 1;
    clear_canvas();
    for (;;) {
        if (guiapp_read_event(&ctx, &ev) < 0 || ev.type == GUIAPP_EVT_CLOSE)
            break;
        int dirty_canvas = 0;
        if (ev.type == GUIAPP_EVT_MOUSE)
            dirty_canvas = mouse(ev.x, ev.y, ev.buttons);
        else if (ev.type == GUIAPP_EVT_INIT || ev.type == GUIAPP_EVT_RESIZE) {
            w = ev.width;
            h = ev.height;
            if (w < 220) w = 220;
            if (h < 160) h = 160;
            if (w > MAX_W) w = MAX_W;
            if (h > MAX_H) h = MAX_H;
            if (ensure_paint_buffers() < 0)
                break;
        }
        if (ensure_paint_buffers() < 0)
            break;
        if (dirty_canvas) {
            render_canvas_view();
            if (guiapp_send_dirty(&ctx, "Paint", w, h, CX - 1, CY - 1,
                                  canvas_view_w() + 2, canvas_view_h() + 2,
                                  pixels, w) < 0)
                break;
        } else {
            render();
            if (guiapp_send_frame(&ctx, "Paint", w, h, pixels) < 0)
                break;
        }
    }
    free(pixels);
    free(canvas);
    return 0;
}
