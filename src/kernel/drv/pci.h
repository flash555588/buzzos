#ifndef BUZZOS_PCI_H
#define BUZZOS_PCI_H

#include <stdint.h>

struct pci_device {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t header_type;
    uint8_t secondary_bus;
    uint8_t irq_line;
    uint8_t irq_pin;
};

void pci_init(void);
uint32_t pci_config_read32(const struct pci_device *dev, uint8_t reg);
uint16_t pci_config_read16(const struct pci_device *dev, uint8_t reg);
void pci_config_write32(const struct pci_device *dev, uint8_t reg, uint32_t value);
void pci_config_write16(const struct pci_device *dev, uint8_t reg, uint16_t value);
uint32_t pci_bar(const struct pci_device *dev, uint8_t index);
void pci_enable_device(const struct pci_device *dev, int io, int memory,
                       int bus_master);
int pci_enable_intx(const struct pci_device *dev);
void pci_disable_intx(const struct pci_device *dev);
const struct pci_device *pci_find_device(uint16_t vendor_id,
                                         uint16_t device_id);
const struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass,
                                        const struct pci_device *after);

#endif
