#include "ac97.h"
#include "io.h"
#include "irq.h"
#include "pmm.h"
#include "serial.h"
#include "timer.h"

enum {
    PCI_ADDR = 0xCF8, PCI_DATA = 0xCFC,
    AC97_VENDOR = 0x8086, AC97_DEVICE = 0x2415,
    DESCRIPTORS = 32, ACTIVE_DESCRIPTORS = 4,
    OUTPUT_RATE = 44100,
    FRAMES_PER_BUFFER = 512, FIFO_BYTES = 8192,
};

/* Enable temporarily when diagnosing DMA/FIFO behaviour.  Register polling
 * and serial formatting do not belong in the normal real-time audio path. */
#define AC97_DIAGNOSTICS 0

struct ac97_desc {
    uint32_t address;
    uint32_t control_length;
};

static struct ac97_desc *bdl;
static int16_t *buffers[DESCRIPTORS];
static uint8_t fifo[FIFO_BYTES];
static uint32_t fifo_read, fifo_write, fifo_count;
static uint16_t nam, nabm;
static uint8_t irq_line;
static int ready, playing;
static int16_t current_sample;
static uint8_t repeat_left;
static uint32_t input_rate = 11025;
static uint8_t input_upsample = 4;
static uint8_t active_descriptors = ACTIVE_DESCRIPTORS;
static uint32_t startup_samples;
#if AC97_DIAGNOSTICS
static uint32_t stat_since, stat_underruns, stat_irqs, stat_fifo_errors;
static uint32_t stat_restarts, stat_refills, stat_written;

static void report_stats(void) {
    uint32_t now = timer_ticks();
    if (!stat_since) stat_since = now;
    if (now - stat_since < TIMER_HZ * 2u) return;
    serial_puts("[audiostat] rate="); serial_puthex(input_rate);
    serial_puts(" fifo="); serial_puthex(fifo_count);
    serial_puts(" underrun="); serial_puthex(stat_underruns);
    serial_puts(" irq="); serial_puthex(stat_irqs);
    serial_puts(" refill="); serial_puthex(stat_refills);
    serial_puts(" written="); serial_puthex(stat_written);
    serial_puts(" fifoerr="); serial_puthex(stat_fifo_errors);
    serial_puts(" restart="); serial_puthex(stat_restarts);
    serial_puts(" codec="); serial_puthex(inw(nam + 0x2C));
    serial_puts(" sr="); serial_puthex(inw(nabm + 0x16));
    serial_puts(" civ="); serial_puthex(inb(nabm + 0x14));
    serial_puts(" lvi="); serial_puthex(inb(nabm + 0x15));
    serial_puts(" picb="); serial_puthex(inw(nabm + 0x18));
    serial_puts("\n");
    stat_since = now;
    stat_underruns = stat_irqs = stat_fifo_errors = 0;
    stat_restarts = stat_refills = stat_written = 0;
}
#define AUDIO_STAT(expr) do { expr; } while (0)
#else
#define AUDIO_STAT(expr) do { } while (0)
#endif

static uint32_t start_threshold(void) {
    if (startup_samples)
        return startup_samples;
    /* About 46 ms already staged in DMA plus 31 ms left in the source FIFO. */
    return ACTIVE_DESCRIPTORS * FRAMES_PER_BUFFER / input_upsample +
           (input_rate + 31u) / 32u;
}

static uint32_t pci_read(uint8_t dev, uint8_t fn, uint8_t reg) {
    outl(PCI_ADDR, 0x80000000u | ((uint32_t)dev << 11) |
         ((uint32_t)fn << 8) | (reg & 0xFCu));
    return inl(PCI_DATA);
}

static void pci_write(uint8_t dev, uint8_t fn, uint8_t reg, uint32_t value) {
    outl(PCI_ADDR, 0x80000000u | ((uint32_t)dev << 11) |
         ((uint32_t)fn << 8) | (reg & 0xFCu));
    outl(PCI_DATA, value);
}

static int find_controller(uint8_t *dev_out, uint8_t *fn_out) {
    for (uint8_t dev = 0; dev < 32; dev++) {
        for (uint8_t fn = 0; fn < 8; fn++) {
            uint32_t id = pci_read(dev, fn, 0x00);
            if ((id & 0xFFFFu) == AC97_VENDOR && (id >> 16) == AC97_DEVICE) {
                *dev_out = dev; *fn_out = fn;
                return 0;
            }
        }
    }
    return -1;
}

