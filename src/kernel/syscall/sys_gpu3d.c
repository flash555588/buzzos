/*
 * virgl 3D syscalls.
 *
 * Thin, ownership-checked passthrough to the virtio-gpu 3D layer.  Only the
 * process that owns the display may touch the GPU context, matching the rest
 * of the graphics syscalls -- a background process must not be able to scribble
 * on the compositor's render target or hand the scanout to itself.
 *
 * Command streams are validated for size only.  Their *contents* are encoded
 * by the compositor in user space and interpreted by the host, so a malformed
 * stream can break rendering for the display owner but cannot reach another
 * process: virglrenderer runs one context per guest context id, and the only
 * resources bound to it are ones this layer created.
 */

#include "syscall_internal.h"
#include "fb.h"
#include "task.h"
#include "virtio_gpu.h"

static int user_owns_display(void) {
    return fb_display_user_allowed(task_get_pid());
}

intptr_t sys_gpu3d_info(uintptr_t out_arg, uintptr_t b, uintptr_t c, uintptr_t d,
                   uintptr_t e) {
    (void)b; (void)c; (void)d; (void)e;
    if (!user_range_writable(out_arg, sizeof(struct syscall_gpu3d_info)))
        return -1;
    struct syscall_gpu3d_info *out =
        (struct syscall_gpu3d_info *)(uintptr_t)out_arg;
    struct gpu3d_info info;
    if (virtio_gpu_3d_query(&info) < 0) {
        out->available = 0;
        out->width = 0;
        out->height = 0;
        out->scanout_resource = 0;
        out->max_resources = 0;
        out->command_capacity = 0;
        return 0;
    }
    out->available = info.available;
    out->width = info.width;
    out->height = info.height;
    out->scanout_resource = info.scanout_resource;
    out->max_resources = info.max_resources;
    out->command_capacity = info.command_capacity;
    return 0;
}

intptr_t sys_gpu3d_resource_create(uintptr_t io_arg, uintptr_t b, uintptr_t c,
                              uintptr_t d, uintptr_t e) {
    (void)b; (void)c; (void)d; (void)e;
    if (!user_owns_display() ||
        !user_range_writable(io_arg, sizeof(struct syscall_gpu3d_resource)))
        return -1;
    struct syscall_gpu3d_resource *io =
        (struct syscall_gpu3d_resource *)(uintptr_t)io_arg;
    uint32_t id = 0, bytes = 0;
    uintptr_t address = 0;
    if (virtio_gpu_3d_resource_create(io->target, io->format, io->bind,
                                      io->width, io->height, &id, &address,
                                      &bytes) < 0)
        return -1;
    io->id = id;
    io->address = address;
    io->bytes = bytes;
    return 0;
}

intptr_t sys_gpu3d_resource_destroy(uintptr_t id, uintptr_t b, uintptr_t c,
                               uintptr_t d, uintptr_t e) {
    (void)b; (void)c; (void)d; (void)e;
    if (!user_owns_display())
        return -1;
    return virtio_gpu_3d_resource_destroy(id);
}

intptr_t sys_gpu3d_import_shm(uintptr_t io_arg, uintptr_t b, uintptr_t c,
                         uintptr_t d, uintptr_t e) {
    (void)b; (void)c; (void)d; (void)e;
    if (!user_owns_display() ||
        !user_range_writable(io_arg, sizeof(struct syscall_gpu3d_import)))
        return -1;
    struct syscall_gpu3d_import *io =
        (struct syscall_gpu3d_import *)(uintptr_t)io_arg;
    uint32_t id = 0;
    if (virtio_gpu_3d_resource_import_shm(
            io->shm_token, task_get_pid(), io->shm_offset, io->target,
            io->format, io->bind, io->width, io->height, &id) < 0)
        return -1;
    io->id = id;
    return 0;
}

/* Coordinates are packed two-per-dword to fit the five-argument syscall ABI. */
intptr_t sys_gpu3d_upload(uintptr_t id, uintptr_t packed_xy, uintptr_t packed_wh,
                     uintptr_t d, uintptr_t e) {
    (void)d; (void)e;
    if (!user_owns_display())
        return -1;
    int x = (int)(packed_xy & 0xFFFFu);
    int y = (int)(packed_xy >> 16);
    int w = (int)(packed_wh & 0xFFFFu);
    int h = (int)(packed_wh >> 16);
    return virtio_gpu_3d_resource_upload(id, x, y, w, h);
}

intptr_t sys_gpu3d_submit(uintptr_t cmds_arg, uintptr_t dwords, uintptr_t c,
                     uintptr_t d, uintptr_t e) {
    (void)c; (void)d; (void)e;
    if (!user_owns_display() || !dwords)
        return -1;
    /* dwords * 4 must not overflow before the range check. */
    if (dwords > 0x3FFFFFFFu)
        return -1;
    if (!user_range_ok(cmds_arg, dwords * 4u))
        return -1;
    return virtio_gpu_3d_submit((const uint32_t *)(uintptr_t)cmds_arg, dwords);
}

intptr_t sys_gpu3d_present(uintptr_t x, uintptr_t y, uintptr_t w, uintptr_t h,
                      uintptr_t e) {
    (void)e;
    if (!user_owns_display())
        return -1;
    return virtio_gpu_3d_present((int)x, (int)y, (int)w, (int)h);
}

intptr_t sys_gpu3d_scanout(uintptr_t enable, uintptr_t b, uintptr_t c, uintptr_t d,
                      uintptr_t e) {
    (void)b; (void)c; (void)d; (void)e;
    if (!user_owns_display())
        return -1;
    return virtio_gpu_3d_scanout((int)enable);
}
