#ifndef BUZZOS_VIRTIO_GPU_INTERNAL_H
#define BUZZOS_VIRTIO_GPU_INTERNAL_H

/*
 * Transport surface shared between the virtio-gpu 2D core (virtio_gpu.c) and
 * the virgl 3D layer (virtio_gpu_3d.c).  Not a public kernel API: everything
 * here assumes the device is already probed, the control queue is live, and
 * the caller owns the display.
 */

#include <stdint.h>

/* virtio-gpu control commands used by both layers. */
enum {
    VGPU_CMD_RESOURCE_UNREF = 0x0102,
    VGPU_CMD_SET_SCANOUT = 0x0103,
    VGPU_CMD_RESOURCE_FLUSH = 0x0104,

    /* 3D / virgl block.  Values verified against Linux uapi virtio_gpu.h --
     * RESOURCE_CREATE_3D is 0x0204 and SUBMIT_3D is 0x0207. */
    VGPU_CMD_CTX_CREATE = 0x0200,
    VGPU_CMD_CTX_DESTROY = 0x0201,
    VGPU_CMD_CTX_ATTACH_RESOURCE = 0x0202,
    VGPU_CMD_CTX_DETACH_RESOURCE = 0x0203,
    VGPU_CMD_RESOURCE_CREATE_3D = 0x0204,
    VGPU_CMD_TRANSFER_TO_HOST_3D = 0x0205,
    VGPU_CMD_TRANSFER_FROM_HOST_3D = 0x0206,
    VGPU_CMD_SUBMIT_3D = 0x0207,

    VGPU_RESP_OK_NODATA = 0x1100,

    /* virtio_gpu_resource_create_3d.target -- PIPE_TEXTURE_2D. */
    VGPU_PIPE_TEXTURE_2D = 2,

    /* virtio_gpu_resource_create_3d.flags.  This makes transfer boxes,
     * framebuffer coordinates and QEMU scanout damage share the desktop's
     * top-left origin. */
    VGPU_RESOURCE_FLAG_Y_0_TOP = 1u << 0,
};

struct vgpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx;
    uint8_t padding[3];
} __attribute__((packed));

struct vgpu_rect_raw {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

/* VIRTIO_GPU_CMD_RESOURCE_CREATE_3D */
struct vgpu_resource_create_3d {
    struct vgpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t target;
    uint32_t format;
    uint32_t bind;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t array_size;
    uint32_t last_level;
    uint32_t nr_samples;
    uint32_t flags;
    uint32_t padding;
} __attribute__((packed));

/* VIRTIO_GPU_CMD_CTX_CREATE */
struct vgpu_ctx_create {
    struct vgpu_ctrl_hdr hdr;
    uint32_t nlen;
    uint32_t context_init;
    char debug_name[64];
} __attribute__((packed));

/* VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE / CTX_DETACH_RESOURCE */
struct vgpu_ctx_resource {
    struct vgpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

/* VIRTIO_GPU_CMD_SUBMIT_3D -- the command dwords follow this header
 * contiguously in the same request buffer. */
struct vgpu_cmd_submit {
    struct vgpu_ctrl_hdr hdr;
    uint32_t size;
    uint32_t padding;
} __attribute__((packed));

struct vgpu_set_scanout {
    struct vgpu_ctrl_hdr hdr;
    struct vgpu_rect_raw r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

struct vgpu_resource_flush {
    struct vgpu_ctrl_hdr hdr;
    struct vgpu_rect_raw r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

/* VIRTIO_GPU_CMD_RESOURCE_UNREF */
struct vgpu_resource_unref {
    struct vgpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

/* Submit one control-queue request and require an exact response type. */
int vgpu_submit_expect(const void *request, uint32_t request_size,
                       uint32_t expected_response);

/* Same, but for requests larger than one page (3D command streams).  Uses a
 * dedicated multi-page request buffer; returns -1 if the request does not fit
 * or the buffer could not be allocated. */
int vgpu_submit_large(const void *request, uint32_t request_size,
                      uint32_t expected_response);
uint32_t vgpu_large_request_capacity(void);

/* Attach a physically contiguous guest range as a resource's backing store. */
int vgpu_attach_backing(uint32_t resource_id, uintptr_t phys, uint32_t bytes);
/* Attach a page list.  Adjacent physical pages are coalesced into one virtio
 * memory entry; first_offset permits a resource to begin inside page zero. */
int vgpu_attach_backing_pages(uint32_t resource_id,
                              const uintptr_t *pages, uint32_t page_count,
                              uint32_t first_offset, uint32_t bytes);

/* Monotonic resource-id allocator shared with the 2D scanout resource. */
uint32_t vgpu_alloc_resource_id(void);

/* Device capability discovered during feature negotiation. */
int vgpu_virgl_offered(void);

/* Current 2D scanout resource + geometry, so the 3D layer can hand the
 * display back after a test or when the GPU path is disabled. */
uint32_t vgpu_scanout_resource(void);
uint32_t vgpu_scanout_width(void);
uint32_t vgpu_scanout_height(void);

/* Point scanout 0 at a resource and flush it to the host display. */
int vgpu_set_scanout(uint32_t resource_id, uint32_t width, uint32_t height);
int vgpu_flush_resource(uint32_t resource_id, int x, int y,
                        int width, int height);

/* Bring up the virgl context and run the pipeline self-test.  Returns 0 when
 * the 3D path is usable.  Safe to call when virgl is absent (returns -1 and
 * leaves the 2D path untouched). */
int virtio_gpu_3d_init(void);

#endif /* BUZZOS_VIRTIO_GPU_INTERNAL_H */
