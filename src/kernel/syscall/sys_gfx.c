#include "syscall_internal.h"
#include "fb.h"
#include "mouse.h"

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
    return 0;
}

int sys_gfx_clear(uint32_t color, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)b; (void)c; (void)d; (void)e;
    return fb_clear((uint8_t)color);
}

int sys_gfx_putpixel(uint32_t x, uint32_t y, uint32_t color, uint32_t d, uint32_t e) {
    (void)d; (void)e;
    return fb_putpixel((int)x, (int)y, (uint8_t)color);
}

int sys_gfx_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    return fb_fill_rect((int)x, (int)y, (int)w, (int)h, (uint8_t)color);
}

int sys_gfx_text(uint32_t x, uint32_t y, uint32_t s_arg, uint32_t fg, uint32_t bg) {
    const char *s = (const char *)(uintptr_t)s_arg;
    if (!user_string_ok(s))
        return -1;
    return fb_text((int)x, (int)y, s, (uint8_t)fg, (int)bg);
}

int sys_fb_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t pixels_arg) {
    struct gfx_info info;
    if (w == 0 || h == 0)
        return 0;
    fb_get_info(&info);
    if (x >= info.width || y >= info.height)
        return -1;
    if (w > info.width || h > info.height)
        return -1;
    if (x + w > info.width || y + h > info.height)
        return -1;
    if (!user_range_ok(pixels_arg, w * h))
        return -1;
    const uint8_t *pixels = (const uint8_t *)(uintptr_t)pixels_arg;
    return fb_blit8((int)x, (int)y, (int)w, (int)h, pixels);
}

int sys_mouse_get(uint32_t out_arg, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)b; (void)c; (void)d; (void)e;
    if (!user_range_writable(out_arg, sizeof(struct mouse_state)))
        return -1;
    struct mouse_state *out = (struct mouse_state *)(uintptr_t)out_arg;
    mouse_get_state(out);
    return 0;
}
