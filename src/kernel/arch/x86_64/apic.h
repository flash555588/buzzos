#ifndef BUZZOS_APIC_H
#define BUZZOS_APIC_H

#include <stdint.h>

/*
 * ACPI tables are copied/discovered before paging; the controller is enabled
 * after the APIC MMIO window has been mapped.
 */
void apic_discover(uint32_t multiboot_info);
int apic_initialize(void);
int apic_available(void);
int apic_route_pci_irq(uint8_t irq, uint8_t gsi, uint8_t vector);
void apic_mask_irq(uint8_t irq);
void apic_unmask_irq(uint8_t irq);
void apic_eoi(void);

#endif
