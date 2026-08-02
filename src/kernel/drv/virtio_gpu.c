#include <stddef.h>
#include <stdint.h>
#include "io.h"
#include "irq.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "serial.h"
#include "virtio_gpu.h"
#include "virtio_gpu_internal.h"

enum {
    VIRTIO_VENDOR_ID = 0x1AF4,
    VIRTIO_GPU_DEVICE_ID = 0x1050,

    PCI_STATUS_CAP_LIST = 0x0010,
    PCI_CAP_PTR = 0x34,
    PCI_CAP_ID_VENDOR = 0x09,
    VIRTIO_PCI_CAP_COMMON_CFG = 1,
    VIRTIO_PCI_CAP_NOTIFY_CFG = 2,
    VIRTIO_PCI_CAP_ISR_CFG = 3,
    VIRTIO_PCI_CAP_DEVICE_CFG = 4,

    VIRTIO_STATUS_ACKNOWLEDGE = 1,
    VIRTIO_STATUS_DRIVER = 2,
    VIRTIO_STATUS_DRIVER_OK = 4,
    VIRTIO_STATUS_FEATURES_OK = 8,
    VIRTIO_STATUS_FAILED = 0x80,
    VIRTIO_F_VERSION_1_HIGH = 1,

    VRING_DESC_F_NEXT = 1,
    VRING_DESC_F_WRITE = 2,
    VRING_AVAIL_F_NO_INTERRUPT = 1,
    GPU_QUEUE_INDEX = 0,
    GPU_QUEUE_MAX = 8,
    GPU_QUEUE_NO_VECTOR = 0xFFFF,

    VIRTIO_GPU_CMD_GET_DISPLAY_INFO = 0x0100,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_2D = 0x0101,
    VIRTIO_GPU_CMD_RESOURCE_UNREF = 0x0102,
    VIRTIO_GPU_CMD_SET_SCANOUT = 0x0103,
    VIRTIO_GPU_CMD_RESOURCE_FLUSH = 0x0104,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D = 0x0105,
    VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING = 0x0106,
    VIRTIO_GPU_RESP_OK_NODATA = 0x1100,
    VIRTIO_GPU_RESP_OK_DISPLAY_INFO = 0x1101,
    VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM = 2,
    VIRTIO_GPU_MAX_SCANOUTS = 16,

    /* virtio-gpu device feature bits (low feature dword). */
    VIRTIO_GPU_F_VIRGL_BIT = 1u << 0,
    VIRTIO_GPU_F_EDID_BIT = 1u << 1,
    VIRTIO_GPU_F_RESOURCE_UUID_BIT = 1u << 2,
    VIRTIO_GPU_F_RESOURCE_BLOB_BIT = 1u << 3,
    VIRTIO_GPU_F_CONTEXT_INIT_BIT = 1u << 4,

    /* Match fb_set_mode / GUIAPP_MAX_* so 1920x1200 (16:10) is allowed. */
    GPU_MAX_WIDTH = 1920,
    GPU_MAX_HEIGHT = 1200,
    GPU_BACKING_BYTES = GPU_MAX_WIDTH * GPU_MAX_HEIGHT * 4,
};

struct virtio_pci_common_cfg {
    volatile uint32_t device_feature_select;
    volatile uint32_t device_feature;
    volatile uint32_t guest_feature_select;
    volatile uint32_t guest_feature;
    volatile uint16_t msix_config;
    volatile uint16_t num_queues;
    volatile uint8_t device_status;
    volatile uint8_t config_generation;
    volatile uint16_t queue_select;
    volatile uint16_t queue_size;
    volatile uint16_t queue_msix_vector;
    volatile uint16_t queue_enable;
    volatile uint16_t queue_notify_off;
    volatile uint32_t queue_desc_lo;
    volatile uint32_t queue_desc_hi;
    volatile uint32_t queue_avail_lo;
    volatile uint32_t queue_avail_hi;
    volatile uint32_t queue_used_lo;
    volatile uint32_t queue_used_hi;
} __attribute__((packed));

struct virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct virtq_avail {
    volatile uint16_t flags;
    volatile uint16_t idx;
    volatile uint16_t ring[GPU_QUEUE_MAX];
    volatile uint16_t used_event;
} __attribute__((packed));

