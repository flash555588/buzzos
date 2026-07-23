#include <stddef.h>
#include <stdint.h>
#include "io.h"
#include "netdev.h"
#include "pci.h"
#include "serial.h"

enum {
    PCNET_VENDOR = 0x1022,
    PCNET_DEVICE = 0x2000,

    PCNET_RDP = 0x10,
    PCNET_RAP = 0x12,
    PCNET_RESET = 0x14,
    PCNET_BDP = 0x16,

    CSR0_INIT = 0x0001,
    CSR0_STRT = 0x0002,
    CSR0_STOP = 0x0004,
    CSR0_TDMD = 0x0008,
    CSR0_IDON = 0x0100,
    CSR0_TINT = 0x0200,
    CSR0_RINT = 0x0400,
    CSR0_ERROR_BITS = 0xF800,

    DESC_OWN = 0x8000,
    DESC_ERR = 0x4000,
    DESC_STP = 0x0200,
    DESC_ENP = 0x0100,

    RX_RING_LOG2 = 3,
    RX_RING_COUNT = 1 << RX_RING_LOG2,
    TX_RING_LOG2 = 2,
    TX_RING_COUNT = 1 << TX_RING_LOG2,
    BUFFER_SIZE = 1548,
    POLL_LIMIT = 100000,
};

struct pcnet_init_block {
    uint16_t mode;
    uint8_t rlen;
    uint8_t tlen;
    uint8_t mac[6];
    uint16_t reserved;
    uint32_t filter[2];
    uint32_t rx_ring;
    uint32_t tx_ring;
} __attribute__((packed, aligned(4)));

struct pcnet_desc {
    uint32_t address;
    int16_t length;
    volatile uint16_t status;
    volatile uint32_t misc;
    uint32_t reserved;
} __attribute__((packed, aligned(16)));

static struct pcnet_init_block init_block;
static struct pcnet_desc rx_ring[RX_RING_COUNT] __attribute__((aligned(16)));
static struct pcnet_desc tx_ring[TX_RING_COUNT] __attribute__((aligned(16)));
static uint8_t rx_buffers[RX_RING_COUNT][BUFFER_SIZE] __attribute__((aligned(16)));
static uint8_t tx_buffers[TX_RING_COUNT][BUFFER_SIZE] __attribute__((aligned(16)));
static uint16_t io_base;
static uint8_t rx_index;
static uint8_t tx_index;
static uint32_t tx_packets;
static uint32_t rx_packets;

static void memory_barrier(void) {
    __asm__ volatile("" ::: "memory");
}

static uint16_t pcnet_csr_read(uint16_t index) {
    outw((uint16_t)(io_base + PCNET_RAP), index);
    return inw((uint16_t)(io_base + PCNET_RDP));
}

static void pcnet_csr_write(uint16_t index, uint16_t value) {
    outw((uint16_t)(io_base + PCNET_RAP), index);
    outw((uint16_t)(io_base + PCNET_RDP), value);
}

static void pcnet_bcr_write(uint16_t index, uint16_t value) {
    outw((uint16_t)(io_base + PCNET_RAP), index);
    outw((uint16_t)(io_base + PCNET_BDP), value);
}

static int pcnet_send(struct netdev *dev, const void *data, size_t len) {
    (void)dev;
    if (!data || len > 1514)
        return -1;

    struct pcnet_desc *desc = &tx_ring[tx_index];
    for (int i = 0; i < POLL_LIMIT && (desc->status & DESC_OWN); i++)
        io_wait();
    if (desc->status & DESC_OWN)
        return -1;

    size_t wire_len = len < 60 ? 60 : len;
    for (size_t i = 0; i < len; i++)
        tx_buffers[tx_index][i] = ((const uint8_t *)data)[i];
    for (size_t i = len; i < wire_len; i++)
        tx_buffers[tx_index][i] = 0;

    desc->misc = 0;
    desc->length = (int16_t)-(int16_t)wire_len;
    memory_barrier();
    desc->status = DESC_OWN | DESC_STP | DESC_ENP;
    memory_barrier();
    pcnet_csr_write(0, CSR0_STRT | CSR0_TDMD);

    for (int i = 0; i < POLL_LIMIT && (desc->status & DESC_OWN); i++)
        io_wait();
    uint16_t status = desc->status;
    uint16_t csr0 = pcnet_csr_read(0);
    tx_index = (uint8_t)((tx_index + 1u) & (TX_RING_COUNT - 1u));
    pcnet_csr_write(0, CSR0_TINT | CSR0_ERROR_BITS);
    if (tx_packets++ == 0) {
        serial_puts("[pcnet] first TX descriptor=");
        serial_puthex(status);
        serial_puts(" csr0=");
        serial_puthex(csr0);
        serial_puts("\n");
    }
    return (status & (DESC_OWN | DESC_ERR)) ? -1 : 0;
}

