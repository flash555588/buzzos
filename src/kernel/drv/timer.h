#ifndef BUZZOS_TIMER_H
#define BUZZOS_TIMER_H

#include <stdint.h>

/* 8254 Programmable Interval Timer driver.
 *
 * Programs channel 0 to fire IRQ0 at TIMER_HZ, giving the kernel a steady
 * heartbeat. Each tick increments a monotonic counter and drives the
 * preemptive scheduler. This is the foundation for uptime, sleep, and any
 * future time-based facility. */

/* 250 Hz is the conventional desktop-oriented compromise: Doom's 35 Hz
 * cadence is quantized to 4 ms instead of 10 ms, while the simple i386
 * scheduler avoids the overhead of a 1000 Hz context-switch clock. */
#define TIMER_HZ 250u

/* Program the PIT and unmask IRQ0 on the PIC. Call after idt_install()
 * (so the gate exists) and sched_init() (so schedule() is safe). */
void timer_init(void);

/* IRQ0 handler body, invoked from irq_stub_32. Bumps the tick counter and
 * preempts the current task. */
void timer_irq(void);

/* Monotonic tick count since boot. 32-bit is plenty: at 250 Hz it wraps
 * after ~198 days, and 64-bit division would pull in libgcc helpers
 * (__udivdi3) that this freestanding build does not link. */
uint32_t timer_ticks(void);

/* Whole seconds since boot (ticks / TIMER_HZ). */
uint32_t timer_uptime_secs(void);

/* Busy-wait for `ms` milliseconds, yielding the CPU while it waits so other
 * tasks keep running. Requires interrupts enabled. */
void timer_sleep_ms(uint32_t ms);

#endif /* BUZZOS_TIMER_H */