struct virtq_used_elem {
    volatile uint32_t id;
    volatile uint32_t len;
} __attribute__((packed));

struct virtq_used {
    volatile uint16_t flags;
    volatile uint16_t idx;
    struct virtq_used_elem ring[GPU_QUEUE_MAX];
    volatile uint16_t avail_event;
} __attribute__((packed));

struct virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx;
    uint8_t padding[3];
} __attribute__((packed));

struct virtio_gpu_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} __attribute__((packed));

struct virtio_gpu_resource_unref {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
} __attribute__((packed));

struct virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} __attribute__((packed));

struct virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
} __attribute__((packed));

struct virtio_gpu_attach_one {
    struct virtio_gpu_resource_attach_backing command;
    struct virtio_gpu_mem_entry entry;
} __attribute__((packed));

struct virtio_gpu_display_one {
    struct virtio_gpu_rect r;
    uint32_t enabled;
    uint32_t flags;
} __attribute__((packed));

struct virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_display_one modes[VIRTIO_GPU_MAX_SCANOUTS];
} __attribute__((packed));

static const struct pci_device *gpu_pci;
static volatile struct virtio_pci_common_cfg *gpu_common;
static volatile uint8_t *gpu_notify_base;
static volatile uint8_t *gpu_device_config;
static uint32_t gpu_notify_length;
static uint32_t gpu_notify_multiplier;
static volatile uint16_t *gpu_notify;

static struct virtq_desc *gpu_desc;
static struct virtq_avail *gpu_avail;
static struct virtq_used *gpu_used;
static uint8_t *gpu_request;
static uint8_t *gpu_response;
static uint16_t gpu_queue_size;
static uint16_t gpu_avail_index;

static uintptr_t gpu_backing_phys;
static uint32_t *gpu_backing;
static uint32_t gpu_width;
static uint32_t gpu_height;
static uint32_t gpu_resource_id;
static uint32_t gpu_next_resource_id = 1;
static uint32_t gpu_feature_low;
static int gpu_is_ready;

static void bytes_zero(void *pointer, uint32_t size) {
    uint8_t *bytes = (uint8_t *)pointer;
    for (uint32_t i = 0; i < size; i++)
        bytes[i] = 0;
}

static void bytes_copy(void *destination, const void *source, uint32_t size) {
    uint8_t *dst = (uint8_t *)destination;
    const uint8_t *src = (const uint8_t *)source;
    for (uint32_t i = 0; i < size; i++)
        dst[i] = src[i];
}

static uintptr_t gpu_bar_address(const struct pci_device *device,
                                 uint8_t index) {
    if (!device || index >= 6u)
        return 0;
    uint32_t bar = pci_bar(device, index);
    if (!bar || (bar & 1u))
        return 0;
    uint32_t type = (bar >> 1) & 3u;
    if (type == 2u) {
        if (index + 1u >= 6u || pci_bar(device, index + 1u) != 0)
            return 0;
    } else if (type == 3u) {
        return 0;
    }
    return (uintptr_t)(bar & ~0x0Fu);
}

static void *gpu_map_capability(const struct pci_device *device,
                                uint8_t capability, uint32_t length) {
    uint8_t bar_index = pci_config_read8(device, capability + 4u);
    uint32_t offset = pci_config_read32(device, capability + 8u);
    uintptr_t bar = gpu_bar_address(device, bar_index);
    if (!bar || !length || offset > 0xFFFFFFFFu - (uint32_t)bar)
        return 0;
    return paging_map_mmio(bar + offset, length);
}

