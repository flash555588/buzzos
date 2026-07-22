#include "task.h"
#include "gdt.h"
#include "irq.h"
#include "paging.h"
#include "pmm.h"
#include "serial.h"
#include "syscall.h"
#include "sys_ipc.h"
#include "timer.h"
#include "vfs.h"

/* ------------------------------------------------------------------ */
/*  Assembly helper                                                     */
/* ------------------------------------------------------------------ */
extern void switch_context(uint32_t *old_esp_ptr, uint32_t *new_esp);
extern tss32_t tss;

/* ------------------------------------------------------------------ */
/*  Globals                                                             */
/* ------------------------------------------------------------------ */

static struct task tasks[MAX_TASKS];
static int          num_tasks;
static int          current_id;
struct task        *current_task;
static uint32_t preempt_depth;

#define PROC_UNUSED 0
#define PROC_RUNNING 1
#define PROC_ZOMBIE 2

struct process {
    int used;
    int pid;
    int parent;
    int state;
    int exit_code;
    int console_silent;
    char name[16];
    char cwd[128];
};

static struct process procs[MAX_TASKS];

static void copy_cstr(char *dst, int dst_size, const char *src) {
    int i = 0;
    if (!src) src = "";
    while (i < dst_size - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
    while (++i < dst_size)
        dst[i] = 0;
}

static int task_slot_active(int id) {
    return id >= 0 && id < num_tasks && tasks[id].state != TASK_DEAD;
}

static int task_process_owner(const struct task *task) {
    if (!task)
        return 0;
    if (task->proc_id >= 0 && task->proc_id < MAX_TASKS)
        return task->proc_id;
    if (task->fd_owner >= 0 && task->fd_owner < num_tasks)
        return task->fd_owner;
    return task->id;
}

static int process_has_live_tasks(int owner) {
    for (int i = 0; i < num_tasks; i++) {
        if (task_process_owner(&tasks[i]) == owner && task_slot_active(i))
            return 1;
    }
    return 0;
}

static int process_cr3_referenced(uint32_t cr3, int owner) {
    for (int i = 0; i < num_tasks; i++) {
        if (task_process_owner(&tasks[i]) != owner && tasks[i].cr3 == cr3)
            return 1;
    }
    return 0;
}

static void task_reap_one(int id) {
    if (id <= 0 || id == current_id || id >= num_tasks)
        return;
    if (tasks[id].state != TASK_DEAD)
        return;

    if (tasks[id].kstack) {
        pmm_free_pages((uintptr_t)(tasks[id].kstack - 2 * PAGE_SIZE), 2);
        tasks[id].kstack = 0;
        tasks[id].esp = 0;
        tasks[id].esp0 = 0;
    }

    int owner = task_process_owner(&tasks[id]);
    if (owner != id)
        syscall_release_thread(id);
    if (owner == id && !process_has_live_tasks(id)) {
        syscall_cleanup_process(id);
        uint32_t cr3 = tasks[id].cr3;
        if (cr3 && cr3 != paging_kernel_cr3() && !process_cr3_referenced(cr3, id)) {
            paging_destroy_user_space(cr3);
            for (int i = 0; i < num_tasks; i++) {
                if (task_process_owner(&tasks[i]) == id)
                    tasks[i].cr3 = paging_kernel_cr3();
            }
        }
        vfs_task_reset(id);
    }
}

static int task_slot_reusable(int id) {
    if (id <= 0 || id >= num_tasks)
        return 0;
    if (tasks[id].state != TASK_DEAD)
        return 0;
    if (procs[id].used && procs[id].state == PROC_ZOMBIE)
        return 0;
    task_reap_one(id);
    if (tasks[id].kstack)
        return 0;
    if (task_process_owner(&tasks[id]) == id && process_has_live_tasks(id))
        return 0;
    if (task_process_owner(&tasks[id]) != id && process_has_live_tasks(task_process_owner(&tasks[id])))
        return 0;
    return 1;
}

static void task_reap_dead(void) {
    for (int i = 1; i < num_tasks; i++)
        task_reap_one(i);
}

/* ------------------------------------------------------------------ */
/*  Initialise                                                          */
/* ------------------------------------------------------------------ */

void sched_init(void) {
    num_tasks  = 0;
    current_id = 0;

    /* Task 0 = idle, reusing the boot stack (0x90000) */
    tasks[0].id     = 0;
    tasks[0].state  = TASK_RUNNING;
    tasks[0].kstack = 0x90000;
    tasks[0].esp0   = 0x90000;
    tasks[0].cr3    = paging_current_cr3();
    tasks[0].exit_code = 0;
    tasks[0].console_silent = 0;
    tasks[0].fd_owner = 0;
    tasks[0].proc_id = 0;
    tasks[0].wake_tick = 0;
    for (int i = 0; i < 16; i++) tasks[0].name[i] = 0;
    tasks[0].name[0] = 'i'; tasks[0].name[1] = 'd'; tasks[0].name[2] = 'l';
    tasks[0].name[3] = 'e';
    for (int i = 0; i < 128; i++) tasks[0].cwd[i] = 0;
    tasks[0].cwd[0] = '/';
    procs[0].used = 1;
    procs[0].pid = 0;
    procs[0].parent = 0;
    procs[0].state = PROC_RUNNING;
    procs[0].exit_code = 0;
    procs[0].console_silent = 0;
    copy_cstr(procs[0].name, sizeof(procs[0].name), "idle");
    copy_cstr(procs[0].cwd, sizeof(procs[0].cwd), "/");
    num_tasks = 1;
    current_task = &tasks[0];
    fpu_state_init(tasks[0].fpu_state);

    serial_puts("[sched] init: idle task created\n");
}

/* ------------------------------------------------------------------ */
/*  Create a kernel thread                                              */
/* ------------------------------------------------------------------ */

int task_create(void (*entry)(void), const char *name) {
    return task_create_ex(entry, name, current_task ? current_task->console_silent : 0);
}

int task_create_ex(void (*entry)(void), const char *name, int console_silent) {
    task_reap_dead();

    int id = -1;
    for (int i = 1; i < num_tasks; i++) {
        if (task_slot_reusable(i)) {
            id = i;
            break;
        }
    }
    if (id < 0 && num_tasks < MAX_TASKS)
        id = num_tasks++;
    if (id < 0) return -1;

    /* Allocate 2 pages (8 KiB) for the kernel stack */
    uintptr_t stack_base = pmm_alloc_pages(2);
    if (!stack_base) return -1;

    /* Stack grows down from the top */
    uintptr_t stack_top = stack_base + 2 * 4096;
    uint32_t *sp = (uint32_t *)stack_top;

    /* Build initial stack frame for switch_context */
    *--sp = (uint32_t)entry;   /* "return address" */
    *--sp = 0x00000202;        /* eflags (IF=1) */
    *--sp = 0;                 /* eax */
    *--sp = 0;                 /* ecx */
    *--sp = 0;                 /* edx */
    *--sp = 0;                 /* ebx */
    *--sp = 0;                 /* ebp */
    *--sp = 0;                 /* esi */
    *--sp = 0;                 /* edi */

    tasks[id].id     = id;
    tasks[id].esp    = (uint32_t)sp;
    tasks[id].kstack = (uint32_t)stack_top;
    tasks[id].esp0   = (uint32_t)stack_top;
    tasks[id].cr3    = current_task ? current_task->cr3 : paging_current_cr3();
    tasks[id].exit_code = 0;
    tasks[id].console_silent = console_silent ? 1 : 0;
    tasks[id].fd_owner = id;
    tasks[id].proc_id = id;
    tasks[id].wake_tick = 0;
    int parent = current_task ? task_process_owner(current_task) : 0;
    procs[id].used = 1;
    procs[id].pid = id;
    procs[id].parent = parent;
    procs[id].state = PROC_RUNNING;
    procs[id].exit_code = 0;
    procs[id].console_silent = console_silent ? 1 : 0;
    copy_cstr(procs[id].name, sizeof(procs[id].name), name);
    if (current_task) {
        copy_cstr(procs[id].cwd, sizeof(procs[id].cwd), procs[parent].cwd[0] ? procs[parent].cwd : "/");
    } else {
        copy_cstr(procs[id].cwd, sizeof(procs[id].cwd), "/");
    }
    copy_cstr(tasks[id].cwd, sizeof(tasks[id].cwd), procs[id].cwd);
    vfs_task_reset(id);
    for (int i = 0; i < 16; i++)
        tasks[id].name[i] = 0;
    for (int i = 0; i < 15 && name[i]; i++)
        tasks[id].name[i] = name[i];
    tasks[id].state  = TASK_READY;
    fpu_state_init(tasks[id].fpu_state);

    if (!tasks[id].console_silent) {
        serial_puts("[sched] task ");
        serial_puthex(id);
        serial_puts(" '");
        serial_puts(name);
        serial_puts("' created, esp=0x");
        serial_puthex((uint32_t)sp);
        serial_puts("\n");
    }

    return id;
}

/* ------------------------------------------------------------------ */
/*  Round-robin scheduler                                               */
/* ------------------------------------------------------------------ */

static void schedule(void) {
    uint32_t irq_flags = irq_save();

    uint32_t now = timer_ticks();
    for (int i = 0; i < num_tasks; i++) {
        if (tasks[i].state == TASK_SLEEPING && (int32_t)(now - tasks[i].wake_tick) >= 0) {
            tasks[i].state = TASK_READY;
            tasks[i].wake_tick = 0;
        }
        if (tasks[i].state == TASK_BLOCKED && tasks[i].wake_tick &&
            (int32_t)(now - tasks[i].wake_tick) >= 0) {
            tasks[i].state = TASK_READY;
            tasks[i].wake_tick = 0;
        }
    }

    int next = -1;
    for (int i = 1; i <= num_tasks; i++) {
        int cand = (current_id + i) % num_tasks;
        if (tasks[cand].state == TASK_READY) {
            next = cand;
            break;
        }
    }

    if (next < 0) {
        if (current_task && current_task->state == TASK_RUNNING) {
            irq_restore(irq_flags);
            return;
        }
        next = 0;
    }

    if (next == current_id && current_task->state == TASK_RUNNING) {
        irq_restore(irq_flags);
        return;
    }

    struct task *prev = current_task;
    struct task *task = &tasks[next];

    if (prev->state == TASK_RUNNING)
        prev->state = TASK_READY;
    task->state = TASK_RUNNING;
    current_id  = next;
    current_task = task;
    tss.esp0 = task->esp0;
    if (paging_current_cr3() != task->cr3)
        paging_switch(task->cr3);

    fpu_state_save(prev->fpu_state);
    fpu_state_restore(task->fpu_state);
    switch_context(&prev->esp, (uint32_t *)(uintptr_t)task->esp);

    irq_restore(irq_flags);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

void task_yield(void) {
    if (preempt_depth)
        return;
    schedule();
}

void sched_tick(void) {
    if (preempt_depth)
        return;
    schedule();
}

void task_preempt_disable(void) {
    preempt_depth++;
}

void task_preempt_enable(void) {
    if (preempt_depth)
        preempt_depth--;
}

int task_get_state(int id) {
    if (id < 0 || id >= num_tasks) return -1;
    return tasks[id].state;
}

int task_get_exit_code(int id) {
    if (id < 0 || id >= num_tasks) return -1;
    return tasks[id].exit_code;
}

int task_forget_dead(int id) {
    if (id <= 0 || id >= num_tasks)
        return -1;
    if (tasks[id].state != TASK_DEAD)
        return -1;
    task_reap_one(id);
    if (tasks[id].fd_owner != id)
        tasks[id].fd_owner = -1;
    if (tasks[id].proc_id != id)
        tasks[id].proc_id = -1;
    return 0;
}

int task_get_pid(void) {
    if (!current_task)
        return 0;
    return task_process_owner(current_task);
}

int task_get_tid(void) {
    return current_task ? current_task->id : 0;
}

int task_get_cwd(char *buf, int size) {
    if (!buf || size <= 0)
        return -1;
    int owner = current_task ? task_process_owner(current_task) : 0;
    int i = 0;
    const char *cwd = (owner >= 0 && owner < MAX_TASKS && procs[owner].used && procs[owner].cwd[0])
        ? procs[owner].cwd : "/";
    while (i < size - 1 && cwd[i]) {
        buf[i] = cwd[i];
        i++;
    }
    buf[i] = 0;
    return i;
}

int task_set_cwd(const char *path) {
    if (!path || path[0] != '/')
        return -1;
    int owner = current_task ? task_process_owner(current_task) : 0;
    if (owner < 0 || owner >= MAX_TASKS || !procs[owner].used)
        return -1;
    int len = 0;
    while (path[len]) len++;
    if (len >= 128)
        return -1;
    copy_cstr(procs[owner].cwd, sizeof(procs[owner].cwd), path);
    for (int i = 0; i < num_tasks; i++)
        if (task_process_owner(&tasks[i]) == owner)
            copy_cstr(tasks[i].cwd, sizeof(tasks[i].cwd), path);
    return 0;
}

static int process_matches_wait(int proc, int parent, int pid) {
    if (proc <= 0 || proc >= MAX_TASKS || !procs[proc].used)
        return 0;
    if (procs[proc].parent != parent)
        return 0;
    return pid == -1 || pid == proc;
}

static int process_waitable_child(int parent, int pid) {
    for (int i = 1; i < MAX_TASKS; i++)
        if (process_matches_wait(i, parent, pid))
            return i;
    return -1;
}

static void process_forget(int pid) {
    if (pid <= 0 || pid >= MAX_TASKS || !procs[pid].used)
        return;

    for (int i = 1; i < num_tasks; i++) {
        if (task_process_owner(&tasks[i]) == pid && tasks[i].state == TASK_DEAD) {
            task_reap_one(i);
            tasks[i].fd_owner = -1;
            tasks[i].proc_id = -1;
            tasks[i].wake_tick = 0;
        }
    }
    procs[pid].used = 0;
    procs[pid].state = PROC_UNUSED;
    procs[pid].exit_code = 0;
}

int task_wait_pid(int pid, int *status, int options) {
    (void)options;
    int parent = current_task ? task_process_owner(current_task) : 0;
    if (pid == 0)
        pid = -1;
    if (pid < -1)
        return -1;

    for (;;) {
        int child = process_waitable_child(parent, pid);
        if (child < 0)
            return -1;
        if (procs[child].state == PROC_ZOMBIE) {
            int code = procs[child].exit_code;
            if (status)
                *status = code;
            process_forget(child);
            return child;
        }
        task_yield();
    }
}

void task_set_cr3(int id, uint32_t cr3) {
    if (id < 0 || id >= num_tasks) return;
    tasks[id].cr3 = cr3;
}

void task_set_console_silent(int id, int silent) {
    if (id < 0 || id >= num_tasks) return;
    tasks[id].console_silent = silent ? 1 : 0;
}

void task_set_fd_owner(int id, int owner) {
    if (id < 0 || id >= num_tasks) return;
    if (owner < 0 || owner >= num_tasks) return;
    if (tasks[id].proc_id == id && owner != id && procs[id].used) {
        procs[id].used = 0;
        procs[id].state = PROC_UNUSED;
    }
    tasks[id].fd_owner = owner;
    tasks[id].proc_id = owner;
}

int task_kill(int id) {
    if (id <= 0 || id >= num_tasks)
        return -1;
    if (tasks[id].state == TASK_DEAD)
        return -1;

    int owner = task_process_owner(&tasks[id]);
    if (owner < 0 || owner >= MAX_TASKS)
        owner = id;

    uint32_t irq_flags = irq_save();
    for (int i = 1; i < num_tasks; i++) {
        if (task_process_owner(&tasks[i]) == owner || i == id) {
            futex_cancel_task_locked(i);
            tasks[i].exit_code = -1;
            tasks[i].wake_tick = 0;
            tasks[i].state = TASK_DEAD;
        }
    }
    if (owner > 0 && owner < MAX_TASKS && procs[owner].used) {
        procs[owner].state = PROC_ZOMBIE;
        procs[owner].exit_code = -1;
    }

    if (current_task && current_task->state == TASK_DEAD)
        schedule();
    irq_restore(irq_flags);
    return 0;
}

static void dump_str(void (*putc)(char), const char *s) {
    while (*s) putc(*s++);
}

static void dump_u32(void (*putc)(char), uint32_t v) {
    char buf[11];
    int i = 0;
    if (v == 0) {
        putc('0');
        return;
    }
    while (v && i < 10) {
        buf[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0)
        putc(buf[--i]);
}

void task_dump(void (*putc)(char), int show_dead) {
    dump_str(putc, "PID  STATE    OUT    CODE  NAME\n");
    for (int pid = 0; pid < MAX_TASKS; pid++) {
        if (!procs[pid].used)
            continue;
        if (!show_dead && procs[pid].state == PROC_ZOMBIE)
            continue;
        dump_u32(putc, (uint32_t)pid);
        dump_str(putc, "    ");
        int state = TASK_DEAD;
        for (int i = 0; i < num_tasks; i++) {
            if (task_process_owner(&tasks[i]) != pid)
                continue;
            if (tasks[i].state == TASK_RUNNING) { state = TASK_RUNNING; break; }
            if (tasks[i].state == TASK_READY) state = TASK_READY;
            else if (state == TASK_DEAD && tasks[i].state == TASK_SLEEPING) state = TASK_SLEEPING;
            else if (state == TASK_DEAD && tasks[i].state == TASK_BLOCKED) state = TASK_BLOCKED;
        }
        if (procs[pid].state == PROC_ZOMBIE)
            dump_str(putc, "ZOMBIE   ");
        else if (state == TASK_RUNNING)
            dump_str(putc, "RUNNING  ");
        else if (state == TASK_READY)
            dump_str(putc, "READY    ");
        else if (state == TASK_SLEEPING)
            dump_str(putc, "SLEEP    ");
        else if (state == TASK_BLOCKED)
            dump_str(putc, "BLOCKED  ");
        else
            dump_str(putc, "UNKNOWN  ");
        dump_str(putc, procs[pid].console_silent ? "null   " : "tty    ");
        dump_u32(putc, (uint32_t)procs[pid].exit_code);
        dump_str(putc, "     ");
        dump_str(putc, procs[pid].name);
        putc('\n');
    }
}

static void dump_task_state(void (*putc)(char), int state) {
    if (state == TASK_RUNNING)
        dump_str(putc, "RUNNING  ");
    else if (state == TASK_READY)
        dump_str(putc, "READY    ");
    else if (state == TASK_SLEEPING)
        dump_str(putc, "SLEEP    ");
    else if (state == TASK_BLOCKED)
        dump_str(putc, "BLOCKED  ");
    else if (state == TASK_DEAD)
        dump_str(putc, "DEAD     ");
    else
        dump_str(putc, "UNKNOWN  ");
}

void task_dump_threads(void (*putc)(char), int show_dead) {
    dump_str(putc, "TID  PID  STATE    OUT    NAME\n");
    for (int i = 0; i < num_tasks; i++) {
        if (!show_dead && tasks[i].state == TASK_DEAD)
            continue;
        dump_u32(putc, (uint32_t)i);
        dump_str(putc, "    ");
        dump_u32(putc, (uint32_t)task_process_owner(&tasks[i]));
        dump_str(putc, "    ");
        dump_task_state(putc, tasks[i].state);
        dump_str(putc, tasks[i].console_silent ? "null   " : "tty    ");
        dump_str(putc, tasks[i].name);
        putc('\n');
    }
}

static char *dump_buf_ptr;
static int dump_buf_pos;
static int dump_buf_size;

static void dump_buf_putc(char c) {
    if (dump_buf_pos < dump_buf_size - 1)
        dump_buf_ptr[dump_buf_pos] = c;
    dump_buf_pos++;
}

int task_dump_text(char *buf, int size, int show_dead) {
    if (!buf || size <= 0)
        return -1;
    dump_buf_ptr = buf;
    dump_buf_pos = 0;
    dump_buf_size = size;
    task_dump(dump_buf_putc, show_dead);
    int n = dump_buf_pos;
    if (n > size - 1)
        n = size - 1;
    buf[n] = 0;
    return n;
}

int task_dump_threads_text(char *buf, int size, int show_dead) {
    if (!buf || size <= 0)
        return -1;
    dump_buf_ptr = buf;
    dump_buf_pos = 0;
    dump_buf_size = size;
    task_dump_threads(dump_buf_putc, show_dead);
    int n = dump_buf_pos;
    if (n > size - 1)
        n = size - 1;
    buf[n] = 0;
    return n;
}

void task_exit(void) {
    task_exit_code(0);
}

void task_exit_code(int code) {
    __asm__ volatile("cli");
    int owner = task_process_owner(current_task);
    futex_cancel_task_locked(current_task->id);
    current_task->exit_code = code;
    current_task->wake_tick = 0;
    current_task->state = TASK_DEAD;
    if (owner > 0 && owner < MAX_TASKS && procs[owner].used &&
        !process_has_live_tasks(owner)) {
        procs[owner].state = PROC_ZOMBIE;
        procs[owner].exit_code = code;
    }
    schedule();
    /* Should never reach here */
    for (;;) __asm__ volatile("hlt");
}

void task_exit_process_code(int code) {
    __asm__ volatile("cli");
    int owner = task_process_owner(current_task);
    if (owner <= 0 || owner >= MAX_TASKS)
        owner = current_task ? current_task->id : 0;

    for (int i = 1; i < num_tasks; i++) {
        if (task_process_owner(&tasks[i]) != owner)
            continue;
        futex_cancel_task_locked(i);
        tasks[i].exit_code = code;
        tasks[i].wake_tick = 0;
        tasks[i].state = TASK_DEAD;
    }
    if (owner > 0 && owner < MAX_TASKS && procs[owner].used) {
        procs[owner].state = PROC_ZOMBIE;
        procs[owner].exit_code = code;
    }
    schedule();
    for (;;) __asm__ volatile("hlt");
}

void task_sleep_until(uint32_t wake_tick) {
    uint32_t irq_flags = irq_save();
    current_task->wake_tick = wake_tick;
    current_task->state = TASK_SLEEPING;
    schedule();
    irq_restore(irq_flags);
}

void task_prepare_block_current(uint32_t wake_tick) {
    if (!current_task || current_task->id == 0)
        return;
    current_task->wake_tick = wake_tick;
    current_task->state = TASK_BLOCKED;
}

void task_block_current(void) {
    if (!current_task || current_task->id == 0)
        return;
    uint32_t irq_flags = irq_save();
    task_prepare_block_current(0);
    schedule();
    irq_restore(irq_flags);
}

void task_block_current_until(uint32_t wake_tick) {
    if (!current_task || current_task->id == 0)
        return;
    uint32_t irq_flags = irq_save();
    task_prepare_block_current(wake_tick ? wake_tick : 1);
    schedule();
    irq_restore(irq_flags);
}

int task_wake(int id) {
    if (id <= 0 || id >= num_tasks)
        return 0;
    if (tasks[id].state != TASK_BLOCKED)
        return 0;
    tasks[id].wake_tick = 0;
    tasks[id].state = TASK_READY;
    return 1;
}
