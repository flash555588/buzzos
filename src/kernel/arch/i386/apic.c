#include <stddef.h>
#include "apic.h"
#include "irq.h"
#include "serial.h"

enum {
    MULTIBOOT_TAG_END = 0,
    MULTIBOOT_TAG_ACPI_OLD = 14,
    MULTIBOOT_TAG_ACPI_NEW = 15,

    IA32_APIC_BASE_MSR = 0x1B,
    IA32_APIC_BASE_ENABLE = 1u << 11,

    LAPIC_ID = 0x020,
    LAPIC_VERSION = 0x030,
    LAPIC_TPR = 0x080,
    LAPIC_EOI = 0x0B0,
    LAPIC_SVR = 0x0F0,
    LAPIC_ESR = 0x280,
    LAPIC_LVT_CMCI = 0x2F0,
    LAPIC_LVT_TIMER = 0x320,
    LAPIC_LVT_THERMAL = 0x330,
    LAPIC_LVT_PERFORMANCE = 0x340,
    LAPIC_SVR_ENABLE = 1u << 8,
    LAPIC_LVT_LINT0 = 0x350,
    LAPIC_LVT_LINT1 = 0x360,
    LAPIC_LVT_ERROR = 0x370,
    LAPIC_LVT_DELIVERY_EXTINT = 7u << 8,
    LAPIC_LVT_MASKED = 1u << 16,

    IOAPIC_REG_ID = 0x00,
    IOAPIC_REG_VERSION = 0x01,
    IOAPIC_REDIR_BASE = 0x10,
    IOAPIC_REDIR_POLARITY_LOW = 1u << 13,
    IOAPIC_REDIR_TRIGGER_LEVEL = 1u << 15,
    IOAPIC_REDIR_MASK = 1u << 16,
};

struct multiboot_tag {
    uint32_t type;
    uint32_t size;
} __attribute__((packed));

struct acpi_rsdp {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed));

struct acpi_sdt_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct acpi_madt {
    struct acpi_sdt_header header;
    uint32_t local_apic_address;
    uint32_t flags;
    uint8_t entries[];
} __attribute__((packed));

static uint32_t lapic_address;
static uint32_t ioapic_address;
static uint32_t ioapic_gsi_base;
static uint8_t ioapic_max_redirection;
static uint8_t local_apic_id;
static uint16_t routed_irqs;
static int discovered;
static int initialized;

static int bytes_equal(const char *left, const char *right, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (left[i] != right[i])
            return 0;
    }
    return 1;
}

static int checksum_ok(const void *data, uint32_t length) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < length; i++)
        sum = (uint8_t)(sum + bytes[i]);
    return sum == 0;
}

static const struct acpi_rsdp *find_rsdp(uint32_t multiboot_info) {
    const struct acpi_rsdp *old = 0;
    struct multiboot_tag *tag =
        (struct multiboot_tag *)(uintptr_t)(multiboot_info + 8u);
    while (tag->type != MULTIBOOT_TAG_END) {
        if (tag->size < sizeof(*tag))
            break;
        const struct acpi_rsdp *candidate =
            (const struct acpi_rsdp *)((const uint8_t *)tag + sizeof(*tag));
        if (tag->type == MULTIBOOT_TAG_ACPI_NEW)
            return candidate;
        if (tag->type == MULTIBOOT_TAG_ACPI_OLD)
            old = candidate;
        tag = (struct multiboot_tag *)(uintptr_t)
            (((uint32_t)(uintptr_t)tag + tag->size + 7u) & ~7u);
    }
    return old;
}

static const struct acpi_sdt_header *find_madt(
    const struct acpi_rsdp *rsdp) {
    if (!rsdp || !bytes_equal(rsdp->signature, "RSD PTR ", 8) ||
        !checksum_ok(rsdp, 20))
        return 0;
    const struct acpi_sdt_header *rsdt =
        (const struct acpi_sdt_header *)(uintptr_t)rsdp->rsdt_address;
    if (!rsdt || !bytes_equal(rsdt->signature, "RSDT", 4) ||
        rsdt->length < sizeof(*rsdt) || rsdt->length > 1024u * 1024u ||
        !checksum_ok(rsdt, rsdt->length))
        return 0;
    uint32_t entries =
        (rsdt->length - (uint32_t)sizeof(*rsdt)) / sizeof(uint32_t);
    const uint32_t *tables =
        (const uint32_t *)((const uint8_t *)rsdt + sizeof(*rsdt));
    for (uint32_t i = 0; i < entries; i++) {
        const struct acpi_sdt_header *table =
            (const struct acpi_sdt_header *)(uintptr_t)tables[i];
        if (!table || table->length < sizeof(*table) ||
            table->length > 1024u * 1024u)
            continue;
        if (bytes_equal(table->signature, "APIC", 4) &&
            checksum_ok(table, table->length))
            return table;
    }
    return 0;
}

void apic_discover(uint32_t multiboot_info) {
    discovered = 0;
    const struct acpi_sdt_header *header =
        find_madt(find_rsdp(multiboot_info));
    if (!header || header->length < sizeof(struct acpi_madt))
        return;
    const struct acpi_madt *madt = (const struct acpi_madt *)header;
    lapic_address = madt->local_apic_address;
    const uint8_t *entry = madt->entries;
    const uint8_t *end = (const uint8_t *)madt + header->length;
    while (entry + 2u <= end) {
        uint8_t type = entry[0];
        uint8_t length = entry[1];
        if (length < 2u || entry + length > end)
            break;
        if (type == 1u && length >= 12u && !ioapic_address) {
            const uint32_t *words = (const uint32_t *)(entry + 4u);
            ioapic_address = words[0];
            ioapic_gsi_base = words[1];
        }
        entry += length;
    }
    if (lapic_address && ioapic_address)
        discovered = 1;
}

