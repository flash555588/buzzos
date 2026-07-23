#include <stddef.h>
#include <stdint.h>
#include "console.h"
#include "fb.h"
#include "font_builtin.h"
#include "paging.h"
#include "pmm.h"
#include "surface.h"

enum {
    CONSOLE_FONT_W = KFONT_WIDTH,
    CONSOLE_FONT_H = KFONT_HEIGHT,
    CONSOLE_MAX_COLS = 220,
    CONSOLE_MAX_ROWS = 120,
};

struct console_cell {
    char character;
    uint8_t color;
};

struct dirty_rect {
    int x;
    int y;
    int width;
    int height;
    int valid;
};

static struct console_cell cells[CONSOLE_MAX_ROWS][CONSOLE_MAX_COLS];
static struct gfx_surface surface;
static uint16_t console_cols;
static uint16_t console_rows;
static uint16_t first_row;
static uint16_t cursor_row;
static uint16_t cursor_col;
static uint8_t console_color = 0x0F;
static uint8_t ansi_state;
static int ansi_params[2];
static int ansi_param_index;
static int ansi_seen_digit;
static int console_ready;
static struct dirty_rect dirty;

static uint32_t blend_rgb(uint32_t foreground, uint32_t background,
                          uint32_t alpha) {
    uint32_t inverse = 255u - alpha;
    uint32_t rb = (((foreground & 0x00FF00FFu) * alpha +
                    (background & 0x00FF00FFu) * inverse) >> 8) &
                  0x00FF00FFu;
    uint32_t g = (((foreground & 0x0000FF00u) * alpha +
                   (background & 0x0000FF00u) * inverse) >> 8) &
                 0x0000FF00u;
    return rb | g;
}

static struct console_cell *cell_at(uint16_t row, uint16_t col) {
    uint16_t physical_row =
        (uint16_t)((first_row + row) % console_rows);
    return &cells[physical_row][col];
}

static void mark_dirty(int x, int y, int width, int height) {
    if (!console_ready || width <= 0 || height <= 0)
        return;
    if (x < 0) { width += x; x = 0; }
    if (y < 0) { height += y; y = 0; }
    if (x >= (int)surface.width || y >= (int)surface.height)
        return;
    if (width > (int)surface.width - x)
        width = (int)surface.width - x;
    if (height > (int)surface.height - y)
        height = (int)surface.height - y;
    if (width <= 0 || height <= 0)
        return;

    if (!dirty.valid) {
        dirty.x = x;
        dirty.y = y;
        dirty.width = width;
        dirty.height = height;
        dirty.valid = 1;
        return;
    }
    int left = x < dirty.x ? x : dirty.x;
    int top = y < dirty.y ? y : dirty.y;
    int right_a = x + width;
    int right_b = dirty.x + dirty.width;
    int bottom_a = y + height;
    int bottom_b = dirty.y + dirty.height;
    int right = right_a > right_b ? right_a : right_b;
    int bottom = bottom_a > bottom_b ? bottom_a : bottom_b;
    dirty.x = left;
    dirty.y = top;
    dirty.width = right - left;
    dirty.height = bottom - top;
}

static void mark_full_dirty(void) {
    mark_dirty(0, 0, (int)surface.width, (int)surface.height);
}

static void present_dirty(void) {
    if (!console_ready || !dirty.valid || !fb_display_console_active())
        return;
    const uint32_t *source = surface.pixels +
        (uint32_t)dirty.y * surface.stride + (uint32_t)dirty.x;
    if (fb_present_rgb32(dirty.x, dirty.y, dirty.width, dirty.height,
                         source, (int)surface.stride) == 0)
        dirty.valid = 0;
}

static const uint8_t *font_alpha_for(char character) {
    unsigned char ch = (unsigned char)character;
    if (ch < KFONT_FIRST || ch >= KFONT_FIRST + KFONT_COUNT)
        ch = '?';
    return &kfont_alpha[ch - KFONT_FIRST][0][0];
}

