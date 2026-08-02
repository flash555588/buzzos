/*
 * virgl 3D resources and command submission for virtio-gpu.
 *
 * This layer owns the guest side of the host GL pipeline: the render context,
 * GPU resources and their guest backing stores, and the passthrough for
 * command streams encoded in user space.  Command *encoding* deliberately
 * lives in the compositor, not here -- TGSI shader text and vertex data want
 * floating point and string handling, and the kernel is built -mno-sse.
 *
 * When virgl is unavailable (plain virtio-vga, Bochs VBE, Limine linear
 * framebuffer) every entry point fails cleanly and the software compositor
 * keeps working unchanged.
 */

#include <stddef.h>
#include <stdint.h>
#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "user_bounds.h"
#include "virgl_protocol.h"
#include "virtio_gpu.h"
#include "virtio_gpu_internal.h"

enum {
    VGPU_3D_CTX_ID = 1,
    VGPU_3D_MAX_RESOURCES = USER_GPU_SLOTS,
};

struct vgpu_3d_resource {
    uint32_t id;
    uintptr_t phys;
    uint32_t bytes;
    uint32_t pages;
    uint32_t user_va;
    uint32_t owner_cr3; /* address space holding the user mapping */
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t target;
    int mapped;
    int used;
};

static int gpu_3d_ready;
static uint32_t gpu_3d_rt_resource;
static uint32_t gpu_3d_width;
static uint32_t gpu_3d_height;
static uint32_t gpu_3d_backing_bytes;
static struct vgpu_3d_resource gpu_3d_resources[VGPU_3D_MAX_RESOURCES];

/* TRANSFER_TO_HOST_3D */
struct vgpu_box {
    uint32_t x, y, z;
    uint32_t w, h, d;
} __attribute__((packed));

struct vgpu_transfer_host_3d {
    struct vgpu_ctrl_hdr hdr;
    struct vgpu_box box;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t level;
    uint32_t stride;
    uint32_t layer_stride;
} __attribute__((packed));

static void zero_bytes(void *pointer, uint32_t size) {
    uint8_t *bytes = (uint8_t *)pointer;
    for (uint32_t i = 0; i < size; i++)
        bytes[i] = 0;
}

static void copy_bytes(void *destination, const void *source, uint32_t size) {
    uint8_t *dst = (uint8_t *)destination;
    const uint8_t *src = (const uint8_t *)source;
    for (uint32_t i = 0; i < size; i++)
        dst[i] = src[i];
}

/* ---- virtio-level 3D commands ---- */

static int vgpu_ctx_create(void) {
    struct vgpu_ctx_create request;
    const char *name = "buzzos";
    zero_bytes(&request, sizeof(request));
    request.hdr.type = VGPU_CMD_CTX_CREATE;
    request.hdr.ctx_id = VGPU_3D_CTX_ID;
    request.nlen = 6;
    request.context_init = 0;
    for (uint32_t i = 0; i < 6u; i++)
        request.debug_name[i] = name[i];
    return vgpu_submit_expect(&request, sizeof(request), VGPU_RESP_OK_NODATA);
}

static int vgpu_resource_create_3d(uint32_t resource_id, uint32_t target,
                                   uint32_t format, uint32_t bind,
                                   uint32_t width, uint32_t height) {
    struct vgpu_resource_create_3d request;
    zero_bytes(&request, sizeof(request));
    request.hdr.type = VGPU_CMD_RESOURCE_CREATE_3D;
    request.resource_id = resource_id;
    request.target = target;
    request.format = format;
    request.bind = bind;
    request.width = width;
    request.height = height;
    request.depth = 1;
    request.array_size = 1;
    request.last_level = 0;
    request.nr_samples = 0;
    request.flags = 0;
    return vgpu_submit_expect(&request, sizeof(request), VGPU_RESP_OK_NODATA);
}

