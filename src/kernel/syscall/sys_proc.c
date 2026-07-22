#include <stddef.h>
#include <stdint.h>
#include "exec.h"
#include "irq.h"
#include "paging.h"
#include "pmm.h"
#include "reboot.h"
#include "serial.h"
#include "syscall_internal.h"
#include "sys_shm.h"
#include "task.h"
#include "timer.h"
#include "rtc.h"
#include "user.h"
#include "vfs.h"

static volatile int exec_syscall_lock;
static char exec_path_buf[256];
static char exec_argv_storage[16][256];
static const char *exec_argv_ptrs[16];

int sys_monotonic_ms(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return (int)(timer_ticks() * (1000u / TIMER_HZ));
}

static void exec_lock(void) {
    while (__sync_lock_test_and_set(&exec_syscall_lock, 1))
        task_yield();
}

static void exec_unlock(void) {
    __sync_lock_release(&exec_syscall_lock);
}

static void copy_user_cstr_256(char *dst, const char *src) {
    int i = 0;
    if (!src)
        src = "";
    while (i < 255 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static int spawn_proc_common_locked(const char *path, int flags, int argc, const char *const argv[]) {
    struct stat st;
    if (vfs_stat(path, &st) < 0 || st.st_size < 52 ||
        st.st_size > USER_LOAD_END - USER_LOAD_START)
        return -1;

    int fd = vfs_open_flags(path, O_RDONLY);
    if (fd < 0)
        return -1;

    const char *name = path;
    for (int i = 0; path && path[i]; i++)
        if (path[i] == '/')
            name = path + i + 1;
    int silent = (flags & 1u) ? 1 : 0;
    int inherit_all = (flags & 2u) ? 1 : 0;
    int inherit_stdio = (flags & 4u) ? 1 : 0;
    int inherit_owner = (inherit_all || inherit_stdio) ? current_fd_owner() : -1;
    return exec_start_file_args_with_fds(
        fd, (size_t)st.st_size, name, silent, argc, argv, inherit_owner,
        inherit_stdio && !inherit_all);
}

int sys_exit(uint32_t code, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)b; (void)c; (void)d; (void)e;
    if (!current_task->console_silent) {
        serial_puts("[syscall] exit(");
        serial_puthex((uint32_t)code);
        serial_puts(") task=");
        serial_puthex((uint32_t)current_task->id);
        serial_puts("\n");
    }

    if (task_get_tid() == task_get_pid())
        task_exit_process_code((int)code);
    else
        task_exit_code((int)code);
    for (;;) { __asm__ volatile("hlt"); }
    return 0;
}

int sys_spawn_proc(uint32_t path_arg, uint32_t flags, uint32_t c, uint32_t d, uint32_t e) {
    (void)c; (void)d; (void)e;
    const char *path = (const char *)(uintptr_t)path_arg;
    if (!user_string_ok(path))
        return -1;

    exec_lock();
    copy_user_cstr_256(exec_path_buf, path);
    exec_argv_ptrs[0] = exec_path_buf;
    int pid = spawn_proc_common_locked(exec_path_buf, (int)flags, 1, exec_argv_ptrs);
    exec_unlock();
    return pid;
}

int sys_spawn_proc_args(uint32_t path_arg, uint32_t argv_arg, uint32_t argc_arg,
                        uint32_t flags, uint32_t e) {
    (void)e;
    const char *path = (const char *)(uintptr_t)path_arg;
    const char *const *user_argv = (const char *const *)(uintptr_t)argv_arg;
    int argc = (int)argc_arg;
    if (!user_string_ok(path))
        return -1;
    if (argc < 0)
        return -1;
    if (argc > 15)
        argc = 15;
    if (argc > 0 && !user_range_ok(argv_arg, (uint32_t)argc * sizeof(char *)))
        return -1;

    exec_lock();
    copy_user_cstr_256(exec_path_buf, path);

    for (int i = 0; i < argc; i++) {
        if (user_argv && !user_string_ok(user_argv[i])) {
            exec_unlock();
            return -1;
        }
        copy_user_cstr_256(exec_argv_storage[i], user_argv ? user_argv[i] : "");
        exec_argv_ptrs[i] = exec_argv_storage[i];
    }
    if (argc == 0) {
        copy_user_cstr_256(exec_argv_storage[0], exec_path_buf);
        exec_argv_ptrs[0] = exec_argv_storage[0];
        argc = 1;
    }
    int pid = spawn_proc_common_locked(exec_path_buf, (int)flags, argc, exec_argv_ptrs);
    exec_unlock();
    return pid;
}

int sys_ps(uint32_t buf, uint32_t size, uint32_t show_dead, uint32_t d, uint32_t e) {
    (void)d; (void)e;
    if (!user_range_writable(buf, size))
        return -1;
    return task_dump_text((char *)(uintptr_t)buf, (int)size, (int)show_dead);
}

int sys_reboot(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    machine_reboot();
}

struct thread_info {
    uint32_t func_addr;
    uint32_t stack_top;
    int owner;
    int stack_slot;
    int used;
};

static struct thread_info thread_infos[MAX_TASKS];
static uint32_t process_thread_slots[MAX_TASKS];
static uint32_t process_heap_base[MAX_TASKS];
static uint32_t process_heap_break[MAX_TASKS];

void syscall_reset_process(int task_id) {
    if (task_id < 0 || task_id >= MAX_TASKS)
        return;
    process_thread_slots[task_id] = 0;
    process_heap_base[task_id] = 0;
    process_heap_break[task_id] = 0;
}

void syscall_set_heap_start(int task_id, uint32_t start) {
    if (task_id < 0 || task_id >= MAX_TASKS || start < USER_LOAD_START ||
        start > USER_LOAD_END)
        return;
    process_heap_base[task_id] = start;
    process_heap_break[task_id] = start;
}

void syscall_cleanup_process(int task_id) {
    if (task_id < 0 || task_id >= MAX_TASKS)
        return;
    sys_net_cleanup_owner(task_id);
    shm_cleanup_owner(task_id);
    process_thread_slots[task_id] = 0;
    process_heap_base[task_id] = 0;
    process_heap_break[task_id] = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (thread_infos[i].used && thread_infos[i].owner == task_id)
            thread_infos[i].used = 0;
    }
}

