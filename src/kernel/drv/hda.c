#include "hda.h"
#include "io.h"
#include "irq.h"
#include "paging.h"
#include "pci.h"
#include "pmm.h"
#include "serial.h"

/*
 * Intel High Definition Audio output driver.
 *
 * BuzzOS applications submit unsigned 8-bit mono PCM at 11025, 22050, or
 * 44100 Hz.  HDA transports a standard 48000 Hz, signed 16-bit stereo stream.
 * The conversion is kept here so every application uses the same queue and
 * timing rules.
 */
enum {
    HDA_OUTPUT_RATE = 48000,
    HDA_PERIOD_FRAMES = 1024,
    HDA_PERIODS = 8, /* allocation limit; the active ring is latency-driven */
    HDA_MIN_PERIODS = 2,
    HDA_PERIOD_BYTES = HDA_PERIOD_FRAMES * 4,
    HDA_FIFO_BYTES = 32768,
    HDA_DEFAULT_LATENCY_MS = 80,
    HDA_MAX_LATENCY_MS = 250,
    HDA_STREAM_TAG = 1,
    HDA_STREAM_FORMAT = 0x0011, /* 48 kHz, 16-bit, two channels */

    HDA_GCAP = 0x00,
    HDA_GCTL = 0x08,
    HDA_STATESTS = 0x0E,
    HDA_INTCTL = 0x20,
    HDA_INTSTS = 0x24,
    HDA_CORBLBASE = 0x40,
    HDA_CORBUBASE = 0x44,
    HDA_CORBWP = 0x48,
    HDA_CORBRP = 0x4A,
    HDA_CORBCTL = 0x4C,
    HDA_CORBSTS = 0x4D,
    HDA_CORBSIZE = 0x4E,
    HDA_RIRBLBASE = 0x50,
    HDA_RIRBUBASE = 0x54,
    HDA_RIRBWP = 0x58,
    HDA_RINTCNT = 0x5A,
    HDA_RIRBCTL = 0x5C,
    HDA_RIRBSTS = 0x5D,
    HDA_RIRBSIZE = 0x5E,
    HDA_ICOI = 0x60,
    HDA_ICII = 0x64,
    HDA_ICIS = 0x68,
    HDA_DPLBASE = 0x70,
    HDA_DPUBASE = 0x74,
    HDA_STREAM_BASE = 0x80,
    HDA_STREAM_STRIDE = 0x20,

    HDA_SD_CTL = 0x00,
    HDA_SD_STS = 0x03,
    HDA_SD_LPIB = 0x04,
    HDA_SD_CBL = 0x08,
    HDA_SD_LVI = 0x0C,
    HDA_SD_FMT = 0x12,
    HDA_SD_BDPL = 0x18,
    HDA_SD_BDPU = 0x1C,

    HDA_SD_SRST = 0x01,
    HDA_SD_RUN = 0x02,
    HDA_SD_IOCE = 0x04,
    HDA_SD_STATUS_MASK = 0x1C,

    HDA_PARAM_NODE_COUNT = 0x04,
    HDA_PARAM_FUNCTION_TYPE = 0x05,
    HDA_PARAM_WIDGET_CAPS = 0x09,
    HDA_PARAM_PIN_CAPS = 0x0C,
    HDA_PARAM_INPUT_AMP_CAPS = 0x0D,
    HDA_PARAM_CONNECTION_LIST = 0x0E,
    HDA_PARAM_OUTPUT_AMP_CAPS = 0x12,

    HDA_WIDGET_AUDIO_OUTPUT = 0,
    HDA_WIDGET_AUDIO_MIXER = 2,
    HDA_WIDGET_AUDIO_SELECTOR = 3,
    HDA_WIDGET_PIN = 4,

    HDA_MAX_PATH = 16,
    HDA_MAX_CONNECTIONS = 32,
};

struct hda_bdl_entry {
    uint32_t address_low;
    uint32_t address_high;
    uint32_t length;
    uint32_t flags;
} __attribute__((packed));

struct hda_rirb_entry {
    uint32_t response;
    uint32_t response_ex;
} __attribute__((packed));

static volatile uint8_t *hda_mmio;
static const struct pci_device *hda_pci;
static struct hda_bdl_entry *hda_bdl;
static uint32_t *hda_corb;
static struct hda_rirb_entry *hda_rirb;
static uint32_t *hda_position_buffer;
static int16_t *hda_buffers[HDA_PERIODS];
static uint8_t hda_fifo[HDA_FIFO_BYTES];
static uint16_t hda_period_sources[HDA_PERIODS];
static uint8_t hda_codec;
static uint8_t hda_afg;
static uint8_t hda_dac;
static uint8_t hda_pin;
static uint8_t hda_stream_index;
static uint8_t hda_next_refill;
static uint16_t hda_corb_entries;
static uint16_t hda_rirb_entries;
static uint16_t hda_rirb_read;
static uint32_t hda_fifo_read;
static uint32_t hda_fifo_write;
static uint32_t hda_fifo_count;
static uint32_t hda_dma_source_samples;
static uint32_t hda_input_rate = 11025;
static uint32_t hda_phase;
static uint32_t hda_last_position;
static uint32_t hda_queue_limit = 882; /* 80 ms at the initial 11025 Hz */
static int16_t hda_current_sample;
static uint8_t hda_active_periods = 3; /* 64 ms of 48 kHz DMA */
static int hda_ready;
static int hda_playing;
static int hda_single_command;

