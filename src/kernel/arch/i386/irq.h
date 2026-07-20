#ifndef BUZZOS_IRQ_H
#define BUZZOS_IRQ_H

#include <stdint.h>

static inline uint32_t irq_save(void) {
    uint32_t flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(uint32_t flags) {
    __asm__ volatile("push %0; popf" :: "r"(flags) : "memory", "cc");
}

#endif
