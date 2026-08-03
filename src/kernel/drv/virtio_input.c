#include <stdint.h>
#include "fb.h"
#include "irq.h"
#include "mouse.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "serial.h"
#include "virtio_input.h"

enum {
    VIRTIO_VENDOR_ID = 0x1AF4,
    /* Modern PCI device id = 0x1040 + VirtIO input device id (18). */
    VIRTIO_INPUT_DEVICE_ID = 0x1052,

    PCI_STATUS_CAP_LIST = 0x0010,
    PCI_CAP_ID_VENDOR = 0x09,
    PCI_CAP_PTR = 0x34,
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
    VIRTIO_NO_VECTOR = 0xFFFF,

    VIRTIO_INPUT_EVENTQ = 0,
    VIRTIO_INPUT_QUEUE_MAX = 64,
    VRING_DESC_F_WRITE = 2,

    VIRTIO_INPUT_CFG_EV_BITS = 0x11,
    VIRTIO_INPUT_CFG_ABS_INFO = 0x12,

    EV_SYN = 0x00,
    EV_KEY = 0x01,
    EV_REL = 0x02,
    EV_ABS = 0x03,
    SYN_REPORT = 0x00,
    REL_WHEEL = 0x08,
    ABS_X = 0x00,
    ABS_Y = 0x01,
    BTN_LEFT = 0x110,
    BTN_RIGHT = 0x111,
    BTN_MIDDLE = 0x112,
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
    volatile uint16_t ring[VIRTIO_INPUT_QUEUE_MAX];
    volatile uint16_t used_event;
} __attribute__((packed));

struct virtq_used_elem {
    volatile uint32_t id;
    volatile uint32_t len;
} __attribute__((packed));

struct virtq_used {
    volatile uint16_t flags;
    volatile uint16_t idx;
    struct virtq_used_elem ring[VIRTIO_INPUT_QUEUE_MAX];
    volatile uint16_t avail_event;
} __attribute__((packed));

struct virtio_input_absinfo {
    int32_t min;
    int32_t max;
    uint32_t fuzz;
    uint32_t flat;
    uint32_t res;
} __attribute__((packed));

struct virtio_input_devids {
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
} __attribute__((packed));

struct virtio_input_config {
    volatile uint8_t select;
    volatile uint8_t subsel;
    volatile uint8_t size;
    volatile uint8_t reserved[5];
    union {
        volatile char string[128];
        volatile uint8_t bitmap[128];
        volatile struct virtio_input_absinfo abs;
        volatile struct virtio_input_devids ids;
    } u;
} __attribute__((packed));

struct virtio_input_event {
    uint16_t type;
    uint16_t code;
    int32_t value;
} __attribute__((packed));

static const struct pci_device *input_pci;
static volatile struct virtio_pci_common_cfg *input_common;
static volatile struct virtio_input_config *input_config;
static volatile uint8_t *input_isr;
static volatile uint8_t *input_notify_base;
static volatile uint16_t *input_event_notify;
static uint32_t input_notify_length;
static uint32_t input_notify_multiplier;

static struct virtq_desc *input_desc;
static struct virtq_avail *input_avail;
static struct virtq_used *input_used;
static struct virtio_input_event *input_events;
static uint16_t input_queue_size;
static uint16_t input_avail_index;
static uint16_t input_used_index;
static int input_ready;

static int32_t input_abs_x_min;
static int32_t input_abs_x_max;
static int32_t input_abs_y_min;
static int32_t input_abs_y_max;
static int32_t input_raw_x;
static int32_t input_raw_y;
static int input_buttons;
static int input_wheel_pending;
static int input_report_pending;

static void input_bytes_zero(void *pointer, uint32_t size) {
    uint8_t *bytes = (uint8_t *)pointer;
    for (uint32_t i = 0; i < size; i++)
        bytes[i] = 0;
}