static int gpu_find_capabilities(const struct pci_device *device) {
    if (!(pci_config_read16(device, 0x06u) & PCI_STATUS_CAP_LIST))
        return -1;
    uint8_t capability = pci_config_read8(device, PCI_CAP_PTR) & 0xFCu;
    for (uint32_t hop = 0; capability >= 0x40u && hop < 48u; hop++) {
        uint8_t id = pci_config_read8(device, capability);
        uint8_t next = pci_config_read8(device, capability + 1u) & 0xFCu;
        uint8_t cap_length = pci_config_read8(device, capability + 2u);
        if (id == PCI_CAP_ID_VENDOR && cap_length >= 16u) {
            uint8_t type = pci_config_read8(device, capability + 3u);
            uint32_t length = pci_config_read32(device, capability + 12u);
            if (type == VIRTIO_PCI_CAP_COMMON_CFG && !gpu_common) {
                if (length < sizeof(struct virtio_pci_common_cfg))
                    return -1;
                gpu_common = (volatile struct virtio_pci_common_cfg *)
                    gpu_map_capability(device, capability, length);
            } else if (type == VIRTIO_PCI_CAP_NOTIFY_CFG &&
                       !gpu_notify_base) {
                if (cap_length < 20u)
                    return -1;
                gpu_notify_base = (volatile uint8_t *)
                    gpu_map_capability(device, capability, length);
                gpu_notify_length = length;
                gpu_notify_multiplier =
                    pci_config_read32(device, capability + 16u);
            } else if (type == VIRTIO_PCI_CAP_ISR_CFG) {
                (void)gpu_map_capability(device, capability, length);
            } else if (type == VIRTIO_PCI_CAP_DEVICE_CFG &&
                       !gpu_device_config) {
                gpu_device_config = (volatile uint8_t *)
                    gpu_map_capability(device, capability, length);
            }
        }
        if (!next || next == capability)
            break;
        capability = next;
    }
    return gpu_common && gpu_notify_base ? 0 : -1;
}

static void gpu_status_add(uint8_t status) {
    gpu_common->device_status =
        (uint8_t)(gpu_common->device_status | status);
}

static void gpu_status_failed(void) {
    if (gpu_common)
        gpu_status_add(VIRTIO_STATUS_FAILED);
}

static int gpu_negotiate_features(void) {
    gpu_common->device_status = 0;
    for (uint32_t spin = 0; spin < 100000u; spin++) {
        if (gpu_common->device_status == 0)
            break;
        if (spin + 1u == 100000u)
            return -1;
        __asm__ volatile("pause");
    }
    gpu_status_add(VIRTIO_STATUS_ACKNOWLEDGE);
    gpu_status_add(VIRTIO_STATUS_DRIVER);

    gpu_common->device_feature_select = 1;
    uint32_t feature_high = gpu_common->device_feature;
    if (!(feature_high & VIRTIO_F_VERSION_1_HIGH))
        return -1;

    /* Device-specific features live in the low dword.  Log what the host
     * offers so the 3D/virgl path can be gated on real capability instead of
     * assumptions:  bit0 VIRGL, bit1 EDID, bit2 RESOURCE_UUID,
     * bit3 RESOURCE_BLOB, bit4 CONTEXT_INIT. */
    gpu_common->device_feature_select = 0;
    gpu_feature_low = gpu_common->device_feature;
    serial_puts("[gpu] device features lo=");
    serial_puthex(gpu_feature_low);
    serial_puts(" hi=");
    serial_puthex(feature_high);
    serial_puts(" virgl=");
    serial_puthex((gpu_feature_low & VIRTIO_GPU_F_VIRGL_BIT) ? 1u : 0u);
    serial_puts(" blob=");
    serial_puthex((gpu_feature_low & VIRTIO_GPU_F_RESOURCE_BLOB_BIT) ? 1u : 0u);
    serial_puts(" ctxinit=");
    serial_puthex((gpu_feature_low & VIRTIO_GPU_F_CONTEXT_INIT_BIT) ? 1u : 0u);
    serial_puts(" queues=");
    serial_puthex(gpu_common->num_queues);
    serial_puts("\n");

    gpu_common->guest_feature_select = 0;
    gpu_common->guest_feature =
        (gpu_feature_low & VIRTIO_GPU_F_VIRGL_BIT) ? VIRTIO_GPU_F_VIRGL_BIT : 0u;
    gpu_common->guest_feature_select = 1;
    gpu_common->guest_feature = VIRTIO_F_VERSION_1_HIGH;
    gpu_status_add(VIRTIO_STATUS_FEATURES_OK);
    if (!(gpu_common->device_status & VIRTIO_STATUS_FEATURES_OK))
        return -1;
    return 0;
}

