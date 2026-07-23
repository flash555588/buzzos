#include <stddef.h>
#include <stdint.h>
#include "netdev.h"
#include "net.h"
#include "io.h"
#include "irq.h"
#include "serial.h"
#include "task.h"
#include "timer.h"

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
#define RX_IRQ_BUDGET 8
struct rx_slot {
    uint16_t len;
    uint8_t data[1514];
};
static struct rx_slot rx_queue[RX_QUEUE_SLOTS];
static volatile uint8_t rx_queue_head;
static volatile uint8_t rx_queue_tail;
static uint32_t rx_queue_dropped;
static volatile int tx_locked;
static volatile int tx_complete;
static volatile int tx_waiter = -1;
static uint8_t tx_staging[1516] __attribute__((aligned(2)));
static int ready;

static size_t ne2000_recv_hw(void *buf, size_t max);
static int ne2000_interrupt(void *context);

/* ── Page select ── */
static void sel(int page) {
    uint8_t cr = inb(IO + CR);
    outb(IO + CR, (cr & 0x3F) | ((page & 3) << 6));
}

static void tx_lock(void) {
    while (__sync_lock_test_and_set(&tx_locked, 1))
        task_yield();
}

static void tx_unlock(void) {
    __sync_lock_release(&tx_locked);
}

static int wait_tx_complete(void) {
    uint32_t deadline = timer_ticks() + TIMER_HZ / 4u + 1u;
    for (int spins = 0; spins < 100000; spins++) {
        uint32_t irq_flags = irq_save();
        uint8_t status = inb(IO + ISR);
        if (status & ISR_PTX) {
            outb(IO + ISR, ISR_PTX);
            tx_complete = 1;
        }
        if (tx_complete) {
            tx_waiter = -1;
            irq_restore(irq_flags);
            return 0;
        }
        if (current_task && task_get_tid() > 0) {
            tx_waiter = task_get_tid();
            task_prepare_block_current(deadline);
            task_block_current_prepared();
            tx_waiter = -1;
            irq_restore(irq_flags);
            if ((int32_t)(timer_ticks() - deadline) >= 0)
                return -1;
        } else {
            irq_restore(irq_flags);
            io_wait();
        }
    }
    return -1;
}

/* ── Init ── */
static int ne2000_init(struct netdev *dev) {
    (void)dev;
    ready = 0;

    /* Hardware reset */
    inb(IO + RSTPORT);

    /* Stop the chip, page 0 */
    outb(IO + CR, CR_PAGE0 | CR_NODMA | CR_STOP);
    io_wait();
    if (inb(IO + CR) == 0xFF)
        return -1;

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
    int mac_valid = 0;
    for (int i = 0; i < 6; i++)
        mac_valid |= dev->mac[i] != 0 && dev->mac[i] != 0xFF;
    if (!mac_valid)
        return -1;
    outb(IO + CR, CR_PAGE0 | CR_START | CR_NODMA);
    /* PROM access above is byte-wide. Normal packet DMA uses the NE2000's
     * 16-bit datapath. */
    outb(IO + DCR, DCR_FIFO8 | DCR_WTS | DCR_NOLPBK | DCR_ARM);

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

    if (irq_register_handler(10, ne2000_interrupt, 0, 1) < 0)
        return -1;
    irq_set_trigger(10, IRQ_TRIGGER_EDGE);
    outb(IO + ISR, 0xFF);
    ready = 1;
    outb(IO + IMR, ISR_PRX | ISR_PTX | ISR_RXE | ISR_OVW);
    irq_enable_line(10);

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
    uint16_t data_len = (uint16_t)len;
    uint16_t length = data_len;

    if (length < 60) length = 60;
    if (!data || length > 1514)
        return -1;
    uint16_t dma_length = (uint16_t)((length + 1u) & ~1u);
    tx_lock();
    for (uint16_t i = 0; i < data_len; i++)
        tx_staging[i] = ((const uint8_t *)data)[i];
    for (uint16_t i = data_len; i < dma_length; i++)
        tx_staging[i] = 0;

    uint32_t irq_flags = irq_save();
    /* Abort any running remote DMA */
    outb(IO + CR, CR_PAGE0 | CR_START | CR_NODMA);

    /* Set up remote DMA write to TX buffer */
    outb(IO + RBCR0, (uint8_t)dma_length);
    outb(IO + RBCR1, (uint8_t)(dma_length >> 8));
    outb(IO + RSAR0, 0);
    outb(IO + RSAR1, TXSTART);
    outb(IO + CR, CR_PAGE0 | CR_START | CR_DMAWRITE);

    io_outsw(IO + RDMA, tx_staging, dma_length / 2u);

    /* Complete DMA */
    outb(IO + CR, CR_PAGE0 | CR_START | CR_NODMA);

    /* Set transmit byte count and page */
    outb(IO + TBCR0, (uint8_t)length);
    outb(IO + TBCR1, (uint8_t)(length >> 8));
    outb(IO + TPSR, TXSTART);

    /* Fire transmit */
    tx_complete = 0;
    outb(IO + CR, CR_PAGE0 | CR_START | CR_TRANSMIT);
    irq_restore(irq_flags);

    int ret = wait_tx_complete();
    if (ret < 0)
        serial_puts("[ne2000] tx timeout\n");
    tx_unlock();
    return ret;
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

    /* Read 4-byte header in two word transfers. */
    uint16_t header0 = inw(IO + RDMA);
    uint16_t header1 = inw(IO + RDMA);
    next_page = (uint8_t)(header0 >> 8);
    if (next_page < RXSTART || next_page >= RXSTOP) next_page = RXSTART;
    pkt_len = header1;

    /* pkt_len from QEMU includes 4-byte NE2000 header; subtract it */
    uint16_t frame_len = (pkt_len >= 4) ? (pkt_len - 4) : 0;
    if (frame_len > max) frame_len = (uint16_t)max;

    /* Read packet payload through the 16-bit remote-DMA port. */
    uint16_t words = frame_len / 2u;
    if (words)
        io_insw(IO + RDMA, buf, words);
    if (frame_len & 1u)
        ((uint8_t *)buf)[frame_len - 1u] = (uint8_t)inw(IO + RDMA);

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

static int ne2000_interrupt(void *context) {
    (void)context;
    if (!ready)
        return 0;
    uint8_t status = inb(IO + ISR);
    if (status == 0 || status == 0xFF)
        return 0;
    if (status & ISR_PTX) {
        tx_complete = 1;
        if (tx_waiter > 0)
            (void)task_wake(tx_waiter);
    }
    int queued = 0;
    for (int packets = 0; packets < RX_IRQ_BUDGET; packets++) {
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
        queued = 1;
    }
    /* Acknowledge every condition observed on entry. New arrivals will
     * assert PRX again after this write. */
    outb(IO + ISR, status | ISR_PRX | ISR_RXE | ISR_OVW);
    if (queued || (status & (ISR_PRX | ISR_RXE | ISR_OVW)))
        net_rx_interrupt_notify();
    return 1;
}

static struct netdev ne_dev = {
    .name = "ne2000",
    .priv = 0,
    .init = ne2000_init,
    .send = ne2000_send,
    .recv = ne2000_recv,
};

int ne2000_init_device(void) {
    if (ne_dev.init(&ne_dev) < 0)
        return -1;
    netdev_register(&ne_dev);
    return 0;
}
