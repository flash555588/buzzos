#include <stddef.h>
#include <stdint.h>
#include "netdev.h"
#include "io.h"
#include "irq.h"
#include "serial.h"

#define IO 0x300

/* ── Register offsets (page 0) ── */
#define CR    0x00   /* Command Register */
#define PSTART 0x01   /* Page Start (W) */
#define PSTOP  0x02   /* Page Stop (W) */
#define BNRY   0x03   /* Boundary (R/W) */
#define TPSR   0x04   /* Transmit Page Start (W) */
#define TBCR0  0x05   /* Transmit Byte Count low (W) */
#define TBCR1  0x06   /* Transmit Byte Count high (W) */
#define ISR    0x07   /* Interrupt Status (R/W) */
#define RSAR0  0x08   /* Remote Start Address low (W) */
#define RSAR1  0x09   /* Remote Start Address high (W) */
#define RBCR0  0x0A   /* Remote Byte Count low (W) */
#define RBCR1  0x0B   /* Remote Byte Count high (W) */
#define RCR    0x0C   /* Receive Config (W) */
#define TCR    0x0D   /* Transmit Config (W) */
#define DCR    0x0E   /* Data Config (W) */
#define IMR    0x0F   /* Interrupt Mask (W) */
#define RDMA   0x10   /* Remote DMA Port */
#define RSTPORT 0x1F  /* Reset Port (read triggers reset) */

/* ── Page 1 registers (offset | 0x10 when page=1) ── */
#define PAR0   0x01   /* Physical Address 0 */
#define CURR   0x07   /* Current Page */

/* ── Command Register bits ── */
#define CR_STOP      0x01
#define CR_START     0x02
#define CR_TRANSMIT  0x04
#define CR_DMAREAD   0x08
#define CR_DMAWRITE  0x10
#define CR_NODMA     0x20
#define CR_PAGE0     0x00
#define CR_PAGE1     0x40

/* ── ISR bits ── */
#define ISR_PTX  0x02
#define ISR_PRX  0x01
#define ISR_RXE  0x04
#define ISR_OVW  0x10
#define ISR_RDC  0x40

/* ── DCR bits ── */
#define DCR_WTS      0x01   /* Word Transfer Select */
#define DCR_LBK      0x02   /* Loopback */
#define DCR_NOLPBK   0x08   /* No loopback (WTS must be 0) */
#define DCR_ARM      0x10   /* Auto-init Remote */
#define DCR_FIFO8    0x40

/* ── RCR bits ── */
#define RCR_AB       0x04   /* Accept Broadcast */

/* ── TCR bits ── */
#define TCR_INTLPBK  0x02   /* Internal Loopback */

/* ── Buffer layout ── */
#define TXSTART      0x40   /* TX buffer start page */
#define RXSTART      0x46   /* RX ring start page */
#define RXSTOP       0x80   /* RX ring end page (end of 16 KiB NIC RAM) */

#define RX_QUEUE_SLOTS 32
struct rx_slot {
    uint16_t len;
    uint8_t data[1514];
};
static struct rx_slot rx_queue[RX_QUEUE_SLOTS];
static volatile uint8_t rx_queue_head;
static volatile uint8_t rx_queue_tail;
static uint32_t rx_queue_dropped;

static size_t ne2000_recv_hw(void *buf, size_t max);

/* ── Page select ── */
static void sel(int page) {
    uint8_t cr = inb(IO + CR);
    outb(IO + CR, (cr & 0x3F) | ((page & 3) << 6));
}

static int wait_isr(uint8_t mask) {
    for (int i = 0; i < 100000; i++) {
        if (inb(IO + ISR) & mask) return 0;
    }
    return -1;
}