static int gpu_setup_queue(void) {
    gpu_common->queue_select = GPU_QUEUE_INDEX;
    uint16_t offered = gpu_common->queue_size;
    if (gpu_common->queue_enable || offered < 2u)
        return -1;
    uint16_t chosen = GPU_QUEUE_MAX;
    while (chosen > offered)
        chosen >>= 1;
    if (chosen < 2u)
        return -1;

    uintptr_t desc_page = pmm_alloc_pages(1);
    uintptr_t avail_page = pmm_alloc_pages(1);
    uintptr_t used_page = pmm_alloc_pages(1);
    uintptr_t request_page = pmm_alloc_pages(1);
    uintptr_t response_page = pmm_alloc_pages(1);
    if (!desc_page || !avail_page || !used_page ||
        !request_page || !response_page)
        return -1;

    gpu_desc = (struct virtq_desc *)desc_page;
    gpu_avail = (struct virtq_avail *)avail_page;
    gpu_used = (struct virtq_used *)used_page;
    gpu_request = (uint8_t *)request_page;
    gpu_response = (uint8_t *)response_page;
    bytes_zero(gpu_desc, PAGE_SIZE);
    bytes_zero(gpu_avail, PAGE_SIZE);
    bytes_zero(gpu_used, PAGE_SIZE);
    bytes_zero(gpu_request, PAGE_SIZE);
    bytes_zero(gpu_response, PAGE_SIZE);
    gpu_avail->flags = VRING_AVAIL_F_NO_INTERRUPT;
    gpu_queue_size = chosen;
    gpu_avail_index = 0;

    gpu_common->queue_size = chosen;
    gpu_common->queue_msix_vector = GPU_QUEUE_NO_VECTOR;
    gpu_common->queue_desc_lo = (uint32_t)desc_page;
    gpu_common->queue_desc_hi = 0;
    gpu_common->queue_avail_lo = (uint32_t)avail_page;
    gpu_common->queue_avail_hi = 0;
    gpu_common->queue_used_lo = (uint32_t)used_page;
    gpu_common->queue_used_hi = 0;

    uint32_t notify_offset =
        (uint32_t)gpu_common->queue_notify_off * gpu_notify_multiplier;
    if (notify_offset > gpu_notify_length ||
        gpu_notify_length - notify_offset < sizeof(uint16_t))
        return -1;
    gpu_notify = (volatile uint16_t *)(gpu_notify_base + notify_offset);
    io_dma_wmb();
    gpu_common->queue_enable = 1;
    if (gpu_common->queue_enable != 1)
        return -1;
    return 0;
}

static int gpu_submit(const void *request, uint32_t request_size,
                      void *response, uint32_t response_size) {
    if (!request || request_size == 0 || request_size > PAGE_SIZE ||
        !response || response_size < sizeof(struct virtio_gpu_ctrl_hdr) ||
        response_size > PAGE_SIZE || !gpu_notify)
        return -1;

    /* Keep IRQ off only while publishing descriptors/notify. Spinning with
     * IRQs masked freezes the PIT and has been observed to leave boot in a
     * corrupted state after a failed GPU probe. */
    uint32_t irq_flags = irq_save();
    bytes_copy(gpu_request, request, request_size);
    bytes_zero(gpu_response, response_size);
    gpu_desc[0].addr = (uint64_t)(uintptr_t)gpu_request;
    gpu_desc[0].len = request_size;
    gpu_desc[0].flags = VRING_DESC_F_NEXT;
    gpu_desc[0].next = 1;
    gpu_desc[1].addr = (uint64_t)(uintptr_t)gpu_response;
    gpu_desc[1].len = response_size;
    gpu_desc[1].flags = VRING_DESC_F_WRITE;
    gpu_desc[1].next = 0;

    uint16_t used_before = gpu_used->idx;
    gpu_avail->ring[gpu_avail_index % gpu_queue_size] = 0;
    __sync_synchronize();
    gpu_avail_index++;
    gpu_avail->idx = gpu_avail_index;
    __sync_synchronize();
    *gpu_notify = GPU_QUEUE_INDEX;
    irq_restore(irq_flags);

    int complete = 0;
    for (uint32_t spin = 0; spin < 10000000u; spin++) {
        if (gpu_used->idx != used_before) {
            complete = 1;
            break;
        }
        __asm__ volatile("pause");
    }
    if (!complete) {
        serial_puts("[gpu] virtqueue command timeout\n");
        return -1;
    }
    __sync_synchronize();
    struct virtq_used_elem *used =
        &gpu_used->ring[used_before % gpu_queue_size];
    if (used->id != 0)
        return -1;
    bytes_copy(response, gpu_response, response_size);
    return 0;
}