int sys_sbrk(uint32_t increment_arg, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)b; (void)c; (void)d; (void)e;
    int owner = task_get_pid();
    if (owner <= 0 || owner >= MAX_TASKS || !process_heap_base[owner])
        return -1;

    int32_t increment = (int32_t)increment_arg;
    uint32_t old_break = process_heap_break[owner];
    uint32_t new_break;
    if (increment >= 0) {
        uint32_t amount = (uint32_t)increment;
        if (amount > USER_LOAD_END - old_break)
            return -1;
        new_break = old_break + amount;
        if (new_break > old_break &&
            paging_map_user_range(old_break, new_break - old_break) < 0)
            return -1;
    } else {
        uint32_t amount = 0u - (uint32_t)increment;
        if (amount > old_break - process_heap_base[owner])
            return -1;
        new_break = old_break - amount;
    }
    process_heap_break[owner] = new_break;
    return (int)old_break;
}

void syscall_release_thread(int task_id) {
    if (task_id < 0 || task_id >= MAX_TASKS || !thread_infos[task_id].used)
        return;
    int owner = thread_infos[task_id].owner;
    int slot = thread_infos[task_id].stack_slot;
    if (owner >= 0 && owner < MAX_TASKS && slot >= 0 && slot < (int)USER_THREAD_STACK_SLOTS)
        process_thread_slots[owner] &= ~(1u << (uint32_t)slot);
    thread_infos[task_id].used = 0;
}

static void thread_trampoline(void) {
    int id = current_task->id;
    uint32_t func = thread_infos[id].func_addr;
    uint32_t stack = thread_infos[id].stack_top;
    user_enter(func, stack);
}

int sys_spawn(uint32_t func_addr, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)c; (void)d; (void)e;
    uint32_t return_addr = b;
    if (!user_range_ok(func_addr, 1) || !user_range_ok(return_addr, 1))
        return -1;
    int owner = current_task ? current_task->fd_owner : 0;
    if (owner < 0 || owner >= MAX_TASKS)
        return -1;

    int slot = -1;
    for (int i = 0; i < (int)USER_THREAD_STACK_SLOTS; i++) {
        if (!(process_thread_slots[owner] & (1u << (uint32_t)i))) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -1;
    process_thread_slots[owner] |= 1u << (uint32_t)slot;
    uint32_t user_stack = USER_DEFAULT_STACK_TOP - USER_MAIN_STACK_SIZE -
                          (uint32_t)slot * USER_THREAD_STACK_SIZE;
    if (user_stack < USER_LOAD_END + USER_THREAD_STACK_SIZE ||
        paging_map_user_range(user_stack - USER_THREAD_STACK_SIZE,
                              USER_THREAD_STACK_SIZE) < 0) {
        process_thread_slots[owner] &= ~(1u << (uint32_t)slot);
        return -1;
    }
    user_stack -= 4;
    *(uint32_t *)(uintptr_t)user_stack = return_addr;

    uint32_t irq_flags = irq_save();
    int id = task_create(thread_trampoline, "user_thread");
    if (id < 0) {
        process_thread_slots[owner] &= ~(1u << (uint32_t)slot);
        irq_restore(irq_flags);
        return -1;
    }

    thread_infos[id].func_addr = func_addr;
    thread_infos[id].stack_top = user_stack;
    thread_infos[id].owner = owner;
    thread_infos[id].stack_slot = slot;
    thread_infos[id].used = 1;
    task_set_fd_owner(id, owner);
    irq_restore(irq_flags);
    return id;
}

int sys_yield(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    task_yield();
    return 0;
}

int sys_join(uint32_t tid_arg, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)b; (void)c; (void)d; (void)e;
    int tid = (int)tid_arg;
    for (;;) {
        int state = task_get_state(tid);
        if (state < 0)
            return -1;
        if (state == TASK_DEAD)
            break;
        task_yield();
    }
    task_forget_dead(tid);
    return 0;
}

int sys_sleep(uint32_t ms, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)b; (void)c; (void)d; (void)e;
    timer_sleep_ms(ms);
    return 0;
}

int sys_realtime(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return rtc_unix_time();
}

int sys_kill(uint32_t pid, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)b; (void)c; (void)d; (void)e;
    return task_kill((int)pid);
}

int sys_getpid(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return task_get_pid();
}

int sys_gettid(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return task_get_tid();
}

int sys_chdir(uint32_t path, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)b; (void)c; (void)d; (void)e;
    if (!user_string_ok((const char *)(uintptr_t)path))
        return -1;
    return vfs_chdir((const char *)(uintptr_t)path);
}

int sys_getcwd(uint32_t buf, uint32_t size, uint32_t c, uint32_t d, uint32_t e) {
    (void)c; (void)d; (void)e;
    if (!user_range_writable(buf, size))
        return -1;
    return vfs_getcwd((char *)(uintptr_t)buf, (size_t)size);
}

int sys_waitpid(uint32_t pid, uint32_t status, uint32_t options, uint32_t d, uint32_t e) {
    (void)d; (void)e;
    if (status && !user_range_writable(status, sizeof(int)))
        return -1;
    return task_wait_pid((int)pid, (int *)(uintptr_t)status, (int)options);
}