static int vgpu_ctx_attach_resource(uint32_t resource_id, int attach) {
    struct vgpu_ctx_resource request;
    zero_bytes(&request, sizeof(request));
    request.hdr.type = attach ? VGPU_CMD_CTX_ATTACH_RESOURCE
                              : VGPU_CMD_CTX_DETACH_RESOURCE;
    request.hdr.ctx_id = VGPU_3D_CTX_ID;
    request.resource_id = resource_id;
    return vgpu_submit_expect(&request, sizeof(request), VGPU_RESP_OK_NODATA);
}

static int vgpu_resource_unref(uint32_t resource_id) {
    struct vgpu_resource_unref request;
    zero_bytes(&request, sizeof(request));
    request.hdr.type = VGPU_CMD_RESOURCE_UNREF;
    request.resource_id = resource_id;
    return vgpu_submit_expect(&request, sizeof(request), VGPU_RESP_OK_NODATA);
}

/* ---- resource table ---- */

static struct vgpu_3d_resource *resource_slot(uint32_t id) {
    if (!id)
        return 0;
    for (int i = 0; i < VGPU_3D_MAX_RESOURCES; i++) {
        if (gpu_3d_resources[i].used && gpu_3d_resources[i].id == id)
            return &gpu_3d_resources[i];
    }
    return 0;
}

static int resource_slot_index(const struct vgpu_3d_resource *entry) {
    return (int)(entry - gpu_3d_resources);
}

static struct vgpu_3d_resource *resource_alloc_slot(void) {
    for (int i = 0; i < VGPU_3D_MAX_RESOURCES; i++) {
        if (!gpu_3d_resources[i].used)
            return &gpu_3d_resources[i];
    }
    return 0;
}

/* Tear a resource down completely: user mapping, host resource, backing.
 *
 * The mapping is removed from the address space it was created in, recorded
 * as owner_cr3 -- not from paging_current_cr3(), which may already belong to
 * another process when release runs during teardown. */
static void resource_release(struct vgpu_3d_resource *entry) {
    if (!entry || !entry->used)
        return;
    if (entry->mapped && entry->owner_cr3) {
        (void)paging_unmap_user_range(entry->owner_cr3, entry->user_va,
                                      entry->bytes);
        entry->mapped = 0;
    }
    (void)vgpu_ctx_attach_resource(entry->id, 0);
    (void)vgpu_resource_unref(entry->id);
    if (entry->phys)
        pmm_free_pages(entry->phys, entry->pages);
    if (gpu_3d_backing_bytes >= entry->bytes)
        gpu_3d_backing_bytes -= entry->bytes;
    zero_bytes(entry, sizeof(*entry));
}

/* ---- public API ---- */

int virtio_gpu_3d_ready(void) { return gpu_3d_ready; }

int virtio_gpu_3d_query(struct gpu3d_info *out) {
    if (!out)
        return -1;
    out->available = (uint32_t)(gpu_3d_ready ? 1 : 0);
    out->width = gpu_3d_width;
    out->height = gpu_3d_height;
    out->scanout_resource = gpu_3d_rt_resource;
    out->max_resources = VGPU_3D_MAX_RESOURCES;
    out->command_capacity = vgpu_large_request_capacity();
    return 0;
}

