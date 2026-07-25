#include "timer.h"
#include "io.h"
#include "irq.h"
#include "pci.h"
#include "serial.h"
#include "task.h"

/* 8254 PIT ports and control values. */
enum {
    PIT_CH0_DATA = 0x40,
    PIT_CMD      = 0x43,
    /* channel 0, lobyte/hibyte access, mode 3 (square wave), binary */
    PIT_CMD_CH0_MODE3 = 0x36,
    PIT_BASE_FREQ     = 1193182u,  /* input clock to the PIT */

    PIC1_DATA = 0x21,
    ACPI_PM_FREQUENCY = 3579545u,
};

static volatile uint32_t ticks;
static uint16_t acpi_pm_port;
static uint32_t acpi_pm_last, acpi_pm_remainder;

static void acpi_pm_init(void) {
    /* SeaBIOS exposes the PIIX4 power-management function used by QEMU's
     * pc machine.  Its 24-bit PM timer is free-running and, unlike IRQ0,
     * does not lose elapsed time while another interrupt is in service. */
    const struct pci_device *pm = pci_find_device(0x8086, 0x7113);
    if (pm) {
        uint32_t base = pci_config_read32(pm, 0x40) & 0x0000FFC0u;
        if (base)
            acpi_pm_port = (uint16_t)(base + 8u);
    }
    if (acpi_pm_port) {
        acpi_pm_last = inl(acpi_pm_port) & 0x00FFFFFFu;
        serial_puts("[timer] ACPI PM clock port=");
        serial_puthex(acpi_pm_port);
        serial_puts("\n");
    } else {
        serial_puts("[timer] ACPI PM clock unavailable; PIT fallback\n");
    }
}

static void update_elapsed_time(void) {
    if (!acpi_pm_port) return;
    uint32_t now = inl(acpi_pm_port) & 0x00FFFFFFu;
    uint32_t delta = (now - acpi_pm_last) & 0x00FFFFFFu;
    acpi_pm_last = now;
    /* delta is below 2^24; at 250 Hz this product safely fits uint32_t. */
    uint32_t scaled = delta * TIMER_HZ + acpi_pm_remainder;
    ticks += scaled / ACPI_PM_FREQUENCY;
    acpi_pm_remainder = scaled % ACPI_PM_FREQUENCY;
}

void timer_init(void) {
    acpi_pm_init();
    uint32_t divisor = PIT_BASE_FREQ / TIMER_HZ;
    if (divisor > 0xFFFF) divisor = 0xFFFF;

    outb(PIT_CMD, PIT_CMD_CH0_MODE3);
    outb(PIT_CH0_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0_DATA, (uint8_t)((divisor >> 8) & 0xFF));

    /* Unmask IRQ0 (timer) on the master PIC, leaving other masked lines
     * untouched. The keyboard (IRQ1) was already unmasked in pic_remap. */
    uint8_t mask = inb(PIC1_DATA);
    mask &= (uint8_t)~0x01;
    outb(PIC1_DATA, mask);

    serial_puts("[timer] PIT @ ");
    serial_puthex(TIMER_HZ);
    serial_puts(" Hz, divisor=");
    serial_puthex(divisor);
    serial_puts("\n");
}

void timer_irq(void) {
    /* How many scheduler jiffies this IRQ accounts for.  With the ACPI PM
     * timebase, one IRQ can advance several ticks when the guest runs behind
     * real time; charge the running task for all of them so CPU% matches wall
     * time (otherwise cpu_ticks stays at 1/IRQ while jiffies jump). */
    uint32_t before = ticks;
    if (acpi_pm_port)
        update_elapsed_time();
    else
        ticks++;
    uint32_t advanced = ticks - before;
    if (advanced == 0)
        advanced = 1;
    /* Preempt: round-robin to the next ready task. schedule() handles the
     * cli/sti and the no-op case when nothing else is runnable. */
    sched_tick(advanced);
}

uint32_t timer_ticks(void) {
    uint32_t flags = irq_save();
    update_elapsed_time();
    uint32_t result = ticks;
    irq_restore(flags);
    return result;
}

uint32_t timer_uptime_secs(void) {
    return timer_ticks() / TIMER_HZ;
}

void timer_sleep_ms(uint32_t ms) {
    /* Convert ms → ticks, rounding up so a sub-tick sleep still waits.
     * 32-bit math throughout to avoid libgcc 64-bit division helpers. */
    uint32_t want = (ms / 1000u) * TIMER_HZ
                  + ((ms % 1000u) * TIMER_HZ + 999u) / 1000u;
    uint32_t end = timer_ticks() + want;
    while ((int32_t)(timer_ticks() - end) < 0)
        task_sleep_until(end);
}
