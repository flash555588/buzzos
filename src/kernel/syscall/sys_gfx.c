#include "syscall_internal.h"
#include "console.h"
#include "fb.h"
#include "mouse.h"
#include "font_unicode.h"
#include "task.h"
#include "timer.h"
#include "irq.h"
#include "gui_event.h"
#include "virtio_gpu.h"

static volatile uint32_t gui_event_sequence_value = 1;
static uint32_t gui_event_waiters;

static uint32_t gui_event_ms_to_ticks(uint32_t ms) {
    uint32_t ticks = (ms * TIMER_HZ + 999u) / 1000u;
    return ticks ? ticks : 1u;
}

static void gui_event_publish_locked(void) {
    uint32_t waiters;
    gui_event_sequence_value++;
    if (gui_event_sequence_value == 0)
        gui_event_sequence_value = 1;
    waiters = gui_event_waiters;
    for (int tid = 1; tid < MAX_TASKS; tid++)
        if (waiters & (1u << tid))
            (void)task_wake(tid);
}

void gui_event_notify_display(void) {
    uint32_t flags = irq_save();
    gui_event_publish_locked();
    irq_restore(flags);
}

static void gui_event_reset(void) {
    uint32_t flags = irq_save();
    gui_event_publish_locked();
    gui_event_waiters = 0;
    irq_restore(flags);
}

static int user_owns_display(void) {
    return fb_display_user_allowed(task_get_pid());
}

intptr_t sys_font_glyph(uintptr_t codepoint, uintptr_t out_arg, uintptr_t cap,
                   uintptr_t d, uintptr_t e) {
    (void)d; (void)e;
    uint8_t glyph[UFONT_BYTES];
    if (cap < UFONT_BYTES)
        return -1;
    int ret = font_unicode_lookup(codepoint, glyph);
    if (ret < 0)
        return ret;
    return copy_to_user(out_arg, glyph, sizeof(glyph));
}

intptr_t sys_gfx_info(uintptr_t out_arg, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)b; (void)c; (void)d; (void)e;
    struct syscall_gfx_info out;
    struct gfx_info info;
    fb_get_info(&info);
    out.width = info.width;
    out.height = info.height;
    out.pitch = info.pitch;
    out.bpp = info.bpp;
    out.backend = info.backend;
    return copy_to_user(out_arg, &out, sizeof(out));
}

intptr_t sys_gfx_clear(uintptr_t color, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)b; (void)c; (void)d; (void)e;
    if (!user_owns_display())
        return -1;
    return fb_clear(color);
}

intptr_t sys_gfx_putpixel(uintptr_t x, uintptr_t y, uintptr_t color, uintptr_t d, uintptr_t e) {
    (void)d; (void)e;
    if (!user_owns_display())
        return -1;
    return fb_putpixel((int)x, (int)y, color);
}

intptr_t sys_gfx_fill_rect(uintptr_t x, uintptr_t y, uintptr_t w, uintptr_t h, uintptr_t color) {
    if (!user_owns_display())
        return -1;
    return fb_fill_rect((int)x, (int)y, (int)w, (int)h, color);
}

intptr_t sys_gfx_text(uintptr_t x, uintptr_t y, uintptr_t s_arg, uintptr_t fg, uintptr_t bg) {
    char text[4096];
    if (!user_owns_display() ||
        copy_string_from_user(text, sizeof(text), s_arg) < 0)
        return -1;
    /* bg is 0xFFFFFFFF (-1 as uint32) for transparent background. */
    return fb_text((int)x, (int)y, text, fg, (int)bg);
}

intptr_t sys_fb_blit(uintptr_t x, uintptr_t y, uintptr_t w, uintptr_t h, uintptr_t pixels_arg) {
    struct gfx_info info;
    if (!user_owns_display())
        return -1;
    if (w == 0 || h == 0)
        return 0;
    fb_get_info(&info);
    if (x >= info.width || y >= info.height)
        return -1;
    if (w > info.width || h > info.height)
        return -1;
    if (x + w > info.width || y + h > info.height)
        return -1;
    uint32_t row[2048];
    if (w > sizeof(row) / sizeof(row[0]))
        return -1;
    for (size_t line = 0; line < h; line++) {
        size_t offset = line * (size_t)w * sizeof(uint32_t);
        if (copy_from_user(row, pixels_arg + offset,
                           (size_t)w * sizeof(uint32_t)) < 0 ||
            fb_blit32((int)x, (int)(y + line), (int)w, 1, row) < 0)
            return -1;
    }
    return 0;
}

intptr_t sys_fb_blit_stride(uintptr_t x, uintptr_t y, uintptr_t packed_wh,
                       uintptr_t pixels_arg, uintptr_t stride) {
    struct gfx_info info;
    if (!user_owns_display())
        return -1;
    uint32_t w = packed_wh & 0xFFFFu, h = packed_wh >> 16;
    if (!w || !h || stride < w) return -1;
    fb_get_info(&info);
    if (x >= info.width || y >= info.height ||
        w > info.width || h > info.height ||
        x + w > info.width || y + h > info.height)
        return -1;
    /* stride is in pixels; buffer is 32bpp. */
    uint32_t row[2048];
    if (w > sizeof(row) / sizeof(row[0]) ||
        (uint64_t)(h - 1u) * stride + w > SIZE_MAX / sizeof(uint32_t))
        return -1;
    for (uint32_t line = 0; line < h; line++) {
        size_t offset = (size_t)line * (size_t)stride * sizeof(uint32_t);
        if (copy_from_user(row, pixels_arg + offset,
                           (size_t)w * sizeof(uint32_t)) < 0 ||
            fb_blit32((int)x, (int)(y + line), (int)w, 1, row) < 0)
            return -1;
    }
    return 0;
}