static void draw_glyph(int x, int y, char character,
                       uint8_t foreground, uint8_t background) {
    const uint8_t *alpha = font_alpha_for(character);
    uint32_t fg_rgb = fb_palette_rgb(foreground);
    uint32_t bg_rgb = fb_palette_rgb(background);
    for (int py = 0; py < CONSOLE_FONT_H; py++) {
        for (int px = 0; px < CONSOLE_FONT_W; px++) {
            uint8_t a = alpha[py * CONSOLE_FONT_W + px];
            uint32_t rgb = a == 0 ? bg_rgb :
                (a == 255 ? fg_rgb : blend_rgb(fg_rgb, bg_rgb, a));
            surface_putpixel(&surface, x + px, y + py, rgb);
        }
    }
}

static void draw_cell(uint16_t row, uint16_t col) {
    if (!console_ready || row >= console_rows || col >= console_cols)
        return;
    struct console_cell *cell = cell_at(row, col);
    int x = (int)col * CONSOLE_FONT_W;
    int y = (int)row * CONSOLE_FONT_H;
    uint8_t foreground = cell->color & 0x0Fu;
    uint8_t background = cell->color >> 4;
    surface_fill_rect(&surface, x, y, CONSOLE_FONT_W, CONSOLE_FONT_H,
                      fb_palette_rgb(background));
    draw_glyph(x, y, cell->character, foreground, background);
    mark_dirty(x, y, CONSOLE_FONT_W, CONSOLE_FONT_H);
}

static void sanitize_cursor(void) {
    if (console_cols == 0 || console_cols > CONSOLE_MAX_COLS)
        console_cols = 1;
    if (console_rows == 0 || console_rows > CONSOLE_MAX_ROWS)
        console_rows = 1;
    if (cursor_col >= console_cols)
        cursor_col = console_cols - 1;
    if (cursor_row >= console_rows)
        cursor_row = console_rows - 1;
}

static void ansi_reset(void) {
    ansi_state = 0;
    ansi_params[0] = 0;
    ansi_params[1] = 0;
    ansi_param_index = 0;
    ansi_seen_digit = 0;
}

static void clear_internal(void) {
    if (!console_ready)
        return;
    first_row = 0;
    for (uint16_t row = 0; row < console_rows; row++) {
        for (uint16_t col = 0; col < console_cols; col++) {
            cells[row][col].character = ' ';
            cells[row][col].color = console_color;
        }
    }
    cursor_row = 0;
    cursor_col = 0;
    surface_clear(&surface, fb_palette_rgb(console_color >> 4));
    mark_full_dirty();
}

static int ansi_value_or_default(int value, int fallback) {
    return value > 0 ? value : fallback;
}

static void set_cursor(int row, int col) {
    sanitize_cursor();
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (row >= console_rows) row = console_rows - 1;
    if (col >= console_cols) col = console_cols - 1;
    cursor_row = (uint16_t)row;
    cursor_col = (uint16_t)col;
}

static void clear_line_from_cursor(void) {
    sanitize_cursor();
    for (uint16_t col = cursor_col; col < console_cols; col++) {
        struct console_cell *cell = cell_at(cursor_row, col);
        cell->character = ' ';
        cell->color = console_color;
        draw_cell(cursor_row, col);
    }
}

static int handle_ansi(char character) {
    if (ansi_state == 0) {
        if ((unsigned char)character == 0x1Bu) {
            ansi_state = 1;
            return 1;
        }
        return 0;
    }
    if (ansi_state == 1) {
        if (character == '[') {
            ansi_state = 2;
            ansi_params[0] = 0;
            ansi_params[1] = 0;
            ansi_param_index = 0;
            ansi_seen_digit = 0;
        } else {
            ansi_reset();
        }
        return 1;
    }
    if (character >= '0' && character <= '9') {
        if (ansi_param_index < 2)
            ansi_params[ansi_param_index] =
                ansi_params[ansi_param_index] * 10 + (character - '0');
        ansi_seen_digit = 1;
        return 1;
    }
    if (character == ';') {
        if (ansi_param_index < 1)
            ansi_param_index++;
        ansi_seen_digit = 0;
        return 1;
    }

    int amount = ansi_value_or_default(ansi_params[0], 1);
    switch (character) {
    case 'A': set_cursor((int)cursor_row - amount, cursor_col); break;
    case 'B': set_cursor((int)cursor_row + amount, cursor_col); break;
    case 'C': set_cursor(cursor_row, (int)cursor_col + amount); break;
    case 'D': set_cursor(cursor_row, (int)cursor_col - amount); break;
    case 'H':
    case 'f':
        set_cursor(ansi_value_or_default(ansi_params[0], 1) - 1,
                   ansi_value_or_default(ansi_params[1], 1) - 1);
        break;
    case 'J':
        if (ansi_params[0] == 2 || !ansi_seen_digit)
            clear_internal();
        break;
    case 'K':
        clear_line_from_cursor();
        break;
    default:
        break;
    }
    ansi_reset();
    return 1;
}

