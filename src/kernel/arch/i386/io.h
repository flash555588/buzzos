#ifndef BUZZOS_IO_H
#define BUZZOS_IO_H

#include <stdint.h>

/* I/O port access. Used by serial, PIC, keyboard, PIT — anything that talks
 * to legacy PC hardware. Wrapped in `volatile` so the compiler keeps the
 * `in` / `out` instructions; without it, both inline asm and the compiler
 * will happily delete "redundant" reads/writes. */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

/* Tiny pause used between I/O port operations. Writing to an unused port
 * (0x80 on POST code) gives older peripherals time to settle. */
static inline void io_wait(void) {
    outb(0x80, 0);
}


#endif
