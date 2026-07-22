#include "io.h"
#include "serial.h"

#define COM1 0x3F8

static int serial_tx_ready(void) {
    /* Bit 5 of Line Status Register = Transmitter Holding Register Empty. */
    return (inb(COM1 + 5) & 0x20) != 0;
}

void serial_init(void) {
    /* Disable interrupts while we configure. */
    outb(COM1 + 1, 0x00);

    /* Enable DLAB, set divisor 1 = 115200 baud.  Boot logging remains
     * synchronous, so the higher standard rate directly shortens stalls. */
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x01);
    outb(COM1 + 1, 0x00);

    /* 8 bits, no parity, one stop bit; clear DLAB. */
    outb(COM1 + 3, 0x03);

    /* Enable FIFO, clear it, with 14-byte threshold. */
    outb(COM1 + 2, 0xC7);

    /* DTR + RTS + OUT2 (needed for IRQ routing later, harmless now). */
    outb(COM1 + 4, 0x0B);

    /* Loopback test: send 0xAE and check it comes back. If the read
     * register doesn't echo it, the UART is broken or missing. */
    outb(COM1 + 4, 0x1E);
    outb(COM1 + 0, 0xAE);
    int good = (inb(COM1 + 0) == 0xAE);
    outb(COM1 + 4, 0x0F);

    (void)good; /* keep simple for now; visible in real hardware */
}

void serial_putc(char c) {
    while (!serial_tx_ready()) {
        /* spin — caller expects a synchronous write */
    }
    outb(COM1 + 0, (uint8_t)c);
}

void serial_puts(const char *s) {
    while (*s) {
        if (*s == '\n') {
            serial_putc('\r');
        }
        serial_putc(*s++);
    }
}

void serial_puthex(uint32_t v) {
    static const char hex[] = "0123456789ABCDEF";
    serial_puts("0x");
    for (int i = 7; i >= 0; i--) {
        serial_putc(hex[(v >> (i * 4)) & 0xF]);
    }
}