/* ── Init ── */
static int ne2000_init(struct netdev *dev) {
    (void)dev;

    /* Hardware reset */
    inb(IO + RSTPORT);

    /* Stop the chip, page 0 */
    outb(IO + CR, CR_PAGE0 | CR_NODMA | CR_STOP);
    io_wait();

    /* Byte-wide DMA, no loopback, auto-init remote */
    outb(IO + DCR, DCR_NOLPBK | DCR_ARM);
    outb(IO + RBCR0, 0);
    outb(IO + RBCR1, 0);
    outb(IO + RCR, RCR_AB);
    outb(IO + TCR, TCR_INTLPBK);

    /* Set buffer addresses */
    outb(IO + TPSR, TXSTART);
    outb(IO + PSTART, RXSTART);
    outb(IO + BNRY, RXSTART);
    outb(IO + PSTOP, RXSTOP);

    /* Re-apply DCR with FIFO */
    outb(IO + CR, CR_PAGE0 | CR_NODMA | CR_STOP);
    outb(IO + DCR, DCR_FIFO8 | DCR_NOLPBK | DCR_ARM);

    /* Start the chip */
    outb(IO + CR, CR_NODMA | CR_START);

    /* Clear interrupts. Receive IRQs are enabled after the MAC and ring are
     * fully programmed below. */
    outb(IO + ISR, 0xFF);
    outb(IO + IMR, 0x00);
    outb(IO + TCR, 0x00);   /* normal operation */

    /* Read MAC from PROM via remote DMA */
    outb(IO + RBCR0, 32);
    outb(IO + RBCR1, 0);
    outb(IO + RSAR0, 0);
    outb(IO + RSAR1, 0);
    outb(IO + CR, CR_PAGE0 | CR_START | CR_DMAREAD);
    for (int i = 0; i < 6; i++) {
        inb(IO + RDMA);             /* skip doubled byte */
        dev->mac[i] = inb(IO + RDMA);
    }
    outb(IO + CR, CR_PAGE0 | CR_START | CR_NODMA);

    /* Program MAC into page 1 PAR registers */
    sel(1);
    outb(IO + CR, CR_PAGE1 | CR_NODMA | CR_STOP);
    for (int i = 0; i < 6; i++)
        outb(IO + PAR0 + i, dev->mac[i]);
    /* 8390 BNRY is the last consumed page; CURR is the next page the NIC
     * will fill. Keep one page between them to represent an empty ring. */
    outb(IO + CURR, RXSTART + 1);
    sel(0);
    outb(IO + CR, CR_NODMA | CR_START);

    /* IRQ10 is slave-PIC line 2; also unmask the master's cascade line. */
    outb(0x21, (uint8_t)(inb(0x21) & ~(1u << 2)));
    outb(0xA1, (uint8_t)(inb(0xA1) & ~(1u << 2)));
    outb(IO + ISR, 0xFF);
    outb(IO + IMR, ISR_PRX | ISR_RXE | ISR_OVW);

    serial_puts("[ne2000] MAC=");
    for (int i = 0; i < 6; i++) {
        serial_puthex(dev->mac[i]);
        if (i < 5) serial_putc(':');
    }
    serial_puts("\n");
    return 0;
}

/* ── Send ── */
static int ne2000_send(struct netdev *dev, const void *data, size_t len) {
    (void)dev;
    uint32_t irq_flags = irq_save();
    uint16_t data_len = (uint16_t)len;
    uint16_t length = data_len;

    if (length < 60) length = 60;
    if (length > 1514) {
        irq_restore(irq_flags);
        return -1;
    }

    /* Wait for any in-progress transmission */
    for (int i = 0; i < 50000; i++) {
        if (!(inb(IO + CR) & CR_TRANSMIT)) break;
    }

    /* Abort any running remote DMA */
    outb(IO + CR, CR_PAGE0 | CR_START | CR_NODMA);

    /* Set up remote DMA write to TX buffer */
    outb(IO + RBCR0, (uint8_t)length);
    outb(IO + RBCR1, (uint8_t)(length >> 8));
    outb(IO + RSAR0, 0);
    outb(IO + RSAR1, TXSTART);
    outb(IO + CR, CR_PAGE0 | CR_START | CR_DMAWRITE);

    /* Write packet data byte by byte */
    for (uint16_t i = 0; i < data_len; i++)
        outb(IO + RDMA, ((const uint8_t *)data)[i]);
    for (uint16_t i = data_len; i < length; i++)
        outb(IO + RDMA, 0);

    /* Complete DMA */
    outb(IO + CR, CR_PAGE0 | CR_START | CR_NODMA);

    /* Set transmit byte count and page */
    outb(IO + TBCR0, (uint8_t)length);
    outb(IO + TBCR1, (uint8_t)(length >> 8));
    outb(IO + TPSR, TXSTART);

    /* Fire transmit */
    outb(IO + CR, CR_PAGE0 | CR_START | CR_TRANSMIT);

    /* Wait for transmit complete — avoids Slirp race where response arrives
     * before TX finishes and gets dropped with "Failed to send packet" */
    if (wait_isr(ISR_PTX) < 0) {
        serial_puts("[ne2000] tx timeout\n");
    }
    outb(IO + ISR, ISR_PTX);

    irq_restore(irq_flags);
    return 0;
}

