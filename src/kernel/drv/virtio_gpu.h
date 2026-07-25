#ifndef BUZZOS_VIRTIO_GPU_H
#define BUZZOS_VIRTIO_GPU_H

#include <stdint.h>

/*
 * Minimal virtio-gpu 2D scanout backend.
 *
 * BuzzOS keeps rendering in software, but the display resource, scanout and
 * damage uploads are owned by virtio-gpu instead of a legacy linear VGA
 * framebuffer.  The caller can keep the existing framebuffer path as a
 * fallback when no compatible device is present.
 */
int virtio_gpu_init(uint32_t width, uint32_t height);
int virtio_gpu_ready(void);
int virtio_gpu_set_mode(uint32_t width, uint32_t height);
uint32_t *virtio_gpu_pixels(void);
uint32_t virtio_gpu_stride(void);
uintptr_t virtio_gpu_backing_phys(void);
uint32_t virtio_gpu_backing_bytes(void);
int virtio_gpu_flush(int x, int y, int width, int height);

#endif /* BUZZOS_VIRTIO_GPU_H */
