#ifndef BUZZOS_TASK_H
#define BUZZOS_TASK_H

#include <stdint.h>
#include "fpu.h"

/* Process Control Block. The saved register context is stored on the
 * task's own kernel stack; the PCB only holds the stack pointer. */
struct task {
    uint32_t esp;        /* saved stack pointer */
    uint32_t kstack;     /* allocated kernel stack top */
    uint32_t esp0;       /* ring3 -> ring0 entry stack pointer */
    uint32_t cr3;        /* page directory used while this task runs */
    int      exit_code;
    int      console_silent;
    int      fd_owner;
    int      proc_id;
    uint32_t wake_tick;
    int      id;         /* task id */
    int      state;
    char     name[16];
    char     cwd[128];
    uint8_t  fpu_state[FPU_STATE_SIZE] __attribute__((aligned(16)));
};

#define TASK_READY   0
#define TASK_RUNNING 1
#define TASK_DEAD    2
#define TASK_SLEEPING 3
#define TASK_BLOCKED 4
#define MAX_TASKS    32

/* Initialise the scheduler. Creates an idle task from the current
 * execution context. */
void sched_init(void);

/* Create a new kernel thread starting at `entry`. Returns task id.
 * The task is created BLOCKED; callers must task_make_ready(id) after any
 * per-task setup so the trampoline never races with metadata init. */
int task_create(void (*entry)(void), const char *name);
int task_create_ex(void (*entry)(void), const char *name, int console_silent);
void task_make_ready(int id);

/* Yield the CPU voluntarily. */
void task_yield(void);

/* Keep the current task on-CPU while leaving hardware IRQs enabled.  Kernel
 * locks use this on the single-CPU system so disk I/O cannot starve audio,
 * network, keyboard, or timer interrupts. */
void task_preempt_disable(void);
void task_preempt_enable(void);

/* Called from timer IRQ to preempt the current task. */
void sched_tick(void);

/* The currently running task. */
extern struct task *current_task;

/* Get task state by id. Returns -1 if invalid. */
int task_get_state(int id);
int task_get_exit_code(int id);
int task_forget_dead(int id);
int task_get_pid(void);
int task_get_tid(void);
int task_get_cwd(char *buf, int size);
int task_set_cwd(const char *path);
int task_wait_pid(int pid, int *status, int options);
void task_set_cr3(int id, uint32_t cr3);
void task_set_console_silent(int id, int silent);
void task_set_fd_owner(int id, int owner);
int task_kill(int id);
void task_dump(void (*putc)(char), int show_dead);
int task_dump_text(char *buf, int size, int show_dead);
void task_dump_threads(void (*putc)(char), int show_dead);
int task_dump_threads_text(char *buf, int size, int show_dead);

/* Mark a task as dead (used by sys_exit for spawned threads). */
void task_exit(void);
void task_exit_code(int code);
void task_exit_process_code(int code);
void task_sleep_until(uint32_t wake_tick);
void task_prepare_block_current(uint32_t wake_tick);
/* Schedule after task_prepare_block_current() while hardware IRQs are already
 * disabled by the caller. Returns with the caller's IRQ state unchanged. */
void task_block_current_prepared(void);
void task_block_current(void);
void task_block_current_until(uint32_t wake_tick);
int  task_wake(int id);

#endif /* BUZZOS_TASK_H */