static uintptr_t input_bar_address(const struct pci_device *device,
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

static void *input_map_capability(const struct pci_device *device,
                                  uint8_t capability, uint32_t length) {
    uint8_t bar_index = pci_config_read8(device, capability + 4u);
    uint32_t offset = pci_config_read32(device, capability + 8u);
    uintptr_t bar = input_bar_address(device, bar_index);
    if (!bar || !length || offset > 0xFFFFFFFFu - (uint32_t)bar)
        return 0;
    return paging_map_mmio(bar + offset, length);
}

static int input_find_capabilities(const struct pci_device *device) {
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
            if (type == VIRTIO_PCI_CAP_COMMON_CFG && !input_common) {
                if (length < sizeof(struct virtio_pci_common_cfg))
                    return -1;
                input_common = (volatile struct virtio_pci_common_cfg *)
                    input_map_capability(device, capability, length);
            } else if (type == VIRTIO_PCI_CAP_NOTIFY_CFG &&
                       !input_notify_base) {
                if (cap_length < 20u)
                    return -1;
                input_notify_base = (volatile uint8_t *)
                    input_map_capability(device, capability, length);
                input_notify_length = length;
                input_notify_multiplier =
                    pci_config_read32(device, capability + 16u);
            } else if (type == VIRTIO_PCI_CAP_ISR_CFG && !input_isr) {
                input_isr = (volatile uint8_t *)
                    input_map_capability(device, capability, length);
            } else if (type == VIRTIO_PCI_CAP_DEVICE_CFG && !input_config) {
                if (length < sizeof(struct virtio_input_config))
                    return -1;
                input_config = (volatile struct virtio_input_config *)
                    input_map_capability(device, capability, length);
            }
        }
        if (!next || next == capability)
            break;
        capability = next;
    }
    return input_common && input_notify_base && input_isr && input_config
        ? 0 : -1;
}

static void input_status_add(uint8_t status) {
    input_common->device_status =
        (uint8_t)(input_common->device_status | status);
}

static void input_status_failed(void) {
    if (input_common)
        input_status_add(VIRTIO_STATUS_FAILED);
}

static int input_negotiate_features(void) {
    input_common->device_status = 0;
    for (uint32_t spin = 0; spin < 100000u; spin++) {
        if (input_common->device_status == 0)
            break;
        if (spin + 1u == 100000u)
            return -1;
        __asm__ volatile("pause");
    }
    input_status_add(VIRTIO_STATUS_ACKNOWLEDGE);
    input_status_add(VIRTIO_STATUS_DRIVER);

    input_common->device_feature_select = 1;
    if (!(input_common->device_feature & VIRTIO_F_VERSION_1_HIGH))
        return -1;
    /* VirtIO input defines no device-specific feature bits. */
    input_common->guest_feature_select = 0;
    input_common->guest_feature = 0;
    input_common->guest_feature_select = 1;
    input_common->guest_feature = VIRTIO_F_VERSION_1_HIGH;
    input_status_add(VIRTIO_STATUS_FEATURES_OK);
    return (input_common->device_status & VIRTIO_STATUS_FEATURES_OK) ? 0 : -1;
}

static int input_config_has_event(uint8_t event_type, uint16_t code) {
    uint32_t byte = code >> 3;
    uint8_t bit = (uint8_t)(1u << (code & 7u));
    for (int attempt = 0; attempt < 8; attempt++) {
        uint8_t before = input_common->config_generation;
        input_config->select = VIRTIO_INPUT_CFG_EV_BITS;
        input_config->subsel = event_type;
        __sync_synchronize();
        uint8_t size = input_config->size;
        uint8_t value = byte < size ? input_config->u.bitmap[byte] : 0;
        __sync_synchronize();
        if (before == input_common->config_generation)
            return (value & bit) != 0;
    }
    return 0;
}

static int input_read_abs_info(uint8_t axis, int32_t *min_out,
                               int32_t *max_out) {
    for (int attempt = 0; attempt < 8; attempt++) {
        uint8_t before = input_common->config_generation;
        input_config->select = VIRTIO_INPUT_CFG_ABS_INFO;
        input_config->subsel = axis;
        __sync_synchronize();
        uint8_t size = input_config->size;
        int32_t minimum = input_config->u.abs.min;
        int32_t maximum = input_config->u.abs.max;
        __sync_synchronize();
        if (before != input_common->config_generation)
            continue;
        if (size < sizeof(struct virtio_input_absinfo) || maximum <= minimum)
            return -1;
        *min_out = minimum;
        *max_out = maximum;
        return 0;
    }
    return -1;
}