static uint32_t fill_buffer(uint32_t index) {
    int16_t *dst = buffers[index];
    uint32_t source_samples = 0;
    for (uint32_t frame = 0; frame < FRAMES_PER_BUFFER; frame++) {
        if (!repeat_left) {
            if (fifo_count) {
                current_sample = (int16_t)(((int)fifo[fifo_read] - 128) << 8);
                fifo_read = (fifo_read + 1u) & (FIFO_BYTES - 1u);
                fifo_count--;
                source_samples++;
            } else {
                current_sample = 0;
                AUDIO_STAT(stat_underruns++);
            }
            repeat_left = input_upsample;
        }
        dst[frame * 2u] = current_sample;
        dst[frame * 2u + 1u] = current_sample;
        repeat_left--;
    }
    return source_samples;
}

/* Maintain a short hardware DMA window from the producer's write path.  AC97
 * still transfers samples autonomously; only descriptor retirement is polled.
 * This deliberately avoids routing the shared PCI IRQ through the young IRQ
 * return path, where a bad first IRQ used to corrupt the saved iret frame. */
static void reconcile_dma_locked(void) {
    if (!playing) return;
    uint16_t status = inw(nabm + 0x16);
    uint8_t civ = inb(nabm + 0x14) & (DESCRIPTORS - 1u);
    uint8_t lvi = inb(nabm + 0x15) & (DESCRIPTORS - 1u);
    uint8_t queued = (status & 0x01u) ? 0u :
        (uint8_t)(((lvi - civ) & (DESCRIPTORS - 1u)) + 1u);
    if (queued > active_descriptors) return;
    while (queued < active_descriptors) {
        uint8_t index = (uint8_t)((lvi + 1u) & (DESCRIPTORS - 1u));
        fill_buffer(index);
        AUDIO_STAT(stat_refills++);
        outb(nabm + 0x15, index);
        lvi = index;
        queued++;
    }
    if (status & 0x1Cu)
        outw(nabm + 0x16, status & 0x1Cu);
    uint8_t control = inb(nabm + 0x1B);
    if (!(control & 0x01u)) {
        AUDIO_STAT(stat_restarts++);
        outb(nabm + 0x1B, 0x01u);
    }
}

int ac97_init(void) {
    uint8_t dev, fn;
    if (find_controller(&dev, &fn) < 0) {
        serial_puts("[audio] AC97 not found\n");
        return -1;
    }
    uint32_t bar0 = pci_read(dev, fn, 0x10);
    uint32_t bar1 = pci_read(dev, fn, 0x14);
    if (!(bar0 & 1u) || !(bar1 & 1u)) {
        serial_puts("[audio] AC97 invalid BARs\n");
        return -1;
    }
    nam = (uint16_t)(bar0 & ~3u);
    nabm = (uint16_t)(bar1 & ~3u);
    irq_line = (uint8_t)pci_read(dev, fn, 0x3C);
    if (irq_line != 11) {
        serial_puts("[audio] AC97 needs PCI IRQ11\n");
        return -1;
    }
    uint32_t cmd = pci_read(dev, fn, 0x04);
    /* I/O + bus master, with legacy PCI INTx disabled. */
    pci_write(dev, fn, 0x04, cmd | 0x00000405u);

    uintptr_t bdl_page = pmm_alloc_pages(1);
    if (!bdl_page) return -1;
    bdl = (struct ac97_desc *)bdl_page;
    for (uint32_t i = 0; i < DESCRIPTORS; i++) {
        uintptr_t page = pmm_alloc_pages(1);
        if (!page) return -1;
        buffers[i] = (int16_t *)page;
        bdl[i].address = (uint32_t)page;
        /* Length is in 16-bit samples: frames x two channels. */
        /* Do not request per-descriptor completion interrupts.  The producer
         * advances LVI while the bus-master engine consumes this ring. */
        bdl[i].control_length = FRAMES_PER_BUFFER * 2u;
        for (uint32_t j = 0; j < FRAMES_PER_BUFFER * 2u; j++) buffers[i][j] = 0;
    }

    outl(nabm + 0x2C, 0x00000002u); /* cold reset AC-link */
    for (uint32_t i = 0; i < 100000u; i++) io_wait();
    outw(nam + 0x00, 0); /* codec reset */
    outw(nam + 0x02, 0); /* master volume unmuted */
    outw(nam + 0x18, 0); /* PCM volume unmuted */
    outw(nam + 0x2A, inw(nam + 0x2A) | 1u); /* variable rate audio */
    /* Hardware always emits standard 44.1 kHz stereo. The source stream is
     * unsigned mono at a configurable exact divisor of this rate. */
    outw(nam + 0x2C, OUTPUT_RATE);

    outb(nabm + 0x1B, 0x02); /* reset PCM-out engine */
    for (uint32_t i = 0; i < 100000u && (inb(nabm + 0x1B) & 0x02u); i++) io_wait();
    outl(nabm + 0x10, (uint32_t)bdl);
    outw(nabm + 0x16, 0x1Cu);

    /* Keep PCI IRQ11 masked as a second line of defence.  Descriptor
     * maintenance is producer-driven; bus-master DMA remains asynchronous. */
    outb(0xA1, (uint8_t)(inb(0xA1) | (1u << 3)));
    ready = 1;
    serial_puts("[audio] Intel AC97 44100 Hz output\n");
    return 0;
}

