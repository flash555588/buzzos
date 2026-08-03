#ifndef BUZZOS_SERIAL_H
#define BUZZOS_SERIAL_H

#include <stdint.h>
/* 16550 UART on COM1. We program the bare minimum: 8-N-1, 38400 baud,
 * FIFO on, IRQ off (we poll). The "off" is important — without an IDT
 * we cannot service interrupts, and a TX interrupt that nobody acks
 * wedges the line. Polling keeps us bootable before the IDT exists. */

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);

/* Cheap non-blocking write of a hex-formatted 32-bit value. Useful for
 * debugging values before printf exists. */
void serial_puthex(uint32_t v);
void serial_puthex64(uint64_t v);

#endif /* BUZZOS_SERIAL_H */
