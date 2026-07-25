#include "syscall_internal.h"
#include "console.h"
#include "fb.h"
#include "mouse.h"
#include "font_unicode.h"
#include "task.h"

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
    return fb_display_acquire(task_get_pid());
}

int sys_gfx_release(uint32_t a, uint32_t b, uint32_t c,
                    uint32_t d, uint32_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    int pid = task_get_pid();
    if (!user_owns_display())
        return -1;
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