int virtio_gpu_3d_resource_create(uint32_t target, uint32_t format,
                                  uint32_t bind, uint32_t width,
                                  uint32_t height, uint32_t *out_id,
                                  uint32_t *out_user_va, uint32_t *out_bytes) {
    struct vgpu_3d_resource *entry;
    uint64_t bytes64;
    uint32_t bytes, pages, slot_va;
    uintptr_t phys;

    if (!gpu_3d_ready || !out_id || !out_user_va || !out_bytes)
        return -1;
    if (!width || !height)
        return -1;
    /* PIPE_BUFFER resources are linear: width carries the byte count. */
    bytes64 = (target == 0) ? (uint64_t)width
                            : (uint64_t)width * (uint64_t)height * 4u;
    if (bytes64 == 0 || bytes64 > USER_GPU_SLOT_SIZE)
        return -1;
    bytes = ((uint32_t)bytes64 + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    if (gpu_3d_backing_bytes + bytes > USER_GPU_BUDGET_BYTES) {
        serial_puts("[gpu3d] resource budget exhausted\n");
        return -1;
    }

    entry = resource_alloc_slot();
    if (!entry)
        return -1;
    pages = bytes / PAGE_SIZE;
    phys = pmm_alloc_pages(pages);
    if (!phys)
        return -1;

    entry->used = 1;
    entry->id = vgpu_alloc_resource_id();
    entry->phys = phys;
    entry->bytes = bytes;
    entry->pages = pages;
    entry->width = width;
    entry->height = height;
    entry->format = format;
    entry->target = target;

    if (vgpu_resource_create_3d(entry->id, target, format, bind, width,
                                height) < 0 ||
        vgpu_attach_backing(entry->id, phys, bytes) < 0 ||
        vgpu_ctx_attach_resource(entry->id, 1) < 0) {
        serial_puts("[gpu3d] resource create failed\n");
        pmm_free_pages(phys, pages);
        zero_bytes(entry, sizeof(*entry));
        return -1;
    }

    slot_va = USER_GPU_START +
              (uint32_t)resource_slot_index(entry) * USER_GPU_SLOT_SIZE;
    entry->owner_cr3 = paging_current_cr3();
    if (paging_map_user_phys(entry->owner_cr3, slot_va, phys, bytes,
                             PAGE_PRESENT | PAGE_RW | PAGE_USER) < 0) {
        serial_puts("[gpu3d] resource user map failed\n");
        resource_release(entry);
        return -1;
    }
    entry->user_va = slot_va;
    entry->mapped = 1;
    gpu_3d_backing_bytes += bytes;

    *out_id = entry->id;
    *out_user_va = slot_va;
    *out_bytes = bytes;
    return 0;
}

int virtio_gpu_3d_resource_destroy(uint32_t id) {
    struct vgpu_3d_resource *entry = resource_slot(id);
    if (!gpu_3d_ready || !entry || id == gpu_3d_rt_resource)
        return -1;
    resource_release(entry);
    return 0;
}

/* Push a damaged box from the resource's own backing store to the host.  The
 * compositor already wrote the pixels through its user mapping, so there is
 * no intermediate copy. */
int virtio_gpu_3d_resource_upload(uint32_t id, int x, int y, int w, int h) {
    struct vgpu_3d_resource *entry = resource_slot(id);
    struct vgpu_transfer_host_3d request;
    uint32_t stride;

    if (!gpu_3d_ready || !entry || w <= 0 || h <= 0 || x < 0 || y < 0)
        return -1;
    if ((uint32_t)x + (uint32_t)w > entry->width)
        return -1;
    if (entry->target != 0 && (uint32_t)y + (uint32_t)h > entry->height)
        return -1;

    zero_bytes(&request, sizeof(request));
    request.hdr.type = VGPU_CMD_TRANSFER_TO_HOST_3D;
    request.hdr.ctx_id = VGPU_3D_CTX_ID;
    request.box.x = (uint32_t)x;
    request.box.y = (uint32_t)y;
    request.box.z = 0;
    request.box.w = (uint32_t)w;
    request.box.h = (uint32_t)h;
    request.box.d = 1;
    request.resource_id = entry->id;
    request.level = 0;
    if (entry->target == 0) {
        /* Linear buffer: byte offsets, single row. */
        request.offset = (uint64_t)(uint32_t)x;
        request.stride = 0;
    } else {
        stride = entry->width * 4u;
        request.offset =
            (uint64_t)((uint32_t)y * stride + (uint32_t)x * 4u);
        request.stride = stride;
    }
    request.layer_stride = 0;
    __sync_synchronize();
    return vgpu_submit_expect(&request, sizeof(request), VGPU_RESP_OK_NODATA);
}

/* Pass a user-encoded command stream to the host.  The dwords are copied once
 * into a staging buffer so the guest cannot mutate them after validation, and
 * because SUBMIT_3D wants the header and payload contiguous.  i386 is
 * little-endian and the wire format is little-endian, so the payload is a
 * straight copy rather than a byte-by-byte serialize. */
int virtio_gpu_3d_submit(const uint32_t *dwords, uint32_t count) {
    static uint8_t *staging;
    struct vgpu_cmd_submit *header;
    uint32_t payload_bytes = count * 4u;
    uint32_t total = (uint32_t)sizeof(struct vgpu_cmd_submit) + payload_bytes;

    if (!gpu_3d_ready || !dwords || !count)
        return -1;
    if (count > 0x3FFFFFFFu || total > vgpu_large_request_capacity())
        return -1;
    if (!staging) {
        uintptr_t phys = pmm_alloc_pages(
            (vgpu_large_request_capacity() + PAGE_SIZE - 1u) / PAGE_SIZE);
        if (!phys)
            return -1;
        staging = (uint8_t *)phys;
    }

    header = (struct vgpu_cmd_submit *)staging;
    zero_bytes(header, sizeof(*header));
    header->hdr.type = VGPU_CMD_SUBMIT_3D;
    header->hdr.ctx_id = VGPU_3D_CTX_ID;
    header->size = payload_bytes;
    copy_bytes(staging + sizeof(*header), dwords, payload_bytes);
    return vgpu_submit_large(staging, total, VGPU_RESP_OK_NODATA);
}

int virtio_gpu_3d_present(int x, int y, int w, int h) {
    if (!gpu_3d_ready || w <= 0 || h <= 0)
        return -1;
    return vgpu_flush_resource(gpu_3d_rt_resource, x, y, w, h);
}

int virtio_gpu_3d_scanout(int enable) {
    if (!gpu_3d_ready)
        return -1;
    if (enable)
        return vgpu_set_scanout(gpu_3d_rt_resource, gpu_3d_width,
                                gpu_3d_height);
    return vgpu_set_scanout(vgpu_scanout_resource(), gpu_3d_width,
                            gpu_3d_height);
}

void virtio_gpu_3d_release(void) {
    if (!gpu_3d_ready)
        return;
    for (int i = 0; i < VGPU_3D_MAX_RESOURCES; i++) {
        if (gpu_3d_resources[i].used &&
            gpu_3d_resources[i].id != gpu_3d_rt_resource)
            resource_release(&gpu_3d_resources[i]);
    }
    (void)virtio_gpu_3d_scanout(0);
}

/* ---- bring-up ---- */

int virtio_gpu_3d_init(void) {
    uint32_t width = vgpu_scanout_width();
    uint32_t height = vgpu_scanout_height();
    uint32_t bind = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW |
                    VIRGL_BIND_SCANOUT;

    if (gpu_3d_ready)
        return 0;
    if (!vgpu_virgl_offered()) {
        serial_puts("[gpu3d] virgl not offered; software compositor only\n");
        return -1;
    }
    if (!width || !height) {
        serial_puts("[gpu3d] no scanout geometry\n");
        return -1;
    }
    if (vgpu_ctx_create() < 0) {
        serial_puts("[gpu3d] CTX_CREATE failed\n");
        return -1;
    }

    /* The scanout render target is owned by the kernel and lives for the
     * lifetime of the device: the compositor renders into it and presents. */
    gpu_3d_rt_resource = vgpu_alloc_resource_id();
    if (vgpu_resource_create_3d(gpu_3d_rt_resource, VGPU_PIPE_TEXTURE_2D,
                                VIRGL_FORMAT_B8G8R8A8_UNORM, bind,
                                width, height) < 0 ||
        vgpu_ctx_attach_resource(gpu_3d_rt_resource, 1) < 0) {
        serial_puts("[gpu3d] render target setup failed\n");
        gpu_3d_rt_resource = 0;
        return -1;
    }

    gpu_3d_width = width;
    gpu_3d_height = height;
    gpu_3d_ready = 1;
    serial_puts("[gpu3d] virgl ready: ctx=1 rt=");
    serial_puthex(gpu_3d_rt_resource);
    serial_puts(" ");
    serial_puthex(width);
    serial_puts("x");
    serial_puthex(height);
    serial_puts("\n");
    return 0;
}
