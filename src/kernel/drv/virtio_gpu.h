#ifndef BUZZOS_VIRTIO_GPU_H
#define BUZZOS_VIRTIO_GPU_H

#include <stdint.h>

/*
 * Minimal virtio-gpu 2D scanout backend.
 *
 * The 2-D resource remains the software fallback.  When virgl is offered, a
 * separate 3-D module owns the render target used by the per-surface GPU
 * compositor and validated app Canvas path.
 */
int virtio_gpu_init(uint32_t width, uint32_t height);
int virtio_gpu_ready(void);
int virtio_gpu_set_mode(uint32_t width, uint32_t height);
uint32_t *virtio_gpu_pixels(void);
uint32_t virtio_gpu_stride(void);
uintptr_t virtio_gpu_backing_phys(void);
uint32_t virtio_gpu_backing_bytes(void);
int virtio_gpu_flush(int x, int y, int width, int height);

/* Hardware cursor plane.  The image is ARGB8888 and may be up to the
 * virtio-gpu mandated 64x64 cursor resource.  Moving it uses cursorq only and
 * never damages or uploads the scanout. */
int virtio_gpu_cursor_define(const uint32_t *pixels, uint32_t width,
                             uint32_t height, uint32_t hot_x,
                             uint32_t hot_y, uint32_t x, uint32_t y);
int virtio_gpu_cursor_move(uint32_t x, uint32_t y, int visible);

/* ---- virgl 3D pipeline ----
 *
 * Command encoding lives in user space (the compositor); the kernel owns the
 * render context, resources and their guest backing, and passes command
 * streams through.  All entry points fail cleanly when virgl is absent. */

struct gpu3d_info {
    uint32_t available;
    uint32_t width;
    uint32_t height;
    uint32_t scanout_resource;
    uint32_t max_resources;
    uint32_t command_capacity; /* bytes accepted by one submit */
};

/* Non-zero once the virgl 3D context and render target are live.  Always 0 on
 * hosts without virgl, where the software compositor path is used. */
int virtio_gpu_3d_ready(void);
int virtio_gpu_3d_init(void);
int virtio_gpu_3d_resize(uint32_t width, uint32_t height);
int virtio_gpu_3d_query(struct gpu3d_info *out);

/* Create a GPU resource with a guest backing store mapped into the calling
 * address space, so the compositor writes texture pixels with no extra copy.
 * target 0 = linear buffer (width carries the byte count), 2 = 2D texture. */
int virtio_gpu_3d_resource_create(uint32_t target, uint32_t format,
                                  uint32_t bind, uint32_t width,
                                  uint32_t height, uint32_t *out_id,
                                  uintptr_t *out_user_va, uint32_t *out_bytes);
/* Create a texture whose guest backing aliases an existing SHM byte range.
 * The SHM object is pinned until the GPU resource is destroyed. */
int virtio_gpu_3d_resource_import_shm(uint32_t shm_token, int owner,
                                     uint32_t shm_offset, uint32_t target,
                                     uint32_t format, uint32_t bind,
                                     uint32_t width, uint32_t height,
                                     uint32_t *out_id);
int virtio_gpu_3d_resource_destroy(uint32_t id);
int virtio_gpu_3d_resource_upload(uint32_t id, int x, int y, int w, int h);
int virtio_gpu_3d_submit(const uint32_t *dwords, uint32_t count);
int virtio_gpu_3d_present(int x, int y, int w, int h);
/* Point the display at the 3D render target (1) or back at the 2D scanout
 * resource (0), so the software path stays usable. */
int virtio_gpu_3d_scanout(int enable);
void virtio_gpu_3d_release(void);

#endif /* BUZZOS_VIRTIO_GPU_H */
