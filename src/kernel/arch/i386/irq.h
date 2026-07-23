#ifndef BUZZOS_IRQ_H
#define BUZZOS_IRQ_H

#include <stdint.h>

enum irq_trigger {
    IRQ_TRIGGER_EDGE = 0,
    IRQ_TRIGGER_LEVEL = 1,
};

/*
 * Return non-zero when the device owned the interrupt. PCI INTx handlers
 * must be shareable: every registered handler on the line is called until
 * all devices have had a chance to deassert their interrupt source.
 */
typedef int (*irq_handler_t)(void *context);

static inline uint32_t irq_save(void) {
    uint32_t flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(uint32_t flags) {
    __asm__ volatile("push %0; popf" :: "r"(flags) : "memory", "cc");
}

int irq_register_handler(uint8_t irq, irq_handler_t handler, void *context,
                         int shared);
void irq_set_trigger(uint8_t irq, enum irq_trigger trigger);
/*
 * Route a PCI INTx line through the legacy PIRQ/8259 path selected by the
 * firmware.  An I/O APIC route must only be used after ACPI/MP firmware has
 * supplied its actual GSI mapping and, for ACPI, _PIC(1) has selected APIC
 * mode.
 */
int irq_route_pci_legacy(uint8_t irq);
void irq_enable_line(uint8_t irq);
void irq_disable_line(uint8_t irq);
void irq_dispatch(uint32_t irq);
uint32_t irq_handled_count(uint8_t irq);
uint32_t irq_spurious_count(uint8_t irq);

#endif