static int gpu_submit_expect(const void *request, uint32_t request_size,
                             uint32_t expected_response) {
    struct virtio_gpu_ctrl_hdr response;
    if (gpu_submit(request, request_size, &response, sizeof(response)) < 0)
        return -1;
    if (response.type != expected_response) {
        serial_puts("[gpu] cmd response=");
        serial_puthex(response.type);
        serial_puts(" expected=");
        serial_puthex(expected_response);
        serial_puts("\n");
        return -1;
    }
    return 0;
}

/* ---- transport surface consumed by the virgl 3D layer ---- */

int vgpu_submit_expect(const void *request, uint32_t request_size,
                       uint32_t expected_response) {
    return gpu_submit_expect(request, request_size, expected_response);
}

/* 3D command streams routinely exceed one page, so they get a dedicated
 * multi-page request buffer.  Allocated on first use: hosts without virgl
 * never pay for it. */
enum { GPU_LARGE_REQUEST_PAGES = 16 }; /* 64 KiB */

static uint8_t *gpu_large_request;

uint32_t vgpu_large_request_capacity(void) {
    return (uint32_t)GPU_LARGE_REQUEST_PAGES * PAGE_SIZE;
}

int vgpu_submit_large(const void *request, uint32_t request_size,
                      uint32_t expected_response) {
    struct virtio_gpu_ctrl_hdr response;
    uint16_t used_before;
    int complete = 0;

    if (!request || request_size == 0 ||
        request_size > vgpu_large_request_capacity() || !gpu_notify)
        return -1;
    if (!gpu_large_request) {
        uintptr_t phys = pmm_alloc_pages(GPU_LARGE_REQUEST_PAGES);
        if (!phys)
            return -1;
        gpu_large_request = (uint8_t *)phys;
    }

    uint32_t irq_flags = irq_save();
    bytes_copy(gpu_large_request, request, request_size);
    bytes_zero(gpu_response, sizeof(response));
    gpu_desc[0].addr = (uint64_t)(uintptr_t)gpu_large_request;
    gpu_desc[0].len = request_size;
    gpu_desc[0].flags = VRING_DESC_F_NEXT;
    gpu_desc[0].next = 1;
    gpu_desc[1].addr = (uint64_t)(uintptr_t)gpu_response;
    gpu_desc[1].len = (uint32_t)sizeof(response);
    gpu_desc[1].flags = VRING_DESC_F_WRITE;
    gpu_desc[1].next = 0;

    used_before = gpu_used->idx;
    gpu_avail->ring[gpu_avail_index % gpu_queue_size] = 0;
    __sync_synchronize();
    gpu_avail_index++;
    gpu_avail->idx = gpu_avail_index;
    __sync_synchronize();
    *gpu_notify = GPU_QUEUE_INDEX;
    irq_restore(irq_flags);

    for (uint32_t spin = 0; spin < 10000000u; spin++) {
        if (gpu_used->idx != used_before) {
            complete = 1;
            break;
        }
        __asm__ volatile("pause");
    }
    if (!complete) {
        serial_puts("[gpu] 3D submit timeout\n");
        return -1;
    }
    __sync_synchronize();
    bytes_copy(&response, gpu_response, sizeof(response));
    if (response.type != expected_response) {
        serial_puts("[gpu] 3D submit response=");
        serial_puthex(response.type);
        serial_puts("\n");
        return -1;
    }
    return 0;
}