static void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                  uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf));
}

static uint64_t read_msr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static void write_msr(uint32_t msr, uint64_t value) {
    __asm__ volatile("wrmsr"
                     :
                     : "c"(msr), "a"((uint32_t)value),
                       "d"((uint32_t)(value >> 32)));
}

static uint32_t lapic_read(uint32_t reg) {
    return *(volatile uint32_t *)(uintptr_t)(lapic_address + reg);
}

static void lapic_write(uint32_t reg, uint32_t value) {
    *(volatile uint32_t *)(uintptr_t)(lapic_address + reg) = value;
    (void)lapic_read(LAPIC_ID);
}

static uint32_t ioapic_read(uint8_t reg) {
    *(volatile uint32_t *)(uintptr_t)ioapic_address = reg;
    return *(volatile uint32_t *)(uintptr_t)(ioapic_address + 0x10u);
}

static void ioapic_write(uint8_t reg, uint32_t value) {
    *(volatile uint32_t *)(uintptr_t)ioapic_address = reg;
    *(volatile uint32_t *)(uintptr_t)(ioapic_address + 0x10u) = value;
}

static uint8_t irq_pins[16];

static int gsi_to_pin(uint8_t gsi, uint8_t *pin) {
    if (!initialized || gsi < ioapic_gsi_base)
        return -1;
    uint32_t candidate = (uint32_t)gsi - ioapic_gsi_base;
    if (candidate > ioapic_max_redirection)
        return -1;
    *pin = (uint8_t)candidate;
    return 0;
}

int apic_initialize(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    if (!discovered || !(edx & (1u << 9)))
        return -1;

    uint32_t flags = irq_save();
    uint64_t base = read_msr(IA32_APIC_BASE_MSR);
    base &= ~0xFFFFF000ull;
    base |= (uint64_t)lapic_address | IA32_APIC_BASE_ENABLE;
    write_msr(IA32_APIC_BASE_MSR, base);

    lapic_write(LAPIC_TPR, 0);
    /*
     * Firmware may leave local vectors programmed for another OS.  BuzzOS
     * currently uses only the legacy PIC through LINT0, so mask every other
     * LVT before enabling the LAPIC.  Otherwise a thermal/performance/error
     * event enters an unowned high IDT vector and halts the kernel.
     */
    uint8_t max_lvt = (uint8_t)(lapic_read(LAPIC_VERSION) >> 16);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    if (max_lvt >= 1u)
        lapic_write(LAPIC_LVT_THERMAL, LAPIC_LVT_MASKED);
    if (max_lvt >= 2u)
        lapic_write(LAPIC_LVT_PERFORMANCE, LAPIC_LVT_MASKED);
    if (max_lvt >= 4u)
        lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_MASKED);
    if (max_lvt >= 5u)
        lapic_write(LAPIC_LVT_ERROR, LAPIC_LVT_MASKED);
    if (max_lvt >= 6u)
        lapic_write(LAPIC_LVT_CMCI, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);
    /* Preserve legacy PIT/keyboard delivery through the PIC's INTR output
     * while PCI devices use I/O APIC fixed delivery. */
    lapic_write(LAPIC_LVT_LINT0, LAPIC_LVT_DELIVERY_EXTINT);
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | 0xFFu);
    local_apic_id = (uint8_t)(lapic_read(LAPIC_ID) >> 24);

    uint32_t version = ioapic_read(IOAPIC_REG_VERSION);
    ioapic_max_redirection = (uint8_t)((version >> 16) & 0xFFu);
    initialized = 1;
    irq_restore(flags);

    serial_puts("[apic] LAPIC=");
    serial_puthex(lapic_address);
    serial_puts(" id=");
    serial_puthex(local_apic_id);
    serial_puts(" IOAPIC=");
    serial_puthex(ioapic_address);
    serial_puts(" gsi=");
    serial_puthex(ioapic_gsi_base);
    serial_puts(" max=");
    serial_puthex(ioapic_max_redirection);
    serial_puts("\n");
    return 0;
}

int apic_available(void) {
    return initialized;
}

int apic_route_pci_irq(uint8_t irq, uint8_t gsi, uint8_t vector) {
    uint8_t pin;
    if (irq >= 16u || gsi_to_pin(gsi, &pin) < 0)
        return -1;
    uint32_t flags = irq_save();
    uint8_t low_reg = (uint8_t)(IOAPIC_REDIR_BASE + pin * 2u);
    ioapic_write((uint8_t)(low_reg + 1u),
                 (uint32_t)local_apic_id << 24);
    ioapic_write(low_reg, vector | IOAPIC_REDIR_POLARITY_LOW |
                          IOAPIC_REDIR_TRIGGER_LEVEL | IOAPIC_REDIR_MASK);
    irq_pins[irq] = pin;
    routed_irqs |= (uint16_t)(1u << irq);
    irq_restore(flags);
    return 0;
}

static void set_irq_mask(uint8_t irq, int masked) {
    if (irq >= 16u || !(routed_irqs & (uint16_t)(1u << irq)))
        return;
    uint8_t pin = irq_pins[irq];
    uint8_t low_reg = (uint8_t)(IOAPIC_REDIR_BASE + pin * 2u);
    uint32_t low = ioapic_read(low_reg);
    if (masked)
        low |= IOAPIC_REDIR_MASK;
    else
        low &= ~IOAPIC_REDIR_MASK;
    ioapic_write(low_reg, low);
}

void apic_mask_irq(uint8_t irq) {
    set_irq_mask(irq, 1);
}

void apic_unmask_irq(uint8_t irq) {
    set_irq_mask(irq, 0);
}

void apic_eoi(void) {
    if (initialized)
        lapic_write(LAPIC_EOI, 0);
}
