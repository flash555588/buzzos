#include "pci.h"
#include "io.h"
#include "irq.h"
#include "serial.h"

enum {
    PCI_CONFIG_ADDRESS = 0xCF8,
    PCI_CONFIG_DATA = 0xCFC,
    PCI_MAX_DEVICES = 64,
    PIRQ_SIGNATURE = 0x52495024u,
    PIRQ_VERSION = 0x0100,
    PIRQ_BIOS_START = 0x000F0000u,
    PIRQ_BIOS_END = 0x00100000u,
};

struct pirq_pin {
    uint8_t link;
    uint16_t bitmap;
} __attribute__((packed));

struct pirq_entry {
    uint8_t bus;
    uint8_t devfn;
    struct pirq_pin irq[4];
    uint8_t slot;
    uint8_t reserved;
} __attribute__((packed));

struct pirq_table {
    uint32_t signature;
    uint16_t version;
    uint16_t size;
    uint8_t router_bus;
    uint8_t router_devfn;
    uint16_t exclusive_irqs;
    uint16_t router_vendor;
    uint16_t router_device;
    uint32_t miniport_data;
    uint8_t reserved[11];
    uint8_t checksum;
    struct pirq_entry entries[];
} __attribute__((packed));

struct intx_route {
    uint8_t irq;
    uint8_t link;
    uint8_t root_bus;
    uint8_t root_device;
    uint8_t root_pin;
    uint16_t bitmap;
};

static struct pci_device devices[PCI_MAX_DEVICES];
static uint32_t device_count;
static const struct pirq_table *pirq_table;

static uint32_t pci_address(uint8_t bus, uint8_t device, uint8_t function,
                            uint8_t reg) {
    return 0x80000000u | ((uint32_t)bus << 16) |
           ((uint32_t)device << 11) | ((uint32_t)function << 8) |
           (reg & 0xFCu);
}

static uint32_t pci_raw_read32(uint8_t bus, uint8_t device, uint8_t function,
                               uint8_t reg) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, device, function, reg));
    return inl(PCI_CONFIG_DATA);
}

static uint8_t pci_raw_read8(uint8_t bus, uint8_t device, uint8_t function,
                             uint8_t reg) {
    uint32_t value = pci_raw_read32(bus, device, function, reg);
    return (uint8_t)(value >> ((reg & 3u) * 8u));
}

static int pirq_checksum_ok(const struct pirq_table *table) {
    const uint8_t *bytes = (const uint8_t *)table;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < table->size; i++)
        sum = (uint8_t)(sum + bytes[i]);
    return sum == 0;
}

/*
 * PCI BIOS 2.1 places the routing table on a 16-byte boundary in the BIOS
 * region.  This is the same discovery path used by Linux x86 when ACPI/MP
 * routing is unavailable.
 */
static const struct pirq_table *pirq_find_table(void) {
    for (uintptr_t address = PIRQ_BIOS_START;
         address + sizeof(struct pirq_table) <= PIRQ_BIOS_END;
         address += 16u) {
        const struct pirq_table *table =
            (const struct pirq_table *)address;
        if (table->signature != PIRQ_SIGNATURE ||
            table->version != PIRQ_VERSION ||
            table->size < sizeof(*table) ||
            (table->size & 15u) != 0u ||
            address + table->size > PIRQ_BIOS_END)
            continue;
        if (pirq_checksum_ok(table))
            return table;
    }
    return 0;
}