static void newline(void) {
    sanitize_cursor();
    cursor_col = 0;
    if (++cursor_row < console_rows)
        return;

    first_row = (uint16_t)((first_row + 1u) % console_rows);
    cursor_row = console_rows - 1;
    for (uint16_t col = 0; col < console_cols; col++) {
        struct console_cell *cell = cell_at(cursor_row, col);
        cell->character = ' ';
        cell->color = console_color;
    }
    surface_scroll_up(&surface, CONSOLE_FONT_H,
                      fb_palette_rgb(console_color >> 4));
    mark_full_dirty();
}

static void consume_character(char character) {
    sanitize_cursor();
    if (handle_ansi(character))
        return;
    if (character == '\n') {
        newline();
        return;
    }
    if (character == '\r') {
        cursor_col = 0;
        return;
    }
    if (character == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
        } else if (cursor_row > 0) {
            cursor_row--;
            cursor_col = console_cols - 1;
        } else {
            return;
        }
        struct console_cell *cell = cell_at(cursor_row, cursor_col);
        cell->character = ' ';
        cell->color = console_color;
        draw_cell(cursor_row, cursor_col);
        return;
    }
    if ((unsigned char)character < 32u)
        return;

    struct console_cell *cell = cell_at(cursor_row, cursor_col);
    cell->character = character;
    cell->color = console_color;
    draw_cell(cursor_row, cursor_col);
    if (++cursor_col >= console_cols)
        newline();
}

int console_init(void) {
    struct gfx_info info;
    fb_get_info(&info);
    if (!info.width || !info.height)
        return -1;
    uint64_t pixel_count = (uint64_t)info.width * info.height;
    uint64_t bytes = pixel_count * sizeof(uint32_t);
    if (!pixel_count || bytes > KERNEL_FB_SIZE)
        return -1;
    size_t pages = ((size_t)bytes + PAGE_SIZE - 1u) / PAGE_SIZE;
    uintptr_t address = pmm_alloc_pages(pages);
    if (!address)
        return -1;

    surface_init(&surface, (uint32_t *)address, info.width, info.height,
                 info.width);
    console_cols = (uint16_t)(info.width / CONSOLE_FONT_W);
    console_rows = (uint16_t)(info.height / CONSOLE_FONT_H);
    if (console_cols > CONSOLE_MAX_COLS)
        console_cols = CONSOLE_MAX_COLS;
    if (console_rows > CONSOLE_MAX_ROWS)
        console_rows = CONSOLE_MAX_ROWS;
    if (!console_cols) console_cols = 1;
    if (!console_rows) console_rows = 1;
    console_ready = 1;
    dirty.valid = 0;
    ansi_reset();
    clear_internal();
    present_dirty();
    return 0;
}

void console_set_color(uint8_t foreground, uint8_t background) {
    console_color = (uint8_t)((background << 4) | (foreground & 0x0Fu));
}

void console_clear(void) {
    clear_internal();
    present_dirty();
}

void console_write(const char *text, size_t count) {
    if (!console_ready || !text || count == 0)
        return;
    for (size_t i = 0; i < count; i++)
        consume_character(text[i]);
    present_dirty();
}

void console_putc(char character) {
    console_write(&character, 1);
}

void console_puts(const char *text) {
    size_t count = 0;
    while (text && text[count])
        count++;
    console_write(text, count);
}

void console_backspace(void) {
    char backspace = '\b';
    console_write(&backspace, 1);
}

void console_activate(int present_now) {
    if (!console_ready)
        return;
    mark_full_dirty();
    if (present_now)
        present_dirty();
}