static int input_read_axis_ranges(void) {
    if (!input_config_has_event(EV_ABS, ABS_X) ||
        !input_config_has_event(EV_ABS, ABS_Y) ||
        input_read_abs_info(ABS_X, &input_abs_x_min, &input_abs_x_max) < 0 ||
        input_read_abs_info(ABS_Y, &input_abs_y_min, &input_abs_y_max) < 0)
        return -1;
    input_raw_x = input_abs_x_min +
        (int32_t)(((uint32_t)input_abs_x_max -
                   (uint32_t)input_abs_x_min) >> 1);
    input_raw_y = input_abs_y_min +
        (int32_t)(((uint32_t)input_abs_y_max -
                   (uint32_t)input_abs_y_min) >> 1);
    return 0;
}

/* Freestanding i386 does not link __udivdi3.  Axis scaling can need a
 * 64-bit numerator, so use a small binary divider instead of C's 64-bit '/'. */
static uint32_t input_div_u64_u32(uint64_t numerator, uint32_t divisor) {
    uint64_t remainder = 0;
    uint32_t quotient = 0;
    if (!divisor)
        return 0;
    for (int bit = 63; bit >= 0; bit--) {
        remainder = (remainder << 1) | ((numerator >> bit) & 1u);
        if (remainder >= divisor) {
            remainder -= divisor;
            if (bit < 32)
                quotient |= 1u << bit;
        }
    }
    return quotient;
}

static int input_scale_axis(int32_t value, int32_t minimum, int32_t maximum,
                            int pixels) {
    if (pixels <= 1 || maximum <= minimum)
        return 0;
    if (value < minimum)
        value = minimum;
    if (value > maximum)
        value = maximum;
    uint32_t position = (uint32_t)value - (uint32_t)minimum;
    uint32_t range = (uint32_t)maximum - (uint32_t)minimum;
    uint64_t numerator = (uint64_t)position * (uint32_t)(pixels - 1);
    uint32_t scaled = input_div_u64_u32(numerator, range);
    return scaled < (uint32_t)pixels ? (int)scaled : pixels - 1;
}

static void input_commit_report(void) {
    struct gfx_info info;
    fb_get_info(&info);
    int width = info.width ? (int)info.width : 1;
    int height = info.height ? (int)info.height : 1;
    int x = input_scale_axis(input_raw_x, input_abs_x_min,
                             input_abs_x_max, width);
    int y = input_scale_axis(input_raw_y, input_abs_y_min,
                             input_abs_y_max, height);
    mouse_absolute_event(x, y, input_buttons, input_wheel_pending);
    input_wheel_pending = 0;
    input_report_pending = 0;
}

static void input_process_event(const struct virtio_input_event *event) {
    if (event->type == EV_ABS) {
        if (event->code == ABS_X) {
            input_raw_x = event->value;
            input_report_pending = 1;
        } else if (event->code == ABS_Y) {
            input_raw_y = event->value;
            input_report_pending = 1;
        }
    } else if (event->type == EV_KEY) {
        int bit = 0;
        if (event->code == BTN_LEFT)
            bit = 1;
        else if (event->code == BTN_RIGHT)
            bit = 2;
        else if (event->code == BTN_MIDDLE)
            bit = 4;
        if (bit) {
            if (event->value)
                input_buttons |= bit;
            else
                input_buttons &= ~bit;
            input_report_pending = 1;
        }
    } else if (event->type == EV_REL && event->code == REL_WHEEL) {
        input_wheel_pending += event->value;
        input_report_pending = 1;
    } else if (event->type == EV_SYN && event->code == SYN_REPORT &&
               input_report_pending) {
        input_commit_report();
    }
}

static void input_kick_eventq(void) {
    __sync_synchronize();
    *input_event_notify = VIRTIO_INPUT_EVENTQ;
}

static int input_interrupt(void *context) {
    (void)context;
    uint8_t isr = *input_isr; /* Read-to-clear, which deasserts PCI INTx. */
    if (!(isr & 1u))
        return isr != 0;
    if (!input_ready)
        return 1;

    int recycled = 0;
    __sync_synchronize();
    while (input_used_index != input_used->idx) {
        struct virtq_used_elem *used =
            &input_used->ring[input_used_index % input_queue_size];
        uint32_t id = used->id;
        uint32_t len = used->len;
        __sync_synchronize();
        if (id < input_queue_size) {
            if (len >= sizeof(struct virtio_input_event))
                input_process_event(&input_events[id]);
            input_avail->ring[input_avail_index % input_queue_size] =
                (uint16_t)id;
            input_avail_index++;
            recycled = 1;
        }
        input_used_index++;
    }
    if (recycled) {
        __sync_synchronize();
        input_avail->idx = input_avail_index;
        input_kick_eventq();
    }
    return 1;
}