static uint8_t hda_path[HDA_MAX_PATH];
static uint8_t hda_path_connection[HDA_MAX_PATH];
static uint8_t hda_path_length;

static inline uint8_t mmio_read8(uint32_t reg) {
    return *(volatile uint8_t *)(hda_mmio + reg);
}

static inline uint16_t mmio_read16(uint32_t reg) {
    return *(volatile uint16_t *)(hda_mmio + reg);
}

static inline uint32_t mmio_read32(uint32_t reg) {
    return *(volatile uint32_t *)(hda_mmio + reg);
}

static inline void mmio_write8(uint32_t reg, uint8_t value) {
    *(volatile uint8_t *)(hda_mmio + reg) = value;
}

static inline void mmio_write16(uint32_t reg, uint16_t value) {
    *(volatile uint16_t *)(hda_mmio + reg) = value;
}

static inline void mmio_write32(uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(hda_mmio + reg) = value;
}

static int wait8(uint32_t reg, uint8_t mask, uint8_t value) {
    for (uint32_t spin = 0; spin < 1000000u; spin++) {
        if ((mmio_read8(reg) & mask) == value)
            return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

static int wait32(uint32_t reg, uint32_t mask, uint32_t value) {
    for (uint32_t spin = 0; spin < 1000000u; spin++) {
        if ((mmio_read32(reg) & mask) == value)
            return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

static uint16_t ring_entries(uint8_t capability, uint8_t size_reg) {
    if (capability & 0x40u) {
        mmio_write8(size_reg, 2u);
        return 256u;
    }
    if (capability & 0x20u) {
        mmio_write8(size_reg, 1u);
        return 16u;
    }
    if (capability & 0x10u) {
        mmio_write8(size_reg, 0u);
        return 2u;
    }
    return 0;
}

static int hda_controller_reset(void) {
    mmio_write32(HDA_INTCTL, 0);
    mmio_write8(HDA_CORBCTL, 0);
    mmio_write8(HDA_RIRBCTL, 0);

    mmio_write32(HDA_GCTL, mmio_read32(HDA_GCTL) & ~1u);
    if (wait32(HDA_GCTL, 1u, 0) < 0)
        return -1;
    for (uint32_t spin = 0; spin < 10000u; spin++)
        __asm__ volatile("pause");
    mmio_write32(HDA_GCTL, mmio_read32(HDA_GCTL) | 1u);
    if (wait32(HDA_GCTL, 1u, 1u) < 0)
        return -1;
    for (uint32_t spin = 0; spin < 100000u; spin++)
        __asm__ volatile("pause");
    return 0;
}

static int hda_setup_command_rings(void) {
    mmio_write8(HDA_CORBCTL, 0);
    if (wait8(HDA_CORBCTL, 2u, 0) < 0)
        return -1;
    mmio_write8(HDA_RIRBCTL, 0);
    if (wait8(HDA_RIRBCTL, 2u, 0) < 0)
        return -1;

    hda_corb_entries = ring_entries(mmio_read8(HDA_CORBSIZE), HDA_CORBSIZE);
    hda_rirb_entries = ring_entries(mmio_read8(HDA_RIRBSIZE), HDA_RIRBSIZE);
    if (!hda_corb_entries || !hda_rirb_entries)
        return -1;

    for (uint32_t i = 0; i < 1024u; i++)
        hda_corb[i] = 0;
    for (uint32_t i = 0; i < 512u; i++) {
        hda_rirb[i].response = 0;
        hda_rirb[i].response_ex = 0;
    }

    mmio_write32(HDA_CORBLBASE, (uint32_t)(uintptr_t)hda_corb);
    mmio_write32(HDA_CORBUBASE, 0);
    mmio_write16(HDA_CORBWP, 0);
    mmio_write16(HDA_CORBRP, 0x8000u);
    for (uint32_t spin = 0; spin < 100000u; spin++) {
        if (mmio_read16(HDA_CORBRP) & 0x8000u)
            break;
    }
    mmio_write16(HDA_CORBRP, 0);
    for (uint32_t spin = 0; spin < 100000u; spin++) {
        if (!(mmio_read16(HDA_CORBRP) & 0x8000u))
            break;
    }
    mmio_write8(HDA_CORBSTS, mmio_read8(HDA_CORBSTS));

    mmio_write32(HDA_RIRBLBASE, (uint32_t)(uintptr_t)hda_rirb);
    mmio_write32(HDA_RIRBUBASE, 0);
    mmio_write16(HDA_RIRBWP, 0x8000u);
    hda_rirb_read = 0;
    mmio_write16(HDA_RINTCNT, 1);
    mmio_write8(HDA_RIRBSTS, mmio_read8(HDA_RIRBSTS));

    io_dma_wmb();
    mmio_write8(HDA_CORBCTL, 0x02u);
    mmio_write8(HDA_RIRBCTL, 0x02u);
    return 0;
}

static int hda_immediate_command(uint32_t command, uint32_t *response) {
    for (uint32_t spin = 0; spin < 1000000u; spin++) {
        if (!(mmio_read16(HDA_ICIS) & 1u))
            break;
        if (spin == 999999u)
            return -1;
        __asm__ volatile("pause");
    }
    if (mmio_read16(HDA_ICIS) & 2u)
        mmio_write16(HDA_ICIS, 2u);
    mmio_write32(HDA_ICOI, command);
    mmio_write16(HDA_ICIS, 1u);
    for (uint32_t spin = 0; spin < 1000000u; spin++) {
        uint16_t status = mmio_read16(HDA_ICIS);
        if (!(status & 1u) && (status & 2u)) {
            *response = mmio_read32(HDA_ICII);
            mmio_write16(HDA_ICIS, 2u);
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -1;
}

static int hda_command(uint32_t command, uint32_t *response) {
    if (hda_single_command)
        return hda_immediate_command(command, response);

    uint16_t wp = (uint16_t)(mmio_read16(HDA_CORBWP) &
                             (hda_corb_entries - 1u));
    wp = (uint16_t)((wp + 1u) & (hda_corb_entries - 1u));
    hda_corb[wp] = command;
    io_dma_wmb();
    mmio_write16(HDA_CORBWP, wp);

    for (uint32_t spin = 0; spin < 2000000u; spin++) {
        uint16_t hardware_wp = (uint16_t)(mmio_read16(HDA_RIRBWP) &
                                          (hda_rirb_entries - 1u));
        while (hda_rirb_read != hardware_wp) {
            hda_rirb_read =
                (uint16_t)((hda_rirb_read + 1u) & (hda_rirb_entries - 1u));
            /* RIRB entries are written by the controller, not by C code. */
            __asm__ volatile("" ::: "memory");
            struct hda_rirb_entry entry = hda_rirb[hda_rirb_read];
            if (!(entry.response_ex & 0x10u) &&
                (entry.response_ex & 0x0Fu) == (command >> 28)) {
                *response = entry.response;
                return 0;
            }
        }
        __asm__ volatile("pause");
    }
    serial_puts("[hda] verb timeout cmd=");
    serial_puthex(command);
    serial_puts(" corbwp=");
    serial_puthex(mmio_read16(HDA_CORBWP));
    serial_puts(" corbrp=");
    serial_puthex(mmio_read16(HDA_CORBRP));
    serial_puts(" rirbwp=");
    serial_puthex(mmio_read16(HDA_RIRBWP));
    serial_puts(" corbsts=");
    serial_puthex(mmio_read8(HDA_CORBSTS));
    serial_puts(" rirbsts=");
    serial_puthex(mmio_read8(HDA_RIRBSTS));
    serial_puts("; switching to immediate commands\n");
    mmio_write8(HDA_CORBCTL, 0);
    mmio_write8(HDA_RIRBCTL, 0);
    hda_single_command = 1;
    return hda_immediate_command(command, response);
}

static int hda_verb12(uint8_t nid, uint16_t verb, uint8_t payload,
                      uint32_t *response) {
    uint32_t command = ((uint32_t)hda_codec << 28) |
                       ((uint32_t)nid << 20) |
                       ((uint32_t)verb << 8) | payload;
    return hda_command(command, response);
}

static int hda_verb4(uint8_t nid, uint8_t verb, uint16_t payload,
                     uint32_t *response) {
    uint32_t command = ((uint32_t)hda_codec << 28) |
                       ((uint32_t)nid << 20) |
                       ((uint32_t)verb << 16) | payload;
    return hda_command(command, response);
}

static uint32_t hda_get_parameter(uint8_t nid, uint8_t parameter) {
    uint32_t response = 0;
    if (hda_verb12(nid, 0xF00u, parameter, &response) < 0)
        return 0;
    return response;
}

static uint32_t hda_get_verb(uint8_t nid, uint16_t verb, uint8_t payload) {
    uint32_t response = 0;
    if (hda_verb12(nid, verb, payload, &response) < 0)
        return 0;
    return response;
}

static void hda_set_verb(uint8_t nid, uint16_t verb, uint8_t payload) {
    uint32_t response;
    (void)hda_verb12(nid, verb, payload, &response);
}

static int hda_connections(uint8_t nid, uint8_t *connections,
                           uint8_t capacity) {
    uint32_t parameter =
        hda_get_parameter(nid, HDA_PARAM_CONNECTION_LIST);
    uint8_t raw_count = (uint8_t)(parameter & 0x7Fu);
    int long_form = (parameter & 0x80u) != 0;
    uint8_t per_response = long_form ? 2u : 4u;
    uint8_t count = 0;
    uint16_t previous = 0;

    for (uint8_t raw = 0; raw < raw_count; raw++) {
        uint32_t packed = hda_get_verb(nid, 0xF02u,
                                       (uint8_t)(raw -
                                       (raw % per_response)));
        uint32_t shift = (uint32_t)(raw % per_response) *
                         (long_form ? 16u : 8u);
        uint16_t item = long_form ?
            (uint16_t)(packed >> shift) :
            (uint16_t)((packed >> shift) & 0xFFu);
        uint16_t node = item & (long_form ? 0x7FFFu : 0x7Fu);
        uint16_t range_bit = long_form ? 0x8000u : 0x80u;
        if (item & range_bit) {
            for (uint16_t expanded = (uint16_t)(previous + 1u);
                 expanded <= node && count < capacity; expanded++)
                connections[count++] = (uint8_t)expanded;
        } else if (count < capacity) {
            connections[count++] = (uint8_t)node;
        }
        previous = node;
    }
    return count;
}

static int hda_find_dac_path(uint8_t nid, uint8_t depth,
                             uint8_t visited[256]) {
    if (depth >= HDA_MAX_PATH || visited[nid])
        return -1;
    visited[nid] = 1;
    hda_path[depth] = nid;

    uint32_t caps = hda_get_parameter(nid, HDA_PARAM_WIDGET_CAPS);
    uint8_t type = (uint8_t)((caps >> 20) & 0x0Fu);
    if (type == HDA_WIDGET_AUDIO_OUTPUT) {
        hda_path_length = (uint8_t)(depth + 1u);
        return 0;
    }

    uint8_t connections[HDA_MAX_CONNECTIONS];
    int count = hda_connections(nid, connections, HDA_MAX_CONNECTIONS);
    for (int i = 0; i < count; i++) {
        if (hda_find_dac_path(connections[i], (uint8_t)(depth + 1u),
                              visited) == 0) {
            hda_path_connection[depth] = (uint8_t)i;
            return 0;
        }
    }
    visited[nid] = 0;
    return -1;
}

static uint8_t hda_zero_db_gain(uint8_t nid, int output) {
    uint32_t widget_caps =
        hda_get_parameter(nid, HDA_PARAM_WIDGET_CAPS);
    uint8_t parameter = output ? HDA_PARAM_OUTPUT_AMP_CAPS :
                                 HDA_PARAM_INPUT_AMP_CAPS;
    uint32_t amp_caps;
    if (widget_caps & (1u << 3))
        amp_caps = hda_get_parameter(nid, parameter);
    else
        amp_caps = hda_get_parameter(hda_afg, parameter);
    return (uint8_t)(amp_caps & 0x7Fu);
}

static void hda_unmute_output(uint8_t nid) {
    uint32_t caps = hda_get_parameter(nid, HDA_PARAM_WIDGET_CAPS);
    if (!(caps & (1u << 2)))
        return;
    uint16_t payload = (uint16_t)(0xB000u | hda_zero_db_gain(nid, 1));
    uint32_t response;
    (void)hda_verb4(nid, 0x3u, payload, &response);
}

static void hda_unmute_input(uint8_t nid, uint8_t connection) {
    uint32_t caps = hda_get_parameter(nid, HDA_PARAM_WIDGET_CAPS);
    if (!(caps & (1u << 1)))
        return;
    uint16_t payload = (uint16_t)(0x7000u |
                                  ((uint16_t)connection << 8) |
                                  hda_zero_db_gain(nid, 0));
    uint32_t response;
    (void)hda_verb4(nid, 0x3u, payload, &response);
}

static int hda_configure_codec(void) {
    uint32_t vendor = hda_get_parameter(0, 0);
    uint32_t root_nodes = hda_get_parameter(0, HDA_PARAM_NODE_COUNT);
    serial_puts("[hda] codec vendor=");
    serial_puthex(vendor);
    serial_puts(" root-nodes=");
    serial_puthex(root_nodes);
    serial_puts("\n");
    uint8_t first = (uint8_t)(root_nodes >> 16);
    uint8_t count = (uint8_t)root_nodes;
    hda_afg = 0;
    for (uint8_t i = 0; i < count; i++) {
        uint8_t nid = (uint8_t)(first + i);
        if ((hda_get_parameter(nid, HDA_PARAM_FUNCTION_TYPE) & 0x7Fu) == 1u) {
            hda_afg = nid;
            break;
        }
    }
    if (!hda_afg)
        return -1;
    hda_set_verb(hda_afg, 0x705u, 0);

    uint32_t widgets =
        hda_get_parameter(hda_afg, HDA_PARAM_NODE_COUNT);
    serial_puts("[hda] afg=");
    serial_puthex(hda_afg);
    serial_puts(" widgets=");
    serial_puthex(widgets);
    serial_puts("\n");
    first = (uint8_t)(widgets >> 16);
    count = (uint8_t)widgets;
    int best_score = -1;
    uint8_t best_pin = 0;
    for (uint8_t i = 0; i < count; i++) {
        uint8_t nid = (uint8_t)(first + i);
        uint32_t caps = hda_get_parameter(nid, HDA_PARAM_WIDGET_CAPS);
        if (((caps >> 20) & 0x0Fu) != HDA_WIDGET_PIN)
            continue;
        uint32_t config = hda_get_verb(nid, 0xF1Cu, 0);
        uint8_t connectivity = (uint8_t)(config >> 30);
        uint8_t device = (uint8_t)((config >> 20) & 0x0Fu);
        if (connectivity == 1u)
            continue;
        int score = device == 1u ? 100 :
                    device == 2u ? 90 :
                    device == 0u ? 80 : 10;
        serial_puts("[hda] pin=");
        serial_puthex(nid);
        serial_puts(" config=");
        serial_puthex(config);
        serial_puts(" connlen=");
        serial_puthex(hda_get_parameter(nid, HDA_PARAM_CONNECTION_LIST));
        serial_puts("\n");
        uint8_t visited[256];
        for (uint32_t v = 0; v < sizeof(visited); v++)
            visited[v] = 0;
        hda_path_length = 0;
        if (score > best_score &&
            hda_find_dac_path(nid, 0, visited) == 0) {
            best_score = score;
            best_pin = nid;
        }
    }
    if (!best_pin)
        return -1;

    /*
     * Re-run the search for the selected pin: trial searches for later pins
     * reuse the single path array.
     */
    uint8_t visited[256];
    for (uint32_t v = 0; v < sizeof(visited); v++)
        visited[v] = 0;
    hda_path_length = 0;
    if (hda_find_dac_path(best_pin, 0, visited) < 0)
        return -1;
    hda_pin = best_pin;
    hda_dac = hda_path[hda_path_length - 1u];

    for (uint8_t i = 0; i < hda_path_length; i++) {
        uint8_t nid = hda_path[i];
        hda_set_verb(nid, 0x705u, 0);
    }
    for (uint32_t spin = 0; spin < 10000u; spin++)
        __asm__ volatile("pause");

    for (uint8_t i = 0; i < hda_path_length; i++) {
        uint8_t nid = hda_path[i];
        uint32_t caps = hda_get_parameter(nid, HDA_PARAM_WIDGET_CAPS);
        uint8_t type = (uint8_t)((caps >> 20) & 0x0Fu);
        hda_unmute_output(nid);
        if (i + 1u < hda_path_length) {
            hda_unmute_input(nid, hda_path_connection[i]);
            if (type == HDA_WIDGET_AUDIO_SELECTOR)
                hda_set_verb(nid, 0x701u, hda_path_connection[i]);
        }
    }

    hda_set_verb(hda_pin, 0x707u, 0x40u);
    if (hda_get_parameter(hda_pin, HDA_PARAM_PIN_CAPS) & (1u << 16))
        hda_set_verb(hda_pin, 0x70Cu, 0x02u);

    uint32_t response;
    if (hda_verb4(hda_dac, 0x2u, HDA_STREAM_FORMAT, &response) < 0)
        return -1;
    hda_set_verb(hda_dac, 0x706u, (uint8_t)(HDA_STREAM_TAG << 4));
    return 0;
}

static uint32_t hda_stream_reg(uint32_t offset) {
    return HDA_STREAM_BASE +
           (uint32_t)hda_stream_index * HDA_STREAM_STRIDE + offset;
}

static uint32_t hda_active_dma_bytes(void) {
    return (uint32_t)hda_active_periods * HDA_PERIOD_BYTES;
}

static int hda_reset_stream(void) {
    uint32_t control = hda_stream_reg(HDA_SD_CTL);
    mmio_write8(control, 0);
    if (wait8(control, HDA_SD_RUN, 0) < 0)
        return -1;
    mmio_write8(control, HDA_SD_SRST);
    if (wait8(control, HDA_SD_SRST, HDA_SD_SRST) < 0)
        return -1;
    mmio_write8(control, 0);
    if (wait8(control, HDA_SD_SRST, 0) < 0)
        return -1;
    mmio_write8(hda_stream_reg(HDA_SD_STS), HDA_SD_STATUS_MASK);
    return 0;
}

static int hda_setup_stream(void) {
    uint16_t gcap = mmio_read16(HDA_GCAP);
    uint8_t input_streams = (uint8_t)((gcap >> 8) & 0x0Fu);
    uint8_t output_streams = (uint8_t)((gcap >> 12) & 0x0Fu);
    if (!output_streams)
        return -1;
    hda_stream_index = input_streams;
    if (hda_reset_stream() < 0)
        return -1;

    uint32_t base = hda_stream_reg(0);
    mmio_write32(base + HDA_SD_CBL, hda_active_dma_bytes());
    mmio_write16(base + HDA_SD_LVI, hda_active_periods - 1u);
    mmio_write16(base + HDA_SD_FMT, HDA_STREAM_FORMAT);
    mmio_write32(base + HDA_SD_BDPL, (uint32_t)(uintptr_t)hda_bdl);
    mmio_write32(base + HDA_SD_BDPU, 0);
    mmio_write8(base + 2u, (uint8_t)(HDA_STREAM_TAG << 4));
    return 0;
}

static uint32_t hda_forward_distance(uint32_t position, uint32_t previous) {
    uint32_t bytes = hda_active_dma_bytes();
    return position >= previous ? position - previous
                                : bytes - previous + position;
}

static uint32_t hda_dma_position(void) {
    uint32_t bytes = hda_active_dma_bytes();
    __asm__ volatile("" ::: "memory");
    uint32_t position =
        hda_position_buffer[(uint32_t)hda_stream_index * 2u];
    uint32_t link =
        mmio_read32(hda_stream_reg(HDA_SD_LPIB)) % bytes;
    if (position >= bytes)
        return link;

    /*
     * Linux needs controller-specific position_fix policies because some HDA
     * implementations lag or stall either the DMA position buffer or LPIB.
     * Use whichever valid source made the smaller forward advance; if one is
     * stationary, prefer the source that is moving.  The lagging source is
     * safer because it never releases a buffer that hardware may still read.
     */
    uint32_t position_delta =
        hda_forward_distance(position, hda_last_position);
    uint32_t link_delta = hda_forward_distance(link, hda_last_position);
    if (!position_delta)
        return link_delta < bytes / 2u ? link : position;
    if (!link_delta)
        return position_delta < bytes / 2u ? position : link;
    if (position_delta >= bytes / 2u)
        return link;
    if (link_delta >= bytes / 2u)
        return position;
    return position_delta <= link_delta ? position : link;
}

static uint32_t hda_take_sample(void) {
    if (hda_fifo_count) {
        hda_current_sample =
            (int16_t)(((int)hda_fifo[hda_fifo_read] - 128) << 8);
        hda_fifo_read = (hda_fifo_read + 1u) &
                        (HDA_FIFO_BYTES - 1u);
        hda_fifo_count--;
        return 1;
    }
    hda_current_sample = 0;
    return 0;
}

static uint16_t hda_fill_period(uint8_t period) {
    int16_t *destination = hda_buffers[period];
    uint32_t sources = 0;
    for (uint32_t frame = 0; frame < HDA_PERIOD_FRAMES; frame++) {
        hda_phase += hda_input_rate;
        if (hda_phase >= HDA_OUTPUT_RATE) {
            hda_phase -= HDA_OUTPUT_RATE;
            sources += hda_take_sample();
        }
        destination[frame * 2u] = hda_current_sample;
        destination[frame * 2u + 1u] = hda_current_sample;
    }
    return (uint16_t)sources;
}

static void hda_refill_period(uint8_t period) {
    uint16_t retired = hda_period_sources[period];
    if (retired > hda_dma_source_samples)
        hda_dma_source_samples = 0;
    else
        hda_dma_source_samples -= retired;
    hda_period_sources[period] = hda_fill_period(period);
    hda_dma_source_samples += hda_period_sources[period];
}

static void hda_service_stream_locked(void) {
    if (!hda_playing)
        return;
    uint32_t position = hda_dma_position();
    uint8_t current = (uint8_t)(position / HDA_PERIOD_BYTES);

    while (hda_next_refill != current) {
        hda_refill_period(hda_next_refill);
        hda_next_refill =
            (uint8_t)((hda_next_refill + 1u) % hda_active_periods);
    }
    hda_last_position = position;
    io_dma_wmb();

    uint8_t status = mmio_read8(hda_stream_reg(HDA_SD_STS));
    if (status & HDA_SD_STATUS_MASK)
        mmio_write8(hda_stream_reg(HDA_SD_STS),
                    status & HDA_SD_STATUS_MASK);
}

static uint32_t hda_ring_source_samples(void) {
    return (HDA_PERIOD_FRAMES * hda_active_periods * hda_input_rate +
            HDA_OUTPUT_RATE - 1u) / HDA_OUTPUT_RATE;
}

static uint32_t hda_period_source_samples(void) {
    return (HDA_PERIOD_FRAMES * hda_input_rate +
            HDA_OUTPUT_RATE - 1u) / HDA_OUTPUT_RATE;
}

static uint32_t hda_start_threshold(void) {
    /*
     * Prime the active DMA ring and retain one complete source period in the
     * FIFO.  Refills happen at period boundaries; a fractional-period reserve
     * would be committed with silence for the missing tail and produce a
     * regular gap even when the producer catches up a few milliseconds later.
     */
    return hda_ring_source_samples() + hda_period_source_samples();
}

static void hda_start_playback(void) {
    hda_phase = HDA_OUTPUT_RATE - hda_input_rate;
    hda_current_sample = 0;
    hda_dma_source_samples = 0;
    for (uint8_t i = 0; i < hda_active_periods; i++) {
        hda_period_sources[i] = hda_fill_period(i);
        hda_dma_source_samples += hda_period_sources[i];
    }
    for (uint8_t i = hda_active_periods; i < HDA_PERIODS; i++)
        hda_period_sources[i] = 0;
    hda_next_refill = 0;
    hda_last_position = 0;
    io_dma_wmb();

    mmio_write32(HDA_INTCTL, 0);
    mmio_write8(hda_stream_reg(HDA_SD_STS), HDA_SD_STATUS_MASK);
    mmio_write8(hda_stream_reg(HDA_SD_CTL), HDA_SD_RUN);
    hda_playing = 1;
}

int hda_init(void) {
    hda_pci = pci_find_class(0x04u, 0x03u, 0);
    if (!hda_pci) {
        serial_puts("[audio] Intel HDA not found\n");
        return -1;
    }

    uint32_t bar0 = pci_bar(hda_pci, 0);
    if ((bar0 & 1u) || !(bar0 & ~0x0Fu)) {
        serial_puts("[hda] invalid MMIO BAR\n");
        return -1;
    }
    if ((bar0 & 0x06u) == 0x04u && pci_bar(hda_pci, 1) != 0) {
        serial_puts("[hda] MMIO BAR is above 4 GiB\n");
        return -1;
    }

    pci_enable_device(hda_pci, 0, 1, 1);
    pci_disable_intx(hda_pci);
    hda_mmio = (volatile uint8_t *)paging_map_mmio(bar0 & ~0x0Fu, 4096u);
    if (!hda_mmio) {
        serial_puts("[hda] cannot map MMIO BAR\n");
        return -1;
    }

    uintptr_t corb_page = pmm_alloc_pages(1);
    uintptr_t rirb_page = pmm_alloc_pages(1);
    uintptr_t bdl_page = pmm_alloc_pages(1);
    uintptr_t position_page = pmm_alloc_pages(1);
    if (!corb_page || !rirb_page || !bdl_page || !position_page)
        return -1;
    hda_corb = (uint32_t *)corb_page;
    hda_rirb = (struct hda_rirb_entry *)rirb_page;
    hda_bdl = (struct hda_bdl_entry *)bdl_page;
    hda_position_buffer = (uint32_t *)position_page;
    for (uint32_t i = 0; i < 1024u; i++)
        hda_position_buffer[i] = 0;

    for (uint8_t i = 0; i < HDA_PERIODS; i++) {
        uintptr_t page = pmm_alloc_pages(1);
        if (!page)
            return -1;
        hda_buffers[i] = (int16_t *)page;
        hda_bdl[i].address_low = (uint32_t)page;
        hda_bdl[i].address_high = 0;
        hda_bdl[i].length = HDA_PERIOD_BYTES;
        hda_bdl[i].flags = 0;
        hda_period_sources[i] = 0;
        for (uint32_t j = 0; j < HDA_PERIOD_BYTES / 2u; j++)
            hda_buffers[i][j] = 0;
    }

    if (hda_controller_reset() < 0) {
        serial_puts("[hda] controller reset failed\n");
        return -1;
    }
    mmio_write32(HDA_DPUBASE, 0);
    mmio_write32(HDA_DPLBASE,
                 (uint32_t)(uintptr_t)hda_position_buffer | 1u);
    uint16_t state = mmio_read16(HDA_STATESTS);
    if (!state) {
        serial_puts("[hda] no codec detected\n");
        return -1;
    }
    for (hda_codec = 0; hda_codec < 15u; hda_codec++) {
        if (state & (1u << hda_codec))
            break;
    }
    if (hda_codec >= 15u)
        return -1;

    if (hda_setup_command_rings() < 0) {
        serial_puts("[hda] CORB/RIRB setup failed\n");
        return -1;
    }
    if (hda_configure_codec() < 0) {
        serial_puts("[hda] codec output route not found\n");
        return -1;
    }
    if (hda_setup_stream() < 0) {
        serial_puts("[hda] output stream setup failed\n");
        return -1;
    }

    /*
     * The deferred worker services LPIB/position-buffer every 4 ms, well
     * inside a 21.3 ms period.  Keep controller and PCI INTx disabled: this
     * avoids VMware's hosted PIRQ timing without sacrificing DMA continuity.
     */
    mmio_write32(HDA_INTCTL, 0);
    pci_disable_intx(hda_pci);

    hda_fifo_read = hda_fifo_write = hda_fifo_count = 0;
    hda_dma_source_samples = 0;
    hda_playing = 0;
    hda_ready = 1;
    serial_puts("[hda] 48000 Hz s16 stereo codec=");
    serial_puthex(hda_codec);
    serial_puts(" afg=");
    serial_puthex(hda_afg);
    serial_puts(" pin=");
    serial_puthex(hda_pin);
    serial_puts(" dac=");
    serial_puthex(hda_dac);
    serial_puts(" posbuf-polling\n");
    return 0;
}

int hda_write(const uint8_t *data, size_t size) {
    if (!hda_ready || !data)
        return -1;
    uint32_t flags = irq_save();
    hda_service_stream_locked();
    size_t available = HDA_FIFO_BYTES - hda_fifo_count;
    uint32_t queued = hda_fifo_count + hda_dma_source_samples;
    uint32_t latency_available =
        queued < hda_queue_limit ? hda_queue_limit - queued : 0;
    if (available > latency_available)
        available = latency_available;
    size_t written = size < available ? size : available;
    for (size_t i = 0; i < written; i++) {
        hda_fifo[hda_fifo_write] = data[i];
        hda_fifo_write = (hda_fifo_write + 1u) &
                         (HDA_FIFO_BYTES - 1u);
    }
    hda_fifo_count += (uint32_t)written;
    if (!hda_playing && hda_fifo_count >= hda_start_threshold())
        hda_start_playback();
    irq_restore(flags);
    return (int)written;
}

void hda_poll(void) {
    if (!hda_ready || !hda_playing)
        return;
    uint32_t flags = irq_save();
    /*
     * Poll unconditionally.  The worker compares the controller-maintained
     * DMA position buffer with LPIB every tick, so no hosted INTx is required.
     */
    hda_service_stream_locked();
    irq_restore(flags);
}

static int hda_flush_locked(void) {
    /*
     * Stopping writes is not enough: both the source FIFO and the cyclic DMA
     * ring contain audio that has not reached the speakers yet.  Reset the
     * stream so a virtual HDA backend also discards its pending host buffers,
     * then rebuild an idle stream ready for the next writer.
     */
    mmio_write32(HDA_INTCTL, 0);
    if (hda_setup_stream() < 0)
        return -1;
    hda_set_verb(hda_dac, 0x706u, (uint8_t)(HDA_STREAM_TAG << 4));
    hda_fifo_read = hda_fifo_write = hda_fifo_count = 0;
    hda_dma_source_samples = 0;
    hda_phase = HDA_OUTPUT_RATE - hda_input_rate;
    hda_current_sample = 0;
    hda_next_refill = 0;
    hda_last_position = 0;
    hda_position_buffer[(uint32_t)hda_stream_index * 2u] = 0;
    for (uint8_t i = 0; i < HDA_PERIODS; i++) {
        hda_period_sources[i] = 0;
        for (uint32_t sample = 0;
             sample < HDA_PERIOD_FRAMES * 2u; sample++)
            hda_buffers[i][sample] = 0;
    }
    io_dma_wmb();
    hda_playing = 0;
    return 0;
}

int hda_flush(void) {
    if (!hda_ready)
        return -1;
    uint32_t flags = irq_save();
    int result = hda_flush_locked();
    irq_restore(flags);
    return result;
}

int hda_set_rate(uint32_t rate, uint32_t latency_ms) {
    if (!hda_ready ||
        (rate != 11025u && rate != 22050u && rate != 44100u))
        return -1;
    if (!latency_ms || latency_ms > HDA_MAX_LATENCY_MS)
        latency_ms = HDA_DEFAULT_LATENCY_MS;

    /*
     * Treat latency as the total queued time, like a normal PCM device:
     * samples already staged in DMA plus samples waiting in the source FIFO.
     * Use the largest whole-period DMA ring that fits the request; the FIFO
     * holds only the small remainder.  Two periods are the safety minimum for
     * the 4 ms polling worker.
     */
    uint32_t requested_frames =
        latency_ms * HDA_OUTPUT_RATE / 1000u;
    uint32_t periods = requested_frames / HDA_PERIOD_FRAMES;
    if (periods < HDA_MIN_PERIODS)
        periods = HDA_MIN_PERIODS;
    if (periods > HDA_PERIODS)
        periods = HDA_PERIODS;

    uint32_t queue_limit = (rate * latency_ms + 999u) / 1000u;
    uint32_t ring_samples =
        (HDA_PERIOD_FRAMES * periods * rate +
         HDA_OUTPUT_RATE - 1u) / HDA_OUTPUT_RATE;
    uint32_t period_samples =
        (HDA_PERIOD_FRAMES * rate +
         HDA_OUTPUT_RATE - 1u) / HDA_OUTPUT_RATE;
    uint32_t minimum = ring_samples + period_samples;
    if (queue_limit < minimum)
        queue_limit = minimum;
    if (queue_limit > HDA_FIFO_BYTES)
        queue_limit = HDA_FIFO_BYTES;

    uint32_t flags = irq_save();
    hda_active_periods = (uint8_t)periods;
    hda_queue_limit = queue_limit;
    hda_input_rate = rate;
    int result = hda_flush_locked();
    if (result == 0)
        hda_phase = HDA_OUTPUT_RATE - rate;
    irq_restore(flags);
    return result;
}

int hda_queued_samples(void) {
    if (!hda_ready)
        return -1;
    uint32_t flags = irq_save();
    hda_service_stream_locked();
    uint32_t queued = hda_fifo_count + hda_dma_source_samples;
    irq_restore(flags);
    return (int)queued;
}