static void start_playback(void) {
    for (uint32_t i = 0; i < active_descriptors; i++) fill_buffer(i);
    outl(nabm + 0x10, (uint32_t)bdl);
    outb(nabm + 0x15, active_descriptors - 1u);
    outw(nabm + 0x16, 0x1Cu);
    outb(nabm + 0x1B, 0x01u); /* run, with all AC97 interrupt enables off */
    playing = 1;
    serial_puts("[audio] PCM playback started (AC97 bus master)\n");
}

int ac97_write(const uint8_t *data, size_t size) {
    if (!ready || !data) return -1;
    uint32_t flags = irq_save();
    size_t available = FIFO_BYTES - fifo_count;
    size_t written = size < available ? size : available;
    for (size_t i = 0; i < written; i++) {
        fifo[fifo_write] = data[i];
        fifo_write = (fifo_write + 1u) & (FIFO_BYTES - 1u);
    }
    fifo_count += (uint32_t)written;
    AUDIO_STAT(stat_written += (uint32_t)written);
    /* Do not start from the first tiny game-tick write.  Prime the hardware
     * window and retain a small source-side reserve for bursty producers. */
    if (!playing && fifo_count >= start_threshold()) start_playback();
    else if (playing) reconcile_dma_locked();
    irq_restore(flags);
#if AC97_DIAGNOSTICS
    /* Serial output is intentionally outside the audio critical section. */
    report_stats();
#endif
    return (int)written;
}

int ac97_set_rate(uint32_t rate, uint32_t latency_ms) {
    if (!ready || (rate != 11025u && rate != 22050u && rate != 44100u))
        return -1;
    uint32_t flags = irq_save();
    outb(nabm + 0x1B, 0x00); /* stop PCM-out before replacing stream state */
    outb(nabm + 0x1B, 0x02);
    for (uint32_t i = 0; i < 100000u && (inb(nabm + 0x1B) & 0x02u); i++)
        io_wait();
    outl(nabm + 0x10, (uint32_t)bdl);
    outw(nabm + 0x16, 0x1Cu);
    fifo_read = fifo_write = fifo_count = 0;
    current_sample = 0;
    repeat_left = 0;
    input_rate = rate;
    input_upsample = (uint8_t)(OUTPUT_RATE / rate);
    active_descriptors = latency_ms && latency_ms <= 50u ? 2u : ACTIVE_DESCRIPTORS;
    startup_samples = latency_ms ? (rate * latency_ms + 999u) / 1000u : 0u;
    uint32_t minimum = active_descriptors * FRAMES_PER_BUFFER / input_upsample;
    if (startup_samples && startup_samples < minimum) startup_samples = minimum;
    playing = 0;
    AUDIO_STAT(stat_since = timer_ticks());
    AUDIO_STAT(stat_underruns = stat_irqs = stat_fifo_errors = 0);
    AUDIO_STAT(stat_restarts = stat_refills = stat_written = 0);
    irq_restore(flags);
    return 0;
}

int ac97_queued_samples(void) {
    if (!ready) return -1;
    uint32_t flags = irq_save();
    uint32_t queued = fifo_count;
    if (playing) {
        uint16_t status = inw(nabm + 0x16);
        uint8_t civ = inb(nabm + 0x14) & (DESCRIPTORS - 1u);
        uint8_t lvi = inb(nabm + 0x15) & (DESCRIPTORS - 1u);
        uint8_t dma = (status & 0x01u) ? 0u :
            (uint8_t)(((lvi - civ) & (DESCRIPTORS - 1u)) + 1u);
        if (dma > active_descriptors) dma = active_descriptors;
        queued += dma * FRAMES_PER_BUFFER / input_upsample;
    }
    irq_restore(flags);
    return (int)queued;
}

void ac97_poll(void) {
    /* IRQ0 is already running with interrupts disabled.  audio_write() also
     * excludes interrupts, so this cannot race the FIFO producer. */
    if (ready && playing)
        reconcile_dma_locked();
}

void ac97_irq_handler(void) {
    if (!ready) return;
    uint16_t status = inw(nabm + 0x16);
    /* IRQ11 stays masked, but acknowledge a stale status defensively if a
     * platform happens to deliver the shared line during early bring-up. */
    if (status & 0x1Cu) outw(nabm + 0x16, status & 0x1Cu);
}