int vgpu_attach_backing(uint32_t resource_id, uintptr_t phys, uint32_t bytes) {
    struct virtio_gpu_attach_one attach = {0};
    attach.command.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach.command.resource_id = resource_id;
    attach.command.nr_entries = 1;
    attach.entry.addr = (uint64_t)phys;
    attach.entry.length = bytes;
    return gpu_submit_expect(&attach, sizeof(attach),
                             VIRTIO_GPU_RESP_OK_NODATA);
}

uint32_t vgpu_alloc_resource_id(void) {
    uint32_t id = gpu_next_resource_id++;
    if (!id)
        id = gpu_next_resource_id++;
    return id;
}

int vgpu_virgl_offered(void) {
    return (gpu_feature_low & VIRTIO_GPU_F_VIRGL_BIT) ? 1 : 0;
}

uint32_t vgpu_scanout_resource(void) { return gpu_resource_id; }
uint32_t vgpu_scanout_width(void) { return gpu_width; }
uint32_t vgpu_scanout_height(void) { return gpu_height; }

int vgpu_set_scanout(uint32_t resource_id, uint32_t width, uint32_t height) {
    struct virtio_gpu_set_scanout scanout = {0};
    scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    scanout.r.width = width;
    scanout.r.height = height;
    scanout.scanout_id = 0;
    scanout.resource_id = resource_id;
    return gpu_submit_expect(&scanout, sizeof(scanout),
                             VIRTIO_GPU_RESP_OK_NODATA);
}

int vgpu_flush_resource(uint32_t resource_id, int x, int y,
                        int width, int height) {
    struct virtio_gpu_resource_flush flush = {0};
    flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush.r.x = (uint32_t)x;
    flush.r.y = (uint32_t)y;
    flush.r.width = (uint32_t)width;
    flush.r.height = (uint32_t)height;
    flush.resource_id = resource_id;
    return gpu_submit_expect(&flush, sizeof(flush),
                             VIRTIO_GPU_RESP_OK_NODATA);
}

static int gpu_get_display_info(void) {
    struct virtio_gpu_ctrl_hdr request = {0};
    struct virtio_gpu_resp_display_info response;
    request.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    if (gpu_submit(&request, sizeof(request),
                   &response, sizeof(response)) < 0 ||
        response.hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO)
        return -1;
    serial_puts("[gpu] scanout0 enabled=");
    serial_puthex(response.modes[0].enabled);
    serial_puts(" preferred=");
    serial_puthex(response.modes[0].r.width);
    serial_puts("x");
    serial_puthex(response.modes[0].r.height);
    serial_puts("\n");
    return 0;
}

static int gpu_unref_resource(uint32_t resource_id) {
    if (!resource_id)
        return 0;
    struct virtio_gpu_resource_unref request = {0};
    request.hdr.type = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    request.resource_id = resource_id;
    return gpu_submit_expect(&request, sizeof(request),
                             VIRTIO_GPU_RESP_OK_NODATA);
}

static int gpu_transfer_flush_resource(uint32_t resource_id,
                                       uint32_t resource_width,
                                       int x, int y, int width, int height) {
    struct virtio_gpu_transfer_to_host_2d transfer = {0};
    transfer.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    transfer.r.x = (uint32_t)x;
    transfer.r.y = (uint32_t)y;
    transfer.r.width = (uint32_t)width;
    transfer.r.height = (uint32_t)height;
    transfer.offset = ((uint64_t)(uint32_t)y * resource_width +
                       (uint32_t)x) * 4u;
    transfer.resource_id = resource_id;
    /* Full barrier is enough on i386; avoid assuming SSE sfence availability
     * in every build flag combination. */
    __sync_synchronize();
    if (gpu_submit_expect(&transfer, sizeof(transfer),
                          VIRTIO_GPU_RESP_OK_NODATA) < 0)
        return -1;

    struct virtio_gpu_resource_flush flush = {0};
    flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush.r = transfer.r;
    flush.resource_id = resource_id;
    return gpu_submit_expect(&flush, sizeof(flush),
                             VIRTIO_GPU_RESP_OK_NODATA);
}

