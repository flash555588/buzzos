#include "io.h"
#include "fb.h"
#include "mouse.h"
#include "gui_event.h"
#include "irq.h"

enum {
    PS2_DATA = 0x60,
    PS2_STATUS = 0x64,
    PS2_CMD = 0x64,
    PIC1_DATA = 0x21,
    PIC2_DATA = 0xA1,
};

static volatile int mouse_x;
static volatile int mouse_y;
static volatile int mouse_buttons;
static volatile int mouse_dx;
static volatile int mouse_dy;
static volatile int mouse_wheel_total;
static volatile uint32_t mouse_wheel_seq;
static volatile uint32_t mouse_seq;
static volatile int mouse_enabled;
static volatile int mouse_absolute_mode;

static uint8_t packet[4];
static int packet_index;
static int packet_size;

static void mouse_screen_bounds(int *w_out, int *h_out) {
    struct gfx_info info;
    fb_get_info(&info);
    if (info.width == 0)
        info.width = 1;
    if (info.height == 0)
        info.height = 1;
    if (w_out)
        *w_out = (int)info.width;
    if (h_out)
        *h_out = (int)info.height;
}

static int ps2_wait_write(void) {
    for (int i = 0; i < 100000; i++) {
        if ((inb(PS2_STATUS) & 0x02) == 0)
            return 0;
    }
    return -1;
}

static int ps2_wait_read(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS) & 0x01)
            return 0;
    }
    return -1;
}

static void ps2_flush_output(void) {
    for (int i = 0; i < 64 && (inb(PS2_STATUS) & 0x01); i++)
        (void)inb(PS2_DATA);
}

static int ps2_read_cmd_byte(uint8_t *cmd) {
    if (ps2_wait_write() < 0)
        return -1;
    outb(PS2_CMD, 0x20);
    if (ps2_wait_read() < 0)
        return -1;
    *cmd = inb(PS2_DATA);
    return 0;
}

static int ps2_write_cmd_byte(uint8_t cmd) {
    if (ps2_wait_write() < 0)
        return -1;
    outb(PS2_CMD, 0x60);
    if (ps2_wait_write() < 0)
        return -1;
    outb(PS2_DATA, cmd);
    return 0;
}

static int mouse_send(uint8_t cmd) {
    for (int attempt = 0; attempt < 3; attempt++) {
        if (ps2_wait_write() < 0)
            return -1;
        outb(PS2_CMD, 0xD4);
        if (ps2_wait_write() < 0)
            return -1;
        outb(PS2_DATA, cmd);
        for (int i = 0; i < 100000; i++) {
            uint8_t status = inb(PS2_STATUS);
            if ((status & 0x01) == 0)
                continue;
            uint8_t data = inb(PS2_DATA);
            if ((status & 0x20) == 0)
                continue;
            if (data == 0xFA)
                return 0;
            if (data == 0xFE)
                break;
            return -1;
        }
    }
    return -1;
}

static int mouse_read_response(uint8_t *out) {
    for (int i = 0; i < 300000; i++) {
        uint8_t status = inb(PS2_STATUS);
        if ((status & 0x01) == 0)
            continue;
        uint8_t data = inb(PS2_DATA);
        if ((status & 0x20) == 0)
            continue;
        if (data == 0xFA || data == 0xFE)
            continue;
        *out = data;
        return 0;
    }
    return -1;
}

static int mouse_set_sample_rate(uint8_t rate) {
    if (mouse_send(0xF3) < 0)
        return -1;
    return mouse_send(rate);
}

static int mouse_probe_wheel_sequence(uint8_t second_rate, uint8_t *id) {
    if (mouse_set_sample_rate(200) < 0 ||
        mouse_set_sample_rate(second_rate) < 0 ||
        mouse_set_sample_rate(80) < 0)
        return -1;
    if (mouse_send(0xF2) < 0)
        return -1;
    return mouse_read_response(id);
}

static void mouse_try_wheel_mode(void) {
    uint8_t id = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (mouse_probe_wheel_sequence(100, &id) == 0 && (id == 3 || id == 4)) {
            packet_size = 4;
            return;
        }
        if (mouse_probe_wheel_sequence(200, &id) == 0 && (id == 3 || id == 4)) {
            packet_size = 4;
            return;
        }
    }
}

static void disable_mouse_port(void) {
    if (ps2_wait_write() == 0)
        outb(PS2_CMD, 0xA7);
}

static void enable_keyboard_port(void) {
    if (ps2_wait_write() == 0)
        outb(PS2_CMD, 0xAE);
}

static void unmask_irq12(void) {
    uint8_t master = inb(PIC1_DATA);
    uint8_t slave = inb(PIC2_DATA);
    master &= (uint8_t)~0x04;  /* IRQ2 cascade */
    slave &= (uint8_t)~0x10;   /* IRQ12 PS/2 mouse */
    outb(PIC1_DATA, master);
    outb(PIC2_DATA, slave);
}

