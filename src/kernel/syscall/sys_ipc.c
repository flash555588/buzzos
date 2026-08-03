#include <stdint.h>
#include "sys_ipc.h"
#include "irq.h"
#include "syscall_internal.h"
#include "task.h"
#include "timer.h"
#include "vfs.h"

enum {
    MAX_FUTEX_WAITERS = 32,
};

struct futex_waiter {
    int used;
    int task_id;
    uint32_t addr;
    int woken;
};

static struct futex_waiter futex_waiters[MAX_FUTEX_WAITERS];

static void futex_enter(void);
static void futex_leave(void);

static void append_char(char *buf, int *pos, int cap, char ch) {
    if (*pos < cap - 1)
        buf[*pos] = ch;
    (*pos)++;
}

static void append_text(char *buf, int *pos, int cap, const char *s) {
    while (s && *s)
        append_char(buf, pos, cap, *s++);
}

static void append_u32_dec(char *buf, int *pos, int cap, uint32_t value) {
    char tmp[12];
    int n = 0;
    if (value == 0) {
        append_char(buf, pos, cap, '0');
        return;
    }
    while (value && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (n > 0)
        append_char(buf, pos, cap, tmp[--n]);
}

static void append_u32_hex(char *buf, int *pos, int cap, uint32_t value) {
    static const char digits[] = "0123456789ABCDEF";
    append_text(buf, pos, cap, "0x");
    for (int shift = 28; shift >= 0; shift -= 4)
        append_char(buf, pos, cap, digits[(value >> shift) & 0xFu]);
}

void futex_cancel_task_locked(int task_id) {
    for (int i = 0; i < MAX_FUTEX_WAITERS; i++) {
        if (futex_waiters[i].used && futex_waiters[i].task_id == task_id) {
            futex_waiters[i].used = 0;
            futex_waiters[i].woken = 1;
        }
    }
}

int futex_status_text(char *buf, int cap) {
    if (!buf || cap <= 0)
        return -1;
    int pos = 0;
    int used = 0;

    futex_enter();
    for (int i = 0; i < MAX_FUTEX_WAITERS; i++)
        if (futex_waiters[i].used)
            used++;

    append_text(buf, &pos, cap, "futex_waiters ");
    append_u32_dec(buf, &pos, cap, (uint32_t)used);
    append_char(buf, &pos, cap, '/');
    append_u32_dec(buf, &pos, cap, (uint32_t)MAX_FUTEX_WAITERS);
    append_char(buf, &pos, cap, '\n');
    append_text(buf, &pos, cap, "SLOT TID ADDR       WOKEN\n");
    for (int i = 0; i < MAX_FUTEX_WAITERS; i++) {
        if (!futex_waiters[i].used)
            continue;
        append_u32_dec(buf, &pos, cap, (uint32_t)i);
        append_char(buf, &pos, cap, ' ');
        append_u32_dec(buf, &pos, cap, (uint32_t)futex_waiters[i].task_id);
        append_char(buf, &pos, cap, ' ');
        append_u32_hex(buf, &pos, cap, futex_waiters[i].addr);
        append_char(buf, &pos, cap, ' ');
        append_u32_dec(buf, &pos, cap, (uint32_t)futex_waiters[i].woken);
        append_char(buf, &pos, cap, '\n');
    }
    futex_leave();

    if (pos > cap - 1)
        pos = cap - 1;
    buf[pos] = 0;
    return pos;
}

/* BuzzOS is single-core today; disabling IRQs makes waiter table updates
 * atomic against scheduler preemption without spinning in the wait path. */
static uint32_t futex_irq_flags;

static void futex_enter(void) {
    futex_irq_flags = irq_save();
}

static void futex_leave(void) {
    irq_restore(futex_irq_flags);
}

static uint32_t futex_ms_to_ticks(uint32_t ms) {
    return (ms / 1000u) * TIMER_HZ
         + ((ms % 1000u) * TIMER_HZ + 999u) / 1000u;
}

static int futex_deadline_reached(uint32_t deadline) {
    return (int32_t)(timer_ticks() - deadline) >= 0;
}

intptr_t sys_pipe(uintptr_t fds_arg, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)b; (void)c; (void)d; (void)e;
    if (!user_range_writable(fds_arg, sizeof(int) * 2))
        return -1;
    return vfs_pipe((int *)(uintptr_t)fds_arg);
}

static int futex_wait_common(uintptr_t addr_arg, uint32_t expected,
                             uint32_t timeout_ms, int use_timeout) {
    if (!user_range_ok(addr_arg, sizeof(int)))
        return -1;
    volatile int *addr = (volatile int *)(uintptr_t)addr_arg;
    uint32_t deadline = 0;
    if (use_timeout) {
        uint32_t ticks = futex_ms_to_ticks(timeout_ms);
        deadline = timer_ticks() + ticks;
        if (!ticks || !deadline)
            deadline = 1;
    }

    int slot = -1;
    int timed_out = 0;
    futex_enter();
    if (*addr != (int)expected) {
        futex_leave();
        return 0;
    }

    for (int i = 0; i < MAX_FUTEX_WAITERS; i++) {
        if (!futex_waiters[i].used) {
            slot = i;
            futex_waiters[i].used = 1;
            futex_waiters[i].task_id = current_task ? current_task->id : 0;
            futex_waiters[i].addr = addr_arg;
            futex_waiters[i].woken = 0;
            break;
        }
    }
    if (slot < 0) {
        futex_leave();
        return -1;
    }

    for (;;) {
        if (futex_waiters[slot].woken || *addr != (int)expected)
            break;
        if (use_timeout && futex_deadline_reached(deadline)) {
            timed_out = 1;
            break;
        }
        if (use_timeout)
            task_block_current_until(deadline);
        else
            task_block_current();
        futex_enter();
    }

    futex_waiters[slot].used = 0;
    futex_leave();
    return timed_out ? -2 : 0;
}

intptr_t sys_futex_wait(uintptr_t addr_arg, uintptr_t expected, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)c; (void)d; (void)e;
    return futex_wait_common(addr_arg, expected, 0, 0);
}

intptr_t sys_futex_wait_timeout(uintptr_t addr_arg, uintptr_t expected, uintptr_t timeout_ms, uintptr_t d, uintptr_t e) {
    (void)d; (void)e;
    return futex_wait_common(addr_arg, expected, timeout_ms, 1);
}

intptr_t sys_futex_wake(uintptr_t addr_arg, uintptr_t count, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)c; (void)d; (void)e;
    if (!user_range_ok(addr_arg, sizeof(int)))
        return -1;
    int woke = 0;
    futex_enter();
    for (int i = 0; i < MAX_FUTEX_WAITERS; i++) {
        if (futex_waiters[i].used && futex_waiters[i].addr == addr_arg && !futex_waiters[i].woken) {
            futex_waiters[i].woken = 1;
            task_wake(futex_waiters[i].task_id);
            woke++;
            if (count && woke >= (int)count)
                break;
        }
    }
    futex_leave();
    return woke;
}