static int gpu_create_scanout(uint32_t width, uint32_t height) {
    if (!gpu_backing || !width || !height ||
        width > GPU_MAX_WIDTH || height > GPU_MAX_HEIGHT)
        return -1;
    uint64_t bytes = (uint64_t)width * height * 4u;
    if (bytes > GPU_BACKING_BYTES)
        return -1;

    uint32_t new_resource = gpu_next_resource_id++;
    if (!new_resource)
        new_resource = gpu_next_resource_id++;

    struct virtio_gpu_resource_create_2d create = {0};
    create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create.resource_id = new_resource;
    create.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    create.width = width;
    create.height = height;
    if (gpu_submit_expect(&create, sizeof(create),
                          VIRTIO_GPU_RESP_OK_NODATA) < 0) {
        serial_puts("[gpu] RESOURCE_CREATE_2D failed\n");
        return -1;
    }

    /* Page-align DMA length (QEMU maps whole pages) but do not exceed the
     * allocated backing slab.  Also avoid overshooting the 2D resource size
     * by more than a page of padding when possible. */
    uint32_t attach_bytes =
        (uint32_t)((bytes + (PAGE_SIZE - 1u)) & ~((uint64_t)PAGE_SIZE - 1u));
    if (attach_bytes == 0 || attach_bytes > GPU_BACKING_BYTES)
        attach_bytes = (uint32_t)bytes;
    if (attach_bytes < (uint32_t)bytes)
        attach_bytes = (uint32_t)bytes;

    struct virtio_gpu_attach_one attach = {0};
    attach.command.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach.command.resource_id = new_resource;
    attach.command.nr_entries = 1;
    attach.entry.addr = (uint64_t)gpu_backing_phys;
    attach.entry.length = attach_bytes;
    if (gpu_submit_expect(&attach, sizeof(attach),
                          VIRTIO_GPU_RESP_OK_NODATA) < 0) {
        serial_puts("[gpu] RESOURCE_ATTACH_BACKING failed phys=");
        serial_puthex((uint32_t)gpu_backing_phys);
        serial_puts(" len=");
        serial_puthex(attach_bytes);
        serial_puts("\n");
        (void)gpu_unref_resource(new_resource);
        return -1;
    }

    struct virtio_gpu_set_scanout scanout = {0};
    scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    scanout.r.x = 0;
    scanout.r.y = 0;
    scanout.r.width = width;
    scanout.r.height = height;
    scanout.scanout_id = 0;
    scanout.resource_id = new_resource;
    if (gpu_submit_expect(&scanout, sizeof(scanout),
                          VIRTIO_GPU_RESP_OK_NODATA) < 0) {
        serial_puts("[gpu] SET_SCANOUT failed\n");
        (void)gpu_unref_resource(new_resource);
        return -1;
    }

    bytes_zero(gpu_backing, (uint32_t)bytes);
    if (gpu_transfer_flush_resource(new_resource, width, 0, 0,
                                    (int)width, (int)height) < 0) {
        serial_puts("[gpu] TRANSFER/FLUSH failed\n");
        (void)gpu_unref_resource(new_resource);
        return -1;
    }

    uint32_t old_resource = gpu_resource_id;
    gpu_resource_id = new_resource;
    gpu_width = width;
    gpu_height = height;
    if (old_resource)
        (void)gpu_unref_resource(old_resource);
    return 0;
}