intptr_t sys_mouse_get(uintptr_t out_arg, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)b; (void)c; (void)d; (void)e;
    struct mouse_state out;
    mouse_get_state(&out);
    return copy_to_user(out_arg, &out, sizeof(out));
}

intptr_t sys_gfx_acquire(uintptr_t a, uintptr_t b, uintptr_t c,
                    uintptr_t d, uintptr_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    int result = fb_display_acquire(task_get_pid());
    if (result == 0)
        gui_event_reset();
    return result;
}

intptr_t sys_gfx_release(uintptr_t a, uintptr_t b, uintptr_t c,
                    uintptr_t d, uintptr_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    int pid = task_get_pid();
    if (!user_owns_display())
        return -1;
    (void)virtio_gpu_cursor_move(0, 0, 0);
    (void)fb_restore_boot_mode();
    mouse_clamp_to_screen();
    if (fb_display_release(pid) < 0)
        return -1;
    console_activate(1);
    return 0;
}

intptr_t sys_gfx_set_mode(uintptr_t width, uintptr_t height, uintptr_t c,
                     uintptr_t d, uintptr_t e) {
    (void)c; (void)d; (void)e;
    if (!user_owns_display() || fb_set_mode(width, height) < 0)
        return -1;
    mouse_clamp_to_screen();
    return 0;
}

intptr_t sys_gfx_map_surface(uintptr_t out_arg, uintptr_t b, uintptr_t c,
                        uintptr_t d, uintptr_t e) {
    (void)b; (void)c; (void)d; (void)e;
    if (!user_owns_display())
        return -1;
    struct fb_scanout_map map;
    if (fb_map_scanout_user(task_get_pid(), &map) < 0)
        return -1;
    struct syscall_gfx_surface out;
    out.address = map.user_va;
    out.width = map.width;
    out.height = map.height;
    out.stride_pixels = map.stride_pixels;
    out.bytes = map.bytes;
    out.backend = map.backend;
    if (copy_to_user(out_arg, &out, sizeof(out)) < 0) {
        (void)fb_unmap_scanout_user(task_get_pid());
        return -1;
    }
    return 0;
}

intptr_t sys_gfx_present(uintptr_t x, uintptr_t y, uintptr_t w, uintptr_t h,
                    uintptr_t e) {
    (void)e;
    if (!user_owns_display())
        return -1;
    return fb_present_rect(task_get_pid(), (int)x, (int)y, (int)w, (int)h);
}

intptr_t sys_gfx_cursor_define(uintptr_t pixels_arg, uintptr_t packed_wh,
                          uintptr_t packed_hot, uintptr_t packed_xy,
                          uintptr_t e) {
    uint32_t width = packed_wh & 0xFFFFu;
    uint32_t height = packed_wh >> 16;
    uint32_t hot_x = packed_hot & 0xFFFFu;
    uint32_t hot_y = packed_hot >> 16;
    uint32_t x = packed_xy & 0xFFFFu;
    uint32_t y = packed_xy >> 16;
    uint32_t bytes;
    (void)e;
    if (!user_owns_display() || !width || !height || width > 64u ||
        height > 64u)
        return -1;
    uint32_t pixels[64u * 64u];
    bytes = width * height * sizeof(uint32_t);
    if (copy_from_user(pixels, pixels_arg, bytes) < 0)
        return -1;
    return virtio_gpu_cursor_define(
        pixels, width, height,
        hot_x, hot_y, x, y);
}

intptr_t sys_gfx_cursor_move(uintptr_t x, uintptr_t y, uintptr_t visible,
                        uintptr_t d, uintptr_t e) {
    (void)d; (void)e;
    if (!user_owns_display())
        return -1;
    return virtio_gpu_cursor_move(x, y, visible ? 1 : 0);
}

intptr_t sys_gui_event_sequence(uintptr_t a, uintptr_t b, uintptr_t c,
                           uintptr_t d, uintptr_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    if (!user_owns_display())
        return -1;
    uint32_t flags = irq_save();
    uint32_t sequence = gui_event_sequence_value;
    irq_restore(flags);
    return (int)sequence;
}

intptr_t sys_gui_event_wait(uintptr_t expected, uintptr_t timeout_ms, uintptr_t c,
                       uintptr_t d, uintptr_t e) {
    (void)c; (void)d; (void)e;
    if (!user_owns_display() || !current_task || current_task->id <= 0 ||
        current_task->id >= MAX_TASKS)
        return -1;

    uint32_t flags = irq_save();
    if (gui_event_sequence_value != expected) {
        uint32_t sequence = gui_event_sequence_value;
        irq_restore(flags);
        return (int)sequence;
    }

    int tid = current_task->id;
    uint32_t deadline = timer_ticks() + gui_event_ms_to_ticks(timeout_ms);
    gui_event_waiters |= 1u << tid;
    task_prepare_block_current(deadline);
    /* IRQs stay disabled across registration and schedule, so neither input
     * nor another GUI thread can publish between the check and the block. */
    task_block_current_prepared();
    gui_event_waiters &= ~(1u << tid);
    uint32_t sequence = gui_event_sequence_value;
    irq_restore(flags);
    return (int)sequence;
}

intptr_t sys_gui_event_signal(uintptr_t a, uintptr_t b, uintptr_t c,
                         uintptr_t d, uintptr_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    if (!user_owns_display())
        return -1;
    gui_event_notify_display();
    return 0;
}