static int input_setup_eventq(void) {
    if (input_common->num_queues <= VIRTIO_INPUT_EVENTQ)
        return -1;
    input_common->queue_select = VIRTIO_INPUT_EVENTQ;
    uint16_t offered = input_common->queue_size;
    if (input_common->queue_enable || offered < 2u)
        return -1;
    uint16_t chosen = VIRTIO_INPUT_QUEUE_MAX;
    while (chosen > offered)
        chosen >>= 1;
    if (chosen < 2u)
        return -1;

    uintptr_t desc_page = pmm_alloc_pages(1);
    uintptr_t avail_page = pmm_alloc_pages(1);
    uintptr_t used_page = pmm_alloc_pages(1);
    uintptr_t event_page = pmm_alloc_pages(1);
    if (!desc_page || !avail_page || !used_page || !event_page)
        return -1;

    input_desc = (struct virtq_desc *)desc_page;
    input_avail = (struct virtq_avail *)avail_page;
    input_used = (struct virtq_used *)used_page;
    input_events = (struct virtio_input_event *)event_page;
    input_bytes_zero(input_desc, PAGE_SIZE);
    input_bytes_zero(input_avail, PAGE_SIZE);
    input_bytes_zero(input_used, PAGE_SIZE);
    input_bytes_zero(input_events, PAGE_SIZE);
    input_queue_size = chosen;
    input_avail_index = chosen;
    input_used_index = 0;

    for (uint16_t i = 0; i < chosen; i++) {
        input_desc[i].addr =
            (uint64_t)(event_page + i * sizeof(struct virtio_input_event));
        input_desc[i].len = sizeof(struct virtio_input_event);
        input_desc[i].flags = VRING_DESC_F_WRITE;
        input_avail->ring[i] = i;
    }
    input_avail->idx = chosen;

    input_common->queue_size = chosen;
    input_common->queue_msix_vector = VIRTIO_NO_VECTOR;
    input_common->queue_desc_lo = (uint32_t)desc_page;
    input_common->queue_desc_hi = 0;
    input_common->queue_avail_lo = (uint32_t)avail_page;
    input_common->queue_avail_hi = 0;
    input_common->queue_used_lo = (uint32_t)used_page;
    input_common->queue_used_hi = 0;

    uint32_t notify_offset =
        (uint32_t)input_common->queue_notify_off * input_notify_multiplier;
    if (notify_offset > input_notify_length ||
        input_notify_length - notify_offset < sizeof(uint16_t))
        return -1;
    input_event_notify =
        (volatile uint16_t *)(input_notify_base + notify_offset);
    __sync_synchronize();
    input_common->queue_enable = 1;
    return input_common->queue_enable == 1 ? 0 : -1;
}

int virtio_input_init(void) {
    if (input_ready)
        return 0;
    input_pci = pci_find_device(VIRTIO_VENDOR_ID, VIRTIO_INPUT_DEVICE_ID);
    if (!input_pci)
        return -1;

    serial_puts("[input] virtio tablet PCI ");
    serial_puthex(input_pci->bus);
    serial_putc(':');
    serial_puthex(input_pci->device);
    serial_putc('.');
    serial_puthex(input_pci->function);
    serial_puts(" modern transport\n");

    pci_enable_device(input_pci, 0, 1, 1);
    pci_disable_intx(input_pci);
    if (input_find_capabilities(input_pci) < 0 ||
        input_negotiate_features() < 0 ||
        input_read_axis_ranges() < 0 ||
        input_setup_eventq() < 0 ||
        irq_register_handler(input_pci->irq_line, input_interrupt, 0, 1) < 0 ||
        pci_enable_intx(input_pci) < 0) {
        input_status_failed();
        pci_disable_intx(input_pci);
        serial_puts("[input] virtio tablet setup failed; PS/2 fallback\n");
        return -1;
    }

    input_buttons = 0;
    input_wheel_pending = 0;
    input_report_pending = 0;
    mouse_set_absolute_mode(1);
    input_ready = 1;
    input_status_add(VIRTIO_STATUS_DRIVER_OK);
    input_kick_eventq();
    serial_puts("[input] virtio tablet absolute pointer ready\n");
    return 0;
}
