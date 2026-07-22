#include "ac97.h"
#include "io.h"
#include "irq.h"
#include "pmm.h"
#include "serial.h"

enum {
    PCI_ADDR = 0xCF8, PCI_DATA = 0xCFC,
    AC97_VENDOR = 0x8086, AC97_DEVICE = 0x2415,
    DESCRIPTORS = 32, ACTIVE_DESCRIPTORS = 4,
    OUTPUT_RATE = 44100, UPSAMPLE = 4,
    FRAMES_PER_BUFFER = 512, FIFO_BYTES = 4096,
    /* Keep one source-side reserve block after priming DMA.  Doom submits
     * 315 samples at a time, while each descriptor consumes 128; without a
     * reserve an IRQ can land between two game tics and bake silence into a
     * buffer that will only play later. */
    SOURCE_RESERVE = 256,
    START_THRESHOLD = ACTIVE_DESCRIPTORS * FRAMES_PER_BUFFER / UPSAMPLE +
                      SOURCE_RESERVE,
};

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
            }
            repeat_left = UPSAMPLE;
        }
        dst[frame * 2u] = current_sample;
        dst[frame * 2u + 1u] = current_sample;
        repeat_left--;
    }
    return source_samples;
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
    pci_write(dev, fn, 0x04, cmd | 0x00000005u); /* I/O + bus master */

    uintptr_t bdl_page = pmm_alloc_pages(1);
    if (!bdl_page) return -1;
    bdl = (struct ac97_desc *)bdl_page;
    for (uint32_t i = 0; i < DESCRIPTORS; i++) {
        uintptr_t page = pmm_alloc_pages(1);
        if (!page) return -1;
        buffers[i] = (int16_t *)page;
        bdl[i].address = (uint32_t)page;
        /* Length is in 16-bit samples: frames x two channels. */
        bdl[i].control_length = 0x80000000u | (FRAMES_PER_BUFFER * 2u);
        for (uint32_t j = 0; j < FRAMES_PER_BUFFER * 2u; j++) buffers[i][j] = 0;
    }

    outl(nabm + 0x2C, 0x00000002u); /* cold reset AC-link */
    for (uint32_t i = 0; i < 100000u; i++) io_wait();
    outw(nam + 0x00, 0); /* codec reset */
    outw(nam + 0x02, 0); /* master volume unmuted */
    outw(nam + 0x18, 0); /* PCM volume unmuted */
    outw(nam + 0x2A, inw(nam + 0x2A) | 1u); /* variable rate audio */
    /* Doom and the user ABI are 11025 Hz unsigned mono.  AC97 emits a
     * standard 44100 Hz stream, using exact 4x expansion in fill_buffer().
     * This avoids fractional host resampling and keeps 315 samples exactly
     * equal to one 35 Hz Doom tic. */
    outw(nam + 0x2C, OUTPUT_RATE);

    outb(nabm + 0x1B, 0x02); /* reset PCM-out engine */
    for (uint32_t i = 0; i < 100000u && (inb(nabm + 0x1B) & 0x02u); i++) io_wait();
    outl(nabm + 0x10, (uint32_t)bdl);
    outw(nabm + 0x16, 0x1Cu);

    /* PCI IRQ11 is slave PIC IRQ3; keep the cascade and IRQ11 enabled. */
    outb(0x21, (uint8_t)(inb(0x21) & ~(1u << 2)));
    outb(0xA1, (uint8_t)(inb(0xA1) & ~(1u << 3)));
    ready = 1;
    serial_puts("[audio] Intel AC97 44100 Hz output, 11025 Hz PCM input\n");
    return 0;
}

static void start_playback(void) {
    for (uint32_t i = 0; i < ACTIVE_DESCRIPTORS; i++) fill_buffer(i);
    outl(nabm + 0x10, (uint32_t)bdl);
    outb(nabm + 0x15, ACTIVE_DESCRIPTORS - 1u);
    outw(nabm + 0x16, 0x1Cu);
    outb(nabm + 0x1B, 0x1Du); /* run + completion/FIFO/LVI interrupts */
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
    /* Do not start from the first tiny game-tick write.  Prime the hardware
     * window and retain a small source-side reserve for bursty producers. */
    if (!playing && fifo_count >= START_THRESHOLD) start_playback();
    irq_restore(flags);
    return (int)written;
}

void ac97_irq_handler(void) {
    if (!ready) return;
    uint16_t status = inw(nabm + 0x16);
    if (!(status & 0x1Cu)) return;
    /* Restore the complete look-ahead window, even when several completion
     * IRQs were coalesced while another interrupt or kernel section ran.
     * CIV is the descriptor currently consumed and LVI is the last valid
     * descriptor.  The old one-buffer-per-IRQ scheme permanently lost DMA
     * depth whenever CIV advanced by more than one, causing rare dropouts. */
    uint8_t civ = inb(nabm + 0x14) & (DESCRIPTORS - 1u);
    uint8_t lvi = inb(nabm + 0x15) & (DESCRIPTORS - 1u);
    uint32_t valid = ((uint32_t)lvi - civ) & (DESCRIPTORS - 1u);
    valid++;

    /* CIV one past LVI represents a drained/halted ring, not 32 queued
     * buffers in this deliberately short-window design. */
    if (valid > ACTIVE_DESCRIPTORS)
        valid = 0;

    while (valid < ACTIVE_DESCRIPTORS) {
        uint8_t index = valid ? (uint8_t)((lvi + 1u) & (DESCRIPTORS - 1u))
                              : civ;
        fill_buffer(index);
        lvi = index;
        valid++;
    }
    outb(nabm + 0x15, lvi);
    outw(nabm + 0x16, status & 0x1Cu);

    /* A delayed interrupt can let the engine halt at LVI.  Updating LVI is
     * sufficient on ICH, but ensure the run bit remains asserted as well. */
    uint8_t control = inb(nabm + 0x1B);
    if (!(control & 0x01u)) outb(nabm + 0x1B, control | 0x01u);
}
