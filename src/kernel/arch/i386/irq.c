#include <stddef.h>
#include "irq.h"
#include "apic.h"
#include "io.h"

enum {
    LEGACY_IRQS = 16,
    MAX_HANDLERS_PER_IRQ = 4,

    PIC1_COMMAND = 0x20,
    PIC1_DATA = 0x21,
    PIC2_COMMAND = 0xA0,
    PIC2_DATA = 0xA1,
    PIC_EOI = 0x20,

    /* Edge/Level Control Registers used by PCI INTx on PC-compatible
     * chipsets. A set bit selects level-triggered operation. */
    PIC_ELCR1 = 0x4D0,
    PIC_ELCR2 = 0x4D1,
};

struct irq_action {
    irq_handler_t handler;
    void *context;
    uint8_t shared;
};

static struct irq_action actions[LEGACY_IRQS][MAX_HANDLERS_PER_IRQ];
static uint8_t action_count[LEGACY_IRQS];
static uint16_t enabled_lines;
static uint16_t apic_lines;
static uint32_t handled_counts[LEGACY_IRQS];
static uint32_t spurious_counts[LEGACY_IRQS];

static void pic_mask(uint8_t irq) {
    uint16_t port = irq < 8u ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = (uint8_t)(irq & 7u);
    outb(port, (uint8_t)(inb(port) | (1u << bit)));
}

static void pic_unmask(uint8_t irq) {
    if (irq >= 8u)
        outb(PIC1_DATA, (uint8_t)(inb(PIC1_DATA) & ~(1u << 2)));
    uint16_t port = irq < 8u ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = (uint8_t)(irq & 7u);
    outb(port, (uint8_t)(inb(port) & ~(1u << bit)));
}

static void pic_specific_eoi(uint8_t irq) {
    if (irq >= 8u) {
        outb(PIC2_COMMAND, (uint8_t)(0x60u + (irq & 7u)));
        outb(PIC1_COMMAND, 0x62u); /* specific EOI for cascade IRQ2 */
    } else {
        outb(PIC1_COMMAND, (uint8_t)(0x60u + irq));
    }
}

static int pic_spurious(uint8_t irq) {
    if (irq != 7u && irq != 15u)
        return 0;
    uint16_t command = irq == 7u ? PIC1_COMMAND : PIC2_COMMAND;
    outb(command, 0x0Bu); /* OCW3: read in-service register */
    if (inb(command) & 0x80u)
        return 0;
    /*
     * A spurious IRQ7 was never accepted by the master, so it needs no EOI.
     * A spurious IRQ15 did pass through master's cascade input; acknowledge
     * IRQ2 on the master only, never a nonexistent slave in-service bit.
     */
    if (irq == 15u)
        outb(PIC1_COMMAND, 0x62u);
    return 1;
}

int irq_register_handler(uint8_t irq, irq_handler_t handler, void *context,
                         int shared) {
    if (irq >= LEGACY_IRQS || !handler)
        return -1;

    uint32_t flags = irq_save();
    uint8_t count = action_count[irq];
    if (count >= MAX_HANDLERS_PER_IRQ ||
        (count && (!shared || !actions[irq][0].shared))) {
        irq_restore(flags);
        return -1;
    }
    actions[irq][count].handler = handler;
    actions[irq][count].context = context;
    actions[irq][count].shared = shared != 0;
    action_count[irq] = (uint8_t)(count + 1u);
    irq_restore(flags);
    return 0;
}

void irq_set_trigger(uint8_t irq, enum irq_trigger trigger) {
    if (irq >= LEGACY_IRQS)
        return;
    /* IRQ0, IRQ1, IRQ2, IRQ8 and IRQ13 are architecturally reserved and
     * must retain their legacy trigger mode. */
    if (irq == 0u || irq == 1u || irq == 2u || irq == 8u || irq == 13u)
        return;

    uint32_t flags = irq_save();
    uint16_t port = irq < 8u ? PIC_ELCR1 : PIC_ELCR2;
    uint8_t bit = (uint8_t)(1u << (irq & 7u));
    uint8_t value = inb(port);
    if (trigger == IRQ_TRIGGER_LEVEL)
        value |= bit;
    else
        value &= (uint8_t)~bit;
    outb(port, value);
    irq_restore(flags);
}

int irq_route_pci_legacy(uint8_t irq) {
    if (irq >= LEGACY_IRQS)
        return -1;

    /*
     * A BIOS $PIR route terminates at an 8259 IRQ.  Merely finding a MADT
     * does not turn that route into an I/O APIC GSI: ACPI systems normally
     * execute _PIC(1), then evaluate _PRT/link devices for the APIC route.
     * Keep this line on the PIC until that complete path is implemented.
     */
    apic_lines &= (uint16_t)~(1u << irq);
    irq_set_trigger(irq, IRQ_TRIGGER_LEVEL);
    return 0;
}

void irq_enable_line(uint8_t irq) {
    if (irq >= LEGACY_IRQS)
        return;
    uint32_t flags = irq_save();
    enabled_lines |= (uint16_t)(1u << irq);
    if (apic_lines & (uint16_t)(1u << irq))
        apic_unmask_irq(irq);
    else
        pic_unmask(irq);
    irq_restore(flags);
}

void irq_disable_line(uint8_t irq) {
    if (irq >= LEGACY_IRQS)
        return;
    uint32_t flags = irq_save();
    enabled_lines &= (uint16_t)~(1u << irq);
    if (apic_lines & (uint16_t)(1u << irq))
        apic_mask_irq(irq);
    else
        pic_mask(irq);
    irq_restore(flags);
}

/*
 * The 8259 path follows the same important ordering used by Linux:
 * mask the level, acknowledge the PIC, service every handler sharing the
 * line (which clears each device's source), then unmask the line.
 */
void irq_dispatch(uint32_t irq_value) {
    if (irq_value >= LEGACY_IRQS)
        return;
    uint8_t irq = (uint8_t)irq_value;
    int via_apic = (apic_lines & (uint16_t)(1u << irq)) != 0;
    if (!via_apic) {
        if (pic_spurious(irq)) {
            spurious_counts[irq]++;
            return;
        }
        pic_mask(irq);
        pic_specific_eoi(irq);
    }

    int handled = 0;
    uint8_t count = action_count[irq];
    for (uint8_t i = 0; i < count; i++) {
        if (actions[irq][i].handler(actions[irq][i].context))
            handled = 1;
    }
    if (handled)
        handled_counts[irq]++;
    else
        spurious_counts[irq]++;

    if (via_apic)
        apic_eoi();
    else if (enabled_lines & (uint16_t)(1u << irq))
        pic_unmask(irq);
}

uint32_t irq_handled_count(uint8_t irq) {
    return irq < LEGACY_IRQS ? handled_counts[irq] : 0;
}

uint32_t irq_spurious_count(uint8_t irq) {
    return irq < LEGACY_IRQS ? spurious_counts[irq] : 0;
}