int virtio_gpu_init(uint32_t width, uint32_t height) {
    if (gpu_is_ready)
        return 0;
    gpu_pci = pci_find_device(VIRTIO_VENDOR_ID, VIRTIO_GPU_DEVICE_ID);
    if (!gpu_pci)
        return -1;

    serial_puts("[gpu] virtio-gpu PCI ");
    serial_puthex(gpu_pci->bus);
    serial_putc(':');
    serial_puthex(gpu_pci->device);
    serial_putc('.');
    serial_puthex(gpu_pci->function);
    serial_puts(" modern transport\n");
    pci_enable_device(gpu_pci, 0, 1, 1);
    pci_disable_intx(gpu_pci);

    if (gpu_find_capabilities(gpu_pci) < 0 ||
        gpu_negotiate_features() < 0 ||
        gpu_setup_queue() < 0) {
        gpu_status_failed();
        serial_puts("[gpu] virtio PCI setup failed\n");
        return -1;
    }
    gpu_status_add(VIRTIO_STATUS_DRIVER_OK);
    if (gpu_get_display_info() < 0) {
        gpu_status_failed();
        serial_puts("[gpu] display-info command failed\n");
        return -1;
    }

    uint32_t backing_pages =
        (GPU_BACKING_BYTES + PAGE_SIZE - 1u) / PAGE_SIZE;
    /* Avoid guest GPAs in the first 1 MiB: some hosts refuse virtio DMA maps
     * into the real-mode / BIOS hole (logs showed phys=0x23000). */
    gpu_backing_phys = 0;
    for (int attempt = 0; attempt < 16 && !gpu_backing_phys; attempt++) {
        uintptr_t phys = pmm_alloc_pages(backing_pages);
        if (!phys)
            break;
        if (phys >= 0x100000u) {
            gpu_backing_phys = phys;
            break;
        }
        serial_puts("[gpu] skip low backing ");
        serial_puthex((uint32_t)phys);
        serial_puts("\n");
        pmm_free_pages(phys, backing_pages);
    }
    if (!gpu_backing_phys) {
        serial_puts("[gpu] scanout backing allocation failed\n");
        return -1;
    }
    gpu_backing = (uint32_t *)gpu_backing_phys;
    serial_puts("[gpu] backing phys=");
    serial_puthex((uint32_t)gpu_backing_phys);
    serial_puts("\n");
    if (gpu_create_scanout(width, height) < 0) {
        serial_puts("[gpu] 2D resource setup failed; falling back to boot FB\n");
        /* Soft-fail: do not mark the device FAILED (that can wedge later MMIO
         * on some QEMU builds). Free the unused backing and keep gpu_is_ready=0
         * so fb.c continues on the Limine linear framebuffer. */
        pmm_free_pages(gpu_backing_phys, backing_pages);
        gpu_backing = 0;
        gpu_backing_phys = 0;
        gpu_resource_id = 0;
        return -1;
    }
    gpu_is_ready = 1;
    serial_puts("[gpu] virtio-gpu 2D scanout ready ");
    serial_puthex(width);
    serial_putc('x');
    serial_puthex(height);
    serial_puts(" damage uploads enabled\n");
    /* Optional: bring up the host GL pipeline.  Failure is not fatal -- the
     * software compositor keeps using the 2D scanout resource. */
    (void)virtio_gpu_3d_init();
    return 0;
}

int virtio_gpu_ready(void) {
    return gpu_is_ready;
}

int virtio_gpu_set_mode(uint32_t width, uint32_t height) {
    if (!gpu_is_ready || !width || !height ||
        width > GPU_MAX_WIDTH || height > GPU_MAX_HEIGHT)
        return -1;
    if (width == gpu_width && height == gpu_height)
        return 0;
    if (gpu_create_scanout(width, height) < 0)
        return -1;
    serial_puts("[gpu] scanout mode ");
    serial_puthex(width);
    serial_putc('x');
    serial_puthex(height);
    serial_puts("\n");
    return 0;
}

uint32_t *virtio_gpu_pixels(void) {
    return gpu_is_ready ? gpu_backing : 0;
}

uint32_t virtio_gpu_stride(void) {
    return gpu_is_ready ? gpu_width : 0;
}

uintptr_t virtio_gpu_backing_phys(void) {
    return gpu_is_ready ? gpu_backing_phys : 0;
}

uint32_t virtio_gpu_backing_bytes(void) {
    if (!gpu_is_ready || !gpu_width || !gpu_height)
        return 0;
    return gpu_width * gpu_height * 4u;
}

int virtio_gpu_flush(int x, int y, int width, int height) {
    if (!gpu_is_ready || x < 0 || y < 0 || width <= 0 || height <= 0 ||
        x >= (int)gpu_width || y >= (int)gpu_height)
        return -1;
    if (width > (int)gpu_width - x)
        width = (int)gpu_width - x;
    if (height > (int)gpu_height - y)
        height = (int)gpu_height - y;
    return gpu_transfer_flush_resource(gpu_resource_id, gpu_width,
                                       x, y, width, height);
}