static size_t pcnet_recv(struct netdev *dev, void *buf, size_t max) {
    (void)dev;
    if (!buf || !max)
        return 0;

    struct pcnet_desc *desc = &rx_ring[rx_index];
    memory_barrier();
    uint16_t status = desc->status;
    if (status & DESC_OWN)
        return 0;

    size_t result = 0;
    if (!(status & DESC_ERR) &&
        (status & (DESC_STP | DESC_ENP)) == (DESC_STP | DESC_ENP)) {
        uint32_t message_length = desc->misc & 0x0FFFu;
        if (message_length >= 4)
            message_length -= 4; /* PCnet includes the Ethernet FCS. */
        result = message_length < max ? message_length : max;
        for (size_t i = 0; i < result; i++)
            ((uint8_t *)buf)[i] = rx_buffers[rx_index][i];
        if (result && rx_packets++ == 0) {
            serial_puts("[pcnet] first RX bytes=");
            serial_puthex((uint32_t)result);
            serial_puts(" descriptor=");
            serial_puthex(status);
            serial_puts("\n");
        }
    }

    desc->misc = 0;
    desc->length = -BUFFER_SIZE;
    memory_barrier();
    desc->status = DESC_OWN;
    rx_index = (uint8_t)((rx_index + 1u) & (RX_RING_COUNT - 1u));
    pcnet_csr_write(0, CSR0_RINT | CSR0_ERROR_BITS);
    return result;
}

static int pcnet_hw_init(struct netdev *dev) {
    const struct pci_device *pci = pci_find_device(PCNET_VENDOR, PCNET_DEVICE);
    if (!pci)
        return -1;

    uint32_t bar = pci_bar(pci, 0);
    if (!(bar & 1u))
        return -1;
    io_base = (uint16_t)(bar & ~3u);
    pci_enable_device(pci, 1, 0, 1);
    pci_config_write16(pci, 0x04,
                       pci_config_read16(pci, 0x04) | 0x0400u);

    /* A 16-bit read from RESET selects word I/O mode and resets the chip. */
    (void)inw((uint16_t)(io_base + PCNET_RESET));
    for (int i = 0; i < 100; i++)
        io_wait();
    pcnet_csr_write(0, CSR0_STOP);
    pcnet_bcr_write(20, 2); /* PCnet-PCI 32-bit software style. */

    int mac_nonzero = 0;
    for (uint32_t i = 0; i < 6; i++) {
        dev->mac[i] = inb((uint16_t)(io_base + i));
        init_block.mac[i] = dev->mac[i];
        mac_nonzero |= dev->mac[i] != 0 && dev->mac[i] != 0xFF;
    }
    if (!mac_nonzero)
        return -1;

    init_block.mode = 0;
    init_block.rlen = RX_RING_LOG2 << 4;
    init_block.tlen = TX_RING_LOG2 << 4;
    init_block.reserved = 0;
    init_block.filter[0] = 0;
    init_block.filter[1] = 0;
    init_block.rx_ring = (uint32_t)(uintptr_t)rx_ring;
    init_block.tx_ring = (uint32_t)(uintptr_t)tx_ring;

    for (uint32_t i = 0; i < RX_RING_COUNT; i++) {
        rx_ring[i].address = (uint32_t)(uintptr_t)rx_buffers[i];
        rx_ring[i].length = -BUFFER_SIZE;
        rx_ring[i].misc = 0;
        rx_ring[i].reserved = 0;
        rx_ring[i].status = DESC_OWN;
    }
    for (uint32_t i = 0; i < TX_RING_COUNT; i++) {
        tx_ring[i].address = (uint32_t)(uintptr_t)tx_buffers[i];
        tx_ring[i].length = 0;
        tx_ring[i].misc = 0;
        tx_ring[i].reserved = 0;
        tx_ring[i].status = 0;
    }
    rx_index = 0;
    tx_index = 0;
    tx_packets = 0;
    rx_packets = 0;
    memory_barrier();

    uint32_t init_address = (uint32_t)(uintptr_t)&init_block;
    pcnet_csr_write(1, (uint16_t)init_address);
    pcnet_csr_write(2, (uint16_t)(init_address >> 16));
    pcnet_csr_write(0, CSR0_INIT);
    int initialized = 0;
    for (int i = 0; i < POLL_LIMIT; i++) {
        if (pcnet_csr_read(0) & CSR0_IDON) {
            initialized = 1;
            break;
        }
        io_wait();
    }
    if (!initialized)
        return -1;

    pcnet_csr_write(0, CSR0_IDON);
    pcnet_csr_write(0, CSR0_STRT);
    serial_puts("[pcnet] AMD PCnet/VLANCE io=");
    serial_puthex(io_base);
    serial_puts(" MAC=");
    for (uint32_t i = 0; i < 6; i++) {
        serial_puthex(dev->mac[i]);
        if (i != 5)
            serial_putc(':');
    }
    serial_puts("\n");
    return 0;
}

static struct netdev pcnet_dev = {
    .name = "pcnet",
    .priv = 0,
    .init = pcnet_hw_init,
    .send = pcnet_send,
    .recv = pcnet_recv,
};

int pcnet_init_device(void) {
    if (pcnet_dev.init(&pcnet_dev) < 0)
        return -1;
    netdev_register(&pcnet_dev);
    return 0;
}