void pci_init(void) {
    device_count = 0;
    pirq_table = pirq_find_table();
    for (uint32_t bus = 0; bus < 256 && device_count < PCI_MAX_DEVICES; bus++) {
        for (uint32_t slot = 0; slot < 32 && device_count < PCI_MAX_DEVICES;
             slot++) {
            uint32_t first = pci_raw_read32((uint8_t)bus, (uint8_t)slot, 0, 0);
            if ((first & 0xFFFFu) == 0xFFFFu)
                continue;

            uint8_t header = (uint8_t)(pci_raw_read32(
                (uint8_t)bus, (uint8_t)slot, 0, 0x0C) >> 16);
            uint32_t functions = (header & 0x80u) ? 8u : 1u;
            for (uint32_t fn = 0; fn < functions &&
                                  device_count < PCI_MAX_DEVICES; fn++) {
                uint32_t id = pci_raw_read32((uint8_t)bus, (uint8_t)slot,
                                             (uint8_t)fn, 0);
                if ((id & 0xFFFFu) == 0xFFFFu)
                    continue;
                uint32_t class_info = pci_raw_read32(
                    (uint8_t)bus, (uint8_t)slot, (uint8_t)fn, 0x08);
                uint32_t irq_info = pci_raw_read32(
                    (uint8_t)bus, (uint8_t)slot, (uint8_t)fn, 0x3C);
                struct pci_device *dev = &devices[device_count++];
                dev->bus = (uint8_t)bus;
                dev->device = (uint8_t)slot;
                dev->function = (uint8_t)fn;
                dev->vendor_id = (uint16_t)id;
                dev->device_id = (uint16_t)(id >> 16);
                dev->class_code = (uint8_t)(class_info >> 24);
                dev->subclass = (uint8_t)(class_info >> 16);
                dev->prog_if = (uint8_t)(class_info >> 8);
                dev->header_type = fn == 0 ? (uint8_t)(header & 0x7Fu) :
                    (uint8_t)((pci_raw_read32((uint8_t)bus, (uint8_t)slot,
                                              (uint8_t)fn, 0x0C) >> 16) &
                              0x7Fu);
                dev->secondary_bus = 0;
                if (dev->header_type == 1u) {
                    uint32_t buses = pci_raw_read32(
                        (uint8_t)bus, (uint8_t)slot, (uint8_t)fn, 0x18);
                    dev->secondary_bus = (uint8_t)(buses >> 8);
                }
                dev->irq_line = (uint8_t)irq_info;
                dev->irq_pin = (uint8_t)(irq_info >> 8);
            }
        }
    }
    serial_puts("[pci] devices=");
    serial_puthex(device_count);
    if (pirq_table) {
        serial_puts(" $PIR=");
        serial_puthex((uint32_t)(uintptr_t)pirq_table);
        serial_puts(" router=");
        serial_puthex(pirq_table->router_bus);
        serial_putc(':');
        serial_puthex(pirq_table->router_devfn >> 3);
        serial_putc('.');
        serial_puthex(pirq_table->router_devfn & 7u);
        serial_putc(' ');
        serial_puthex(pirq_table->router_vendor);
        serial_putc(':');
        serial_puthex(pirq_table->router_device);
    }
    serial_puts("\n");
}

uint32_t pci_config_read32(const struct pci_device *dev, uint8_t reg) {
    if (!dev)
        return 0xFFFFFFFFu;
    return pci_raw_read32(dev->bus, dev->device, dev->function, reg);
}

uint8_t pci_config_read8(const struct pci_device *dev, uint8_t reg) {
    uint32_t value = pci_config_read32(dev, reg);
    return (uint8_t)(value >> ((reg & 3u) * 8u));
}

uint16_t pci_config_read16(const struct pci_device *dev, uint8_t reg) {
    uint32_t value = pci_config_read32(dev, reg);
    return (uint16_t)(value >> ((reg & 2u) * 8u));
}

void pci_config_write32(const struct pci_device *dev, uint8_t reg,
                        uint32_t value) {
    if (!dev)
        return;
    outl(PCI_CONFIG_ADDRESS,
         pci_address(dev->bus, dev->device, dev->function, reg));
    outl(PCI_CONFIG_DATA, value);
}

void pci_config_write16(const struct pci_device *dev, uint8_t reg,
                        uint16_t value) {
    if (!dev)
        return;
    outl(PCI_CONFIG_ADDRESS,
         pci_address(dev->bus, dev->device, dev->function, reg));
    outw((uint16_t)(PCI_CONFIG_DATA + (reg & 2u)), value);
}