/* ── Receive ── */
static size_t ne2000_recv_hw(void *buf, size_t max) {
    uint8_t curr, bnry, packet_page, next_page;
    uint16_t pkt_len;

    /* Read CURR from page 1 */
    sel(1);
    outb(IO + CR, CR_PAGE1 | CR_START | CR_NODMA);
    curr = inb(IO + CURR);

    /* Back to page 0, read BNRY */
    sel(0);
    outb(IO + CR, CR_PAGE0 | CR_START | CR_NODMA);
    bnry = inb(IO + BNRY);

    /* Bounds check / auto-recover */
    if (bnry >= RXSTOP || bnry < RXSTART) {
        outb(IO + BNRY, RXSTART);
        sel(1);
        outb(IO + CR, CR_PAGE1 | CR_NODMA | CR_START);
        outb(IO + CURR, RXSTART + 1);
        sel(0);
        outb(IO + CR, CR_NODMA | CR_START);
        return 0;
    }

    /* BNRY denotes the last consumed page. The packet header starts on the
     * following page, wrapping at PSTOP. */
    packet_page = (uint8_t)(bnry + 1);
    if (packet_page >= RXSTOP) packet_page = RXSTART;
    if (packet_page == curr) return 0;

    /* Set up remote DMA read from this page */
    outb(IO + RBCR0, 0xFF);
    outb(IO + RBCR1, 0xFF);
    outb(IO + RSAR0, 0);
    outb(IO + RSAR1, packet_page);
    outb(IO + CR, CR_PAGE0 | CR_START | CR_DMAREAD);

    /* Read 4-byte header: status (skip), next-page, length */
    inb(IO + RDMA);                    /* status — discard */
    next_page = inb(IO + RDMA);         /* next packet page pointer */
    if (next_page < RXSTART || next_page >= RXSTOP) next_page = RXSTART;
    pkt_len  = inb(IO + RDMA);        /* length low */
    pkt_len |= (uint16_t)inb(IO + RDMA) << 8;  /* length high */

    /* pkt_len from QEMU includes 4-byte NE2000 header; subtract it */
    uint16_t frame_len = (pkt_len >= 4) ? (pkt_len - 4) : 0;
    if (frame_len > max) frame_len = (uint16_t)max;

    /* Read packet payload */
    for (uint16_t i = 0; i < frame_len; i++)
        ((uint8_t *)buf)[i] = inb(IO + RDMA);

    /* End DMA, update boundary */
    outb(IO + CR, CR_PAGE0 | CR_START | CR_NODMA);
    outb(IO + BNRY, next_page == RXSTART ? RXSTOP - 1 : next_page - 1);

    return frame_len;
}

static size_t ne2000_recv(struct netdev *dev, void *buf, size_t max) {
    (void)dev;
    if (!buf || max == 0)
        return 0;
    uint32_t irq_flags = irq_save();
    size_t result = 0;
    if (rx_queue_tail != rx_queue_head) {
        struct rx_slot *slot = &rx_queue[rx_queue_tail];
        result = slot->len < max ? slot->len : max;
        for (size_t i = 0; i < result; i++)
            ((uint8_t *)buf)[i] = slot->data[i];
        rx_queue_tail = (uint8_t)((rx_queue_tail + 1) % RX_QUEUE_SLOTS);
    } else {
        /* Covers early boot and the small interval before an IRQ is raised. */
        result = ne2000_recv_hw(buf, max);
    }
    irq_restore(irq_flags);
    return result;
}

void ne2000_irq_handler(void) {
    uint8_t status = inb(IO + ISR);
    for (;;) {
        uint8_t next = (uint8_t)((rx_queue_head + 1) % RX_QUEUE_SLOTS);
        if (next == rx_queue_tail) {
            rx_queue_dropped++;
            break;
        }
        struct rx_slot *slot = &rx_queue[rx_queue_head];
        size_t n = ne2000_recv_hw(slot->data, sizeof(slot->data));
        if (n == 0)
            break;
        slot->len = (uint16_t)n;
        rx_queue_head = next;
    }
    /* Acknowledge every condition observed on entry. New arrivals will
     * assert PRX again after this write. */
    outb(IO + ISR, status | ISR_PRX | ISR_RXE | ISR_OVW);
}

static struct netdev ne_dev = {
    .priv = 0,
    .init = ne2000_init,
    .send = ne2000_send,
    .recv = ne2000_recv,
};

void ne2000_init_device(void) {
    netdev_register(&ne_dev);
    ne_dev.init(&ne_dev);
}