void mouse_init(void) {
    int screen_w = 1;
    int screen_h = 1;
    mouse_screen_bounds(&screen_w, &screen_h);
    mouse_x = screen_w / 2;
    mouse_y = screen_h / 2;
    mouse_buttons = 0;
    mouse_dx = 0;
    mouse_dy = 0;
    mouse_wheel_total = 0;
    mouse_wheel_seq = 0;
    mouse_seq = 0;
    mouse_enabled = 0;
    mouse_absolute_mode = 0;
    packet_index = 0;
    packet_size = 3;

    uint32_t flags = irq_save();
    uint8_t old_cmd = 0;
    int have_cmd = 0;
    ps2_flush_output();
    if (ps2_wait_write() == 0)
        outb(PS2_CMD, 0xAD);   /* keep keyboard bytes out of 0x60 while programming */
    disable_mouse_port();
    ps2_flush_output();

    if (ps2_read_cmd_byte(&old_cmd) == 0)
        have_cmd = 1;

    if (ps2_wait_write() == 0)
        outb(PS2_CMD, 0xA8);   /* enable auxiliary PS/2 device */

    int ok = 1;
    if (have_cmd) {
        uint8_t cmd = old_cmd;
        cmd |= 0x03;           /* IRQ1 keyboard and IRQ12 mouse */
        cmd &= (uint8_t)~0x30;  /* enable keyboard and mouse clocks */
        if (ps2_write_cmd_byte(cmd) < 0)
            ok = 0;
    } else {
        ok = 0;
    }

    ps2_flush_output();
    (void)mouse_send(0xF5);     /* stop streaming while resetting */
    if (mouse_send(0xF6) < 0)   /* defaults */
        ok = 0;
    if (ok)
        mouse_try_wheel_mode();
    if (mouse_send(0xF4) < 0)   /* enable data reporting */
        ok = 0;

    if (ok) {
        mouse_enabled = 1;
        unmask_irq12();
    } else {
        disable_mouse_port();
        if (have_cmd) {
            uint8_t cmd = old_cmd;
            cmd |= 0x01;        /* keep keyboard IRQ enabled */
            cmd &= (uint8_t)~0x10;
            cmd &= (uint8_t)~0x02;
            cmd |= 0x20;        /* disable mouse clock */
            (void)ps2_write_cmd_byte(cmd);
        }
    }
    enable_keyboard_port();
    irq_restore(flags);
}

void mouse_handler(uint8_t byte) {
    if (!mouse_enabled || mouse_absolute_mode)
        return;
    if (packet_index == 0 && (byte & 0x08) == 0)
        return;

    packet[packet_index++] = byte;
    if (packet_index < packet_size)
        return;
    packet_index = 0;

    int buttons = packet[0] & 0x07;
    int dx = (int)(int8_t)packet[1];
    int dy = (int)(int8_t)packet[2];
    int wheel = 0;

    if (packet[0] & 0xC0) {
        dx = 0;
        dy = 0;
    }
    if (packet_size == 4) {
        wheel = packet[3] & 0x0F;
        if (wheel & 0x08)
            wheel -= 0x10;
    }

    int x = mouse_x + dx;
    int y = mouse_y - dy;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    {
        int screen_w = 1;
        int screen_h = 1;
        mouse_screen_bounds(&screen_w, &screen_h);
        if (x >= screen_w) x = screen_w - 1;
        if (y >= screen_h) y = screen_h - 1;
    }

    mouse_x = x;
    mouse_y = y;
    mouse_buttons = buttons;
    mouse_dx = dx;
    mouse_dy = -dy;
    if (wheel != 0) {
        mouse_wheel_total += wheel;
        mouse_wheel_seq++;
    }
    mouse_seq++;
    gui_event_notify_display();
}

void mouse_set_absolute_mode(int enabled) {
    uint32_t flags = irq_save();
    mouse_absolute_mode = enabled != 0;
    mouse_dx = 0;
    mouse_dy = 0;
    packet_index = 0;
    irq_restore(flags);
}

void mouse_absolute_event(int x, int y, int buttons, int wheel) {
    int screen_w = 1;
    int screen_h = 1;
    mouse_screen_bounds(&screen_w, &screen_h);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= screen_w) x = screen_w - 1;
    if (y >= screen_h) y = screen_h - 1;

    mouse_dx = x - mouse_x;
    mouse_dy = y - mouse_y;
    mouse_x = x;
    mouse_y = y;
    mouse_buttons = buttons & 0x07;
    if (wheel != 0) {
        mouse_wheel_total += wheel;
        mouse_wheel_seq++;
    }
    mouse_seq++;
    gui_event_notify_display();
}

void mouse_get_state(struct mouse_state *out) {
    if (!out)
        return;
    out->x = mouse_x;
    out->y = mouse_y;
    out->buttons = mouse_buttons;
    out->dx = mouse_dx;
    out->dy = mouse_dy;
    out->seq = mouse_seq;
    out->wheel = mouse_wheel_total;
    out->wheel_seq = mouse_wheel_seq;
}

void mouse_clamp_to_screen(void) {
    uint32_t flags = irq_save();
    int screen_w = 1;
    int screen_h = 1;
    mouse_screen_bounds(&screen_w, &screen_h);
    if (mouse_x >= screen_w) mouse_x = screen_w - 1;
    if (mouse_y >= screen_h) mouse_y = screen_h - 1;
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    mouse_seq++;
    irq_restore(flags);
}