uint32_t pci_bar(const struct pci_device *dev, uint8_t index) {
    if (index >= 6)
        return 0;
    return pci_config_read32(dev, (uint8_t)(0x10u + index * 4u));
}

static const struct pci_device *pci_find_bdf(uint8_t bus, uint8_t devfn) {
    for (uint32_t i = 0; i < device_count; i++) {
        if (devices[i].bus == bus &&
            devices[i].device == (uint8_t)(devfn >> 3) &&
            devices[i].function == (uint8_t)(devfn & 7u))
            return &devices[i];
    }
    return 0;
}

static const struct pci_device *pci_parent_bridge(uint8_t bus) {
    for (uint32_t i = 0; i < device_count; i++) {
        if (devices[i].class_code == 0x06u &&
            devices[i].subclass == 0x04u &&
            devices[i].secondary_bus == bus)
            return &devices[i];
    }
    return 0;
}

static const struct pirq_entry *pirq_find_entry(uint8_t bus,
                                                uint8_t device) {
    if (!pirq_table)
        return 0;
    uint32_t count =
        (pirq_table->size - (uint32_t)sizeof(*pirq_table)) /
        (uint32_t)sizeof(struct pirq_entry);
    for (uint32_t i = 0; i < count; i++) {
        const struct pirq_entry *entry = &pirq_table->entries[i];
        if (entry->bus == bus && (entry->devfn >> 3) == device)
            return entry;
    }
    return 0;
}

static uint8_t pirq_router_irq(uint8_t link) {
    if (!pirq_table)
        return 0;
    const struct pci_device *router =
        pci_find_bdf(pirq_table->router_bus, pirq_table->router_devfn);
    if (!router || router->vendor_id != pirq_table->router_vendor)
        return 0;

    /*
     * Intel PIIX/ICH tables use the link byte as the PCI configuration-space
     * offset of a PIRQ route control register.  A value below 16 is the
     * selected legacy IRQ; bit 7 disables a link.  VMware's 8086:122e PIIX
     * follows this standard layout.
     */
    if (router->vendor_id == 0x8086u && link >= 0x40u) {
        uint8_t value = pci_raw_read8(router->bus, router->device,
                                      router->function, link);
        return value < 16u ? value : 0;
    }
    return 0;
}

static int pci_resolve_intx(const struct pci_device *dev,
                            struct intx_route *route) {
    if (!dev || !route || dev->irq_pin < 1u || dev->irq_pin > 4u)
        return -1;

    const struct pci_device *current = dev;
    uint8_t pin = dev->irq_pin;
    const struct pirq_entry *entry = pirq_find_entry(current->bus,
                                                     current->device);
    for (uint32_t depth = 0; !entry && depth < 8u; depth++) {
        const struct pci_device *bridge = pci_parent_bridge(current->bus);
        if (!bridge)
            break;
        pin = (uint8_t)(((pin - 1u + current->device) & 3u) + 1u);
        current = bridge;
        entry = pirq_find_entry(current->bus, current->device);
    }
    if (!entry)
        return -1;

    const struct pirq_pin *pirq = &entry->irq[pin - 1u];
    if (!pirq->link)
        return -1;
    uint8_t irq = pirq_router_irq(pirq->link);
    if (irq < 3u || irq >= 16u || !(pirq->bitmap & (1u << irq)))
        return -1;

    route->irq = irq;
    route->link = pirq->link;
    route->root_bus = entry->bus;
    route->root_device = (uint8_t)(entry->devfn >> 3);
    route->root_pin = pin;
    route->bitmap = pirq->bitmap;
    return 0;
}

void pci_enable_device(const struct pci_device *dev, int io, int memory,
                       int bus_master) {
    uint16_t command = pci_config_read16(dev, 0x04);
    if (io)
        command |= 1u;
    if (memory)
        command |= 2u;
    if (bus_master)
        command |= 4u;
    pci_config_write16(dev, 0x04, command);
}

