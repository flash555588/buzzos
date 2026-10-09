/* Interrupt/yield injection against the real scheduler creation path. */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#undef SYS_OPEN

#define BUZZOS_IRQ_H
static uint64_t irq_save(void) { return 0x202; }
static void irq_restore(uint64_t flags) { (void)flags; }
#include "../src/kernel/sched/task.c"

tss64_t tss;
uint8_t __boot_stack_top;
static uint64_t stack_memory[32 * PAGE_SIZE / sizeof(uint64_t)];
static int injected_yields, fail_allocation;

static void inject_contention(void) {
    int id = num_tasks - 1;
    if (id <= 0 || tasks[id].state == TASK_DEAD)
        return;
    assert(tasks[id].state == TASK_CREATING);
    assert(task_wake(id) == 0);
    task_yield();
    sched_tick(1);
    assert(current_task->id == 0);
    assert(tasks[id].state == TASK_CREATING);
    injected_yields++;
}

uintptr_t pmm_alloc_pages(size_t pages) {
    assert(pages == KERNEL_STACK_PAGES);
    inject_contention();
    if (fail_allocation) { fail_allocation = 0; return 0; }
    return (uintptr_t)stack_memory;
}
void pmm_free_pages(uintptr_t addr, size_t pages) { (void)addr; (void)pages; }
size_t pmm_free_pages_snapshot(void) { return 1024; }
void vfs_task_reset(int id) { (void)id; inject_contention(); }
void syscall_release_thread(int id) { (void)id; }
void syscall_cleanup_process(int id) { (void)id; }
void syscall_process_exited(int id) { (void)id; }
void futex_cancel_task_locked(int id) { (void)id; }
size_t paging_count_user_pages(uintptr_t cr3) { (void)cr3; return 0; }
uintptr_t paging_current_cr3(void) { return 4096; }
uintptr_t paging_kernel_cr3(void) { return 4096; }
void paging_destroy_user_space(uintptr_t cr3) { (void)cr3; }
void paging_switch(uintptr_t cr3) { (void)cr3; }
uint32_t timer_ticks(void) { return 123; }
void fpu_state_init(void *p) { (void)p; }
void fpu_state_save(void *p) { (void)p; }
void fpu_state_restore(const void *p) { (void)p; }
void serial_puts(const char *s) { (void)s; }
void serial_puthex(uint32_t v) { (void)v; }
void serial_puthex64(uint64_t v) { (void)v; }
void switch_context(uintptr_t *old, uintptr_t next) {
    (void)old; (void)next;
    assert(!"scheduler selected an unfinished task");
}
static void entry(void) {}

int main(void) {
    sched_init();
    for (int iteration = 0; iteration < 100; iteration++) {
        int id = task_create_ex(entry, "startup-test", 1);
        assert(id == 1);
        assert(task_get_state(id) == TASK_CREATING);
        assert(task_wake(id) == 0);
        task_make_ready(id);
        assert(task_get_state(id) == TASK_READY);
        tasks[id].state = TASK_DEAD;
        procs[id].used = 0;
    }
    fail_allocation = 1;
    assert(task_create_ex(entry, "allocation-failure", 1) == -1);
    assert(task_create_ex(entry, "after-failure", 1) == 1);
    assert(injected_yields == 203);
    puts("task creation: allocator/VFS yields, IRQ wake, reuse and failure passed");
    return 0;
}
