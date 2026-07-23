#include "ac97.h"
#include "io.h"
#include "irq.h"
#include "pci.h"
#include "pmm.h"
#include "serial.h"
#include "timer.h"

enum {
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
static uint8_t retire_index;
static uint32_t startup_samples;
static uint16_t descriptor_sources[DESCRIPTORS];
static uint32_t dma_source_samples;
static int ac97_interrupt(void *context);
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

static void refill_buffer(uint32_t index) {
    dma_source_samples -= descriptor_sources[index];
    descriptor_sources[index] = (uint16_t)fill_buffer(index);
    dma_source_samples += descriptor_sources[index];
}

static void retire_buffer(uint8_t index) {
    uint16_t sources = descriptor_sources[index];
    if (sources > dma_source_samples)
        dma_source_samples = 0;
    else
        dma_source_samples -= sources;
    descriptor_sources[index] = 0;
}

/* Maintain a short hardware DMA window when the bus-master reports a completed
 * descriptor. The PCI IRQ layer keeps the shared level masked until this
 * routine has acknowledged the controller and cleared its INTx source. */
static void reconcile_dma_locked(void) {
    if (!playing) return;
    uint16_t status = inw(nabm + 0x16);
    uint8_t civ = inb(nabm + 0x14) & (DESCRIPTORS - 1u);
    uint8_t lvi = inb(nabm + 0x15) & (DESCRIPTORS - 1u);

    /*
     * For a normal completion the controller advances CIV before asserting
     * BCIS, so every descriptor before CIV has left the DMA engine.  At the
     * last valid buffer it instead leaves CIV == LVI and sets DCH; that one
     * current descriptor has completed as well.  Retiring here keeps the
     * user-visible queued sample count tied to audio actually pending in DMA
     * instead of retaining a descriptor until its BDL slot wraps 32 periods
     * later.
     */
    while (retire_index != civ) {
        retire_buffer(retire_index);
        retire_index =
            (uint8_t)((retire_index + 1u) & (DESCRIPTORS - 1u));
    }
    if ((status & 0x09u) == 0x09u) {
        retire_buffer(retire_index);
        retire_index =
            (uint8_t)((retire_index + 1u) & (DESCRIPTORS - 1u));
    }

    uint8_t queued = (status & 0x01u) ? 0u :
        (uint8_t)(((lvi - civ) & (DESCRIPTORS - 1u)) + 1u);
    if (status & 0x1Cu)
        outw(nabm + 0x16, status & 0x1Cu);
    if (queued > active_descriptors)
        return;
    while (queued < active_descriptors) {
        uint8_t index = (uint8_t)((lvi + 1u) & (DESCRIPTORS - 1u));
        refill_buffer(index);
        AUDIO_STAT(stat_refills++);
        /* Publish buffer contents and its BDL entry before extending LVI. */
        io_dma_wmb();
        outb(nabm + 0x15, index);
        lvi = index;
        queued++;
    }
    uint8_t control = inb(nabm + 0x1B);
    if (!(control & 0x01u)) {
        AUDIO_STAT(stat_restarts++);
        /* Preserve IOCE. Clearing it here makes the first recovery the last
         * interrupt the stream will ever receive. */
        outb(nabm + 0x1B, (uint8_t)(control | 0x11u));
    }
}

int ac97_init(void) {
    const struct pci_device *dev =
        pci_find_device(AC97_VENDOR, AC97_DEVICE);
    if (!dev) {
        serial_puts("[audio] AC97 not found\n");
        return -1;
    }
    uint32_t bar0 = pci_bar(dev, 0);
    uint32_t bar1 = pci_bar(dev, 1);
    if (!(bar0 & 1u) || !(bar1 & 1u)) {
        serial_puts("[audio] AC97 invalid BARs\n");
        return -1;
    }
    nam = (uint16_t)(bar0 & ~3u);
    nabm = (uint16_t)(bar1 & ~3u);
    irq_line = dev->irq_line;
    pci_enable_device(dev, 1, 0, 1);
    pci_disable_intx(dev);

    uintptr_t bdl_page = pmm_alloc_pages(1);
    if (!bdl_page) return -1;
    bdl = (struct ac97_desc *)bdl_page;
    for (uint32_t i = 0; i < DESCRIPTORS; i++) {
        uintptr_t page = pmm_alloc_pages(1);
        if (!page) return -1;
        buffers[i] = (int16_t *)page;
        bdl[i].address = (uint32_t)page;
        /* Length is in 16-bit samples: frames x two channels. */
        /* IOC requests one period interrupt per descriptor. */
        bdl[i].control_length =
            0x80000000u | (FRAMES_PER_BUFFER * 2u);
        descriptor_sources[i] = 0;
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

    ready = 1;
    if (irq_register_handler(irq_line, ac97_interrupt, 0, 1) < 0 ||
        pci_enable_intx(dev) < 0) {
        ready = 0;
        pci_disable_intx(dev);
        return -1;
    }
    serial_puts("[audio] Intel AC97 44100 Hz output\n");
    return 0;
}

static void start_playback(void) {
    dma_source_samples = 0;
    retire_index = 0;
    for (uint32_t i = 0; i < DESCRIPTORS; i++)
        descriptor_sources[i] = 0;
    for (uint32_t i = 0; i < active_descriptors; i++)
        refill_buffer(i);
    io_dma_wmb();
    outl(nabm + 0x10, (uint32_t)bdl);
    outb(nabm + 0x15, active_descriptors - 1u);
    outw(nabm + 0x16, 0x1Cu);
    /* RPBM | IOCE: run and interrupt on each completed descriptor. */
    outb(nabm + 0x1B, 0x11u);
    playing = 1;
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
    irq_restore(flags);
#if AC97_DIAGNOSTICS
    /* Serial output is intentionally outside the audio critical section. */
    report_stats();
#endif
    return (int)written;
}

static int ac97_flush_locked(void) {
    /*
     * Reset PCM-out, rather than merely emptying the software FIFO.  This
     * makes close/pause discard samples already staged in bus-master DMA and
     * tells emulators to drop their corresponding host-side audio buffer.
     */
    outb(nabm + 0x1B, 0x00);
    outb(nabm + 0x1B, 0x02);
    for (uint32_t i = 0; i < 100000u && (inb(nabm + 0x1B) & 0x02u); i++)
        io_wait();
    if (inb(nabm + 0x1B) & 0x02u)
        return -1;
    outl(nabm + 0x10, (uint32_t)bdl);
    outw(nabm + 0x16, 0x1Cu);
    fifo_read = fifo_write = fifo_count = 0;
    current_sample = 0;
    repeat_left = 0;
    dma_source_samples = 0;
    retire_index = 0;
    for (uint32_t i = 0; i < DESCRIPTORS; i++) {
        descriptor_sources[i] = 0;
        for (uint32_t sample = 0;
             sample < FRAMES_PER_BUFFER * 2u; sample++)
            buffers[i][sample] = 0;
    }
    io_dma_wmb();
    playing = 0;
    return 0;
}

int ac97_flush(void) {
    if (!ready)
        return -1;
    uint32_t flags = irq_save();
    int result = ac97_flush_locked();
    irq_restore(flags);
    return result;
}

int ac97_set_rate(uint32_t rate, uint32_t latency_ms) {
    if (!ready || (rate != 11025u && rate != 22050u && rate != 44100u))
        return -1;
    /* Older one-argument callers did not define the latency register.  Treat
     * values larger than the FIFO can possibly prebuffer as the default
     * policy instead of creating an unreachable start threshold. */
    if (latency_ms > (FIFO_BYTES * 1000u) / rate)
        latency_ms = 0;
    uint32_t flags = irq_save();
    int result = ac97_flush_locked();
    if (result == 0) {
        input_rate = rate;
        input_upsample = (uint8_t)(OUTPUT_RATE / rate);
        active_descriptors =
            latency_ms && latency_ms <= 50u ? 2u : ACTIVE_DESCRIPTORS;
        startup_samples =
            latency_ms ? (rate * latency_ms + 999u) / 1000u : 0u;
        uint32_t minimum =
            active_descriptors * FRAMES_PER_BUFFER / input_upsample;
        if (startup_samples && startup_samples < minimum)
            startup_samples = minimum;
        AUDIO_STAT(stat_since = timer_ticks());
        AUDIO_STAT(stat_underruns = stat_irqs = stat_fifo_errors = 0);
        AUDIO_STAT(stat_restarts = stat_refills = stat_written = 0);
    }
    irq_restore(flags);
    return result;
}

int ac97_queued_samples(void) {
    if (!ready) return -1;
    uint32_t flags = irq_save();
    uint32_t queued = fifo_count + dma_source_samples;
    irq_restore(flags);
    return (int)queued;
}

static int ac97_interrupt(void *context) {
    (void)context;
    if (!ready)
        return 0;
    uint16_t status = inw(nabm + 0x16);
    if (!(status & 0x1Cu))
        return 0;
    AUDIO_STAT(stat_irqs++);
    if (playing)
        reconcile_dma_locked();
    else
        outw(nabm + 0x16, status & 0x1Cu);
    return 1;
}