static void pci_disable_message_interrupts(const struct pci_device *dev) {
    if (!(pci_config_read16(dev, 0x06) & 0x10u))
        return;
    uint8_t capability =
        (uint8_t)(pci_raw_read8(dev->bus, dev->device, dev->function,
                               0x34u) & 0xFCu);
    for (uint32_t hop = 0; capability >= 0x40u && hop < 48u; hop++) {
        uint8_t id = pci_raw_read8(dev->bus, dev->device, dev->function,
                                   capability);
        uint8_t next =
            (uint8_t)(pci_raw_read8(dev->bus, dev->device, dev->function,
                                   (uint8_t)(capability + 1u)) & 0xFCu);
        if (id == 0x05u) {
            uint16_t control =
                pci_config_read16(dev, (uint8_t)(capability + 2u));
            pci_config_write16(dev, (uint8_t)(capability + 2u),
                               control & (uint16_t)~1u);
        } else if (id == 0x11u) {
            uint16_t control =
                pci_config_read16(dev, (uint8_t)(capability + 2u));
            pci_config_write16(dev, (uint8_t)(capability + 2u),
                               control & (uint16_t)~0x8000u);
        }
        if (!next || next == capability)
            break;
        capability = next;
    }
}

int pci_enable_intx(const struct pci_device *dev) {
    if (!dev || dev->irq_pin < 1u || dev->irq_pin > 4u ||
        dev->irq_line < 3u || dev->irq_line >= 16u)
        return -1;

    /*
     * Conventional PCI INTx is active-low and level-triggered.  Resolve its
     * bridge swizzle and firmware PIRQ link instead of treating either the
     * pin or PCI_INTERRUPT_LINE as an I/O APIC GSI.
     */
    struct intx_route route;
    if (pci_resolve_intx(dev, &route) < 0 ||
        route.irq != dev->irq_line ||
        irq_route_pci_legacy(route.irq) < 0)
        return -1;
    /* INTx and MSI/MSI-X must never remain active at the same time. */
    pci_disable_message_interrupts(dev);
    uint16_t command = pci_config_read16(dev, 0x04);
    pci_config_write16(dev, 0x04, command & (uint16_t)~0x0400u);
    irq_enable_line(route.irq);
    serial_puts("[pci] INTx ");
    serial_puthex(dev->bus);
    serial_putc(':');
    serial_puthex(dev->device);
    serial_putc('.');
    serial_puthex(dev->function);
    serial_puts(" pin=");
    serial_puthex(dev->irq_pin);
    serial_puts(" root=");
    serial_puthex(route.root_bus);
    serial_putc(':');
    serial_puthex(route.root_device);
    serial_puts(" INT");
    serial_putc((char)('A' + route.root_pin - 1u));
    serial_puts(" link=");
    serial_puthex(route.link);
    serial_puts(" irq=");
    serial_puthex(route.irq);
    serial_puts(" pic-level");
    serial_puts("\n");
    return 0;
}

void pci_disable_intx(const struct pci_device *dev) {
    if (!dev)
        return;
    uint16_t command = pci_config_read16(dev, 0x04);
    pci_config_write16(dev, 0x04, command | 0x0400u);
}

const struct pci_device *pci_find_device(uint16_t vendor_id,
                                         uint16_t device_id) {
    for (uint32_t i = 0; i < device_count; i++) {
        if (devices[i].vendor_id == vendor_id &&
            devices[i].device_id == device_id)
            return &devices[i];
    }
    return 0;
}

const struct pci_device *pci_find_class(uint8_t class_code, uint8_t subclass,
                                        const struct pci_device *after) {
    uint32_t start = 0;
    if (after) {
        for (uint32_t i = 0; i < device_count; i++) {
            if (&devices[i] == after) {
                start = i + 1;
                break;
            }
        }
    }
    for (uint32_t i = start; i < device_count; i++) {
        if (devices[i].class_code == class_code &&
            devices[i].subclass == subclass)
            return &devices[i];
    }
    return 0;
}
