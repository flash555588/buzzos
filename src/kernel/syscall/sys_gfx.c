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

int sys_font_glyph(uint32_t codepoint, uint32_t out_arg, uint32_t cap,
                   uint32_t d, uint32_t e) {
    (void)d; (void)e;
    if (cap < UFONT_BYTES || !user_range_writable(out_arg, UFONT_BYTES))
        return -1;
    return font_unicode_lookup(codepoint, (uint8_t *)(uintptr_t)out_arg);
}

int sys_gfx_info(uint32_t out_arg, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)b; (void)c; (void)d; (void)e;
    if (!user_range_writable(out_arg, sizeof(struct syscall_gfx_info)))
        return -1;
    struct syscall_gfx_info *out = (struct syscall_gfx_info *)(uintptr_t)out_arg;
    struct gfx_info info;
    fb_get_info(&info);
    out->width = info.width;
    out->height = info.height;
    out->pitch = info.pitch;
    out->bpp = info.bpp;
    out->backend = info.backend;
    return 0;
}

int sys_gfx_clear(uint32_t color, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)b; (void)c; (void)d; (void)e;
    if (!user_owns_display())
        return -1;
    return fb_clear(color);
}

int sys_gfx_putpixel(uint32_t x, uint32_t y, uint32_t color, uint32_t d, uint32_t e) {
    (void)d; (void)e;
    if (!user_owns_display())
        return -1;
    return fb_putpixel((int)x, (int)y, color);
}

int sys_gfx_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!user_owns_display())
        return -1;
    return fb_fill_rect((int)x, (int)y, (int)w, (int)h, color);
}

int sys_gfx_text(uint32_t x, uint32_t y, uint32_t s_arg, uint32_t fg, uint32_t bg) {
    const char *s = (const char *)(uintptr_t)s_arg;
    if (!user_owns_display() || !user_string_ok(s))
        return -1;
    /* bg is 0xFFFFFFFF (-1 as uint32) for transparent background. */
    return fb_text((int)x, (int)y, s, fg, (int)bg);
}

int sys_fb_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t pixels_arg) {
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
    uint64_t bytes = (uint64_t)w * (uint64_t)h * 4u;
    if (bytes > 0xFFFFFFFFu || !user_range_ok(pixels_arg, (uint32_t)bytes))
        return -1;
    const uint32_t *pixels = (const uint32_t *)(uintptr_t)pixels_arg;
    return fb_blit32((int)x, (int)y, (int)w, (int)h, pixels);
}

int sys_fb_blit_stride(uint32_t x, uint32_t y, uint32_t packed_wh,
                       uint32_t pixels_arg, uint32_t stride) {
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
    uint64_t bytes = ((uint64_t)(h - 1u) * (uint64_t)stride + (uint64_t)w) * 4u;
    if (bytes > 0xFFFFFFFFu || !user_range_ok(pixels_arg, (uint32_t)bytes))
        return -1;
    const uint32_t *pixels = (const uint32_t *)(uintptr_t)pixels_arg;
    return fb_blit32_stride((int)x, (int)y, (int)w, (int)h,
                            pixels, (int)stride);
}

int sys_mouse_get(uint32_t out_arg, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)b; (void)c; (void)d; (void)e;
    if (!user_range_writable(out_arg, sizeof(struct mouse_state)))
        return -1;
    struct mouse_state *out = (struct mouse_state *)(uintptr_t)out_arg;
    mouse_get_state(out);
    return 0;
}

int sys_gfx_acquire(uint32_t a, uint32_t b, uint32_t c,
                    uint32_t d, uint32_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    int result = fb_display_acquire(task_get_pid());
    if (result == 0)
        gui_event_reset();
    return result;
}

int sys_gfx_release(uint32_t a, uint32_t b, uint32_t c,
                    uint32_t d, uint32_t e) {
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

int sys_gfx_set_mode(uint32_t width, uint32_t height, uint32_t c,
                     uint32_t d, uint32_t e) {
    (void)c; (void)d; (void)e;
    if (!user_owns_display() || fb_set_mode(width, height) < 0)
        return -1;
    mouse_clamp_to_screen();
    return 0;
}

int sys_gfx_map_surface(uint32_t out_arg, uint32_t b, uint32_t c,
                        uint32_t d, uint32_t e) {
    (void)b; (void)c; (void)d; (void)e;
    if (!user_owns_display() ||
        !user_range_writable(out_arg, sizeof(struct syscall_gfx_surface)))
        return -1;
    struct fb_scanout_map map;
    if (fb_map_scanout_user(task_get_pid(), &map) < 0)
        return -1;
    struct syscall_gfx_surface *out =
        (struct syscall_gfx_surface *)(uintptr_t)out_arg;
    out->address = map.user_va;
    out->width = map.width;
    out->height = map.height;
    out->stride_pixels = map.stride_pixels;
    out->bytes = map.bytes;
    out->backend = map.backend;
    return 0;
}

int sys_gfx_present(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                    uint32_t e) {
    (void)e;
    if (!user_owns_display())
        return -1;
    return fb_present_rect(task_get_pid(), (int)x, (int)y, (int)w, (int)h);
}

int sys_gfx_cursor_define(uint32_t pixels_arg, uint32_t packed_wh,
                          uint32_t packed_hot, uint32_t packed_xy,
                          uint32_t e) {
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
    bytes = width * height * sizeof(uint32_t);
    if (!user_range_ok(pixels_arg, bytes))
        return -1;
    return virtio_gpu_cursor_define(
        (const uint32_t *)(uintptr_t)pixels_arg, width, height,
        hot_x, hot_y, x, y);
}

int sys_gfx_cursor_move(uint32_t x, uint32_t y, uint32_t visible,
                        uint32_t d, uint32_t e) {
    (void)d; (void)e;
    if (!user_owns_display())
        return -1;
    return virtio_gpu_cursor_move(x, y, visible ? 1 : 0);
}

int sys_gui_event_sequence(uint32_t a, uint32_t b, uint32_t c,
                           uint32_t d, uint32_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    if (!user_owns_display())
        return -1;
    uint32_t flags = irq_save();
    uint32_t sequence = gui_event_sequence_value;
    irq_restore(flags);
    return (int)sequence;
}

int sys_gui_event_wait(uint32_t expected, uint32_t timeout_ms, uint32_t c,
                       uint32_t d, uint32_t e) {
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

int sys_gui_event_signal(uint32_t a, uint32_t b, uint32_t c,
                         uint32_t d, uint32_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    if (!user_owns_display())
        return -1;
    gui_event_notify_display();
    return 0;
}
