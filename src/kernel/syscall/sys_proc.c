#include <stddef.h>
#include <stdint.h>
#include "console.h"
#include "exec.h"
#include "fb.h"
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

intptr_t sys_monotonic_ms(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
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
    int serial_stdio = (flags & 8u) ? 1 : 0;
    int inherit_owner = (inherit_all || inherit_stdio) ? current_fd_owner() : -1;
    return exec_start_file_args_with_fds(
        fd, (size_t)st.st_size, name, silent, argc, argv, inherit_owner,
        inherit_stdio && !inherit_all, serial_stdio);
}

intptr_t sys_exit(uintptr_t code, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
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

intptr_t sys_spawn_proc(uintptr_t path_arg, uintptr_t flags, uintptr_t c, uintptr_t d, uintptr_t e) {
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

intptr_t sys_spawn_proc_args(uintptr_t path_arg, uintptr_t argv_arg, uintptr_t argc_arg,
                        uintptr_t flags, uintptr_t e) {
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
    if (argc > 0 && !user_range_ok(argv_arg, (size_t)argc * sizeof(char *)))
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

intptr_t sys_ps(uintptr_t buf, uintptr_t size, uintptr_t show_dead, uintptr_t d, uintptr_t e) {
    (void)d; (void)e;
    if (!user_range_writable(buf, size))
        return -1;
    return task_dump_text((char *)(uintptr_t)buf, (int)size, (int)show_dead);
}

intptr_t sys_reboot(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    machine_reboot();
}

struct thread_info {
    uintptr_t func_addr;
    uintptr_t stack_top;
    int owner;
    int stack_slot;
    int used;
};

static struct thread_info thread_infos[MAX_TASKS];
static uint64_t process_thread_slots[MAX_TASKS][USER_THREAD_STACK_SLOTS / 64u];
static uintptr_t process_heap_base[MAX_TASKS];
static uintptr_t process_heap_break[MAX_TASKS];

void syscall_reset_process(int task_id) {
    if (task_id < 0 || task_id >= MAX_TASKS)
        return;
    for (size_t i = 0; i < USER_THREAD_STACK_SLOTS / 64u; i++)
        process_thread_slots[task_id][i] = 0;
    process_heap_base[task_id] = 0;
    process_heap_break[task_id] = 0;
}

void syscall_set_heap_start(int task_id, uintptr_t start) {
    if (task_id < 0 || task_id >= MAX_TASKS || start < USER_LOAD_START ||
        start > USER_LOAD_END)
        return;
    process_heap_base[task_id] = start;
    process_heap_break[task_id] = start;
}

void syscall_cleanup_process(int task_id) {
    if (task_id < 0 || task_id >= MAX_TASKS)
        return;
    syscall_process_exited(task_id);
    sys_net_cleanup_owner(task_id);
    shm_cleanup_owner(task_id);
    for (size_t i = 0; i < USER_THREAD_STACK_SLOTS / 64u; i++)
        process_thread_slots[task_id][i] = 0;
    process_heap_base[task_id] = 0;
    process_heap_break[task_id] = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (thread_infos[i].used && thread_infos[i].owner == task_id)
            thread_infos[i].used = 0;
    }
}

void syscall_process_exited(int task_id) {
    if (task_id <= 0 || task_id >= MAX_TASKS)
        return;
    if (fb_display_release(task_id) == 0)
        console_activate(0);
}

intptr_t sys_sbrk(uintptr_t increment_arg, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)b; (void)c; (void)d; (void)e;
    int owner = task_get_pid();
    if (owner <= 0 || owner >= MAX_TASKS || !process_heap_base[owner])
        return -1;

    intptr_t increment = (intptr_t)increment_arg;
    uintptr_t old_break = process_heap_break[owner];
    uintptr_t new_break;
    if (increment >= 0) {
        uintptr_t amount = (uintptr_t)increment;
        if (amount > USER_LOAD_END - old_break)
            return -1;
        new_break = old_break + amount;
        if (new_break > old_break &&
            paging_map_user_range(old_break, new_break - old_break) < 0)
            return -1;
    } else {
        uintptr_t amount = (uintptr_t)(-increment);
        if (amount > old_break - process_heap_base[owner])
            return -1;
        new_break = old_break - amount;
    }
    process_heap_break[owner] = new_break;
    return (intptr_t)old_break;
}

void syscall_release_thread(int task_id) {
    if (task_id < 0 || task_id >= MAX_TASKS || !thread_infos[task_id].used)
        return;
    int owner = thread_infos[task_id].owner;
    int slot = thread_infos[task_id].stack_slot;
    if (owner >= 0 && owner < MAX_TASKS && slot >= 0 && slot < (int)USER_THREAD_STACK_SLOTS)
        process_thread_slots[owner][(unsigned)slot / 64u] &=
            ~(UINT64_C(1) << ((unsigned)slot % 64u));
    thread_infos[task_id].used = 0;
}

static void thread_trampoline(void) {
    int id = current_task->id;
    uintptr_t func = 0;
    uintptr_t stack = 0;
    if (id >= 0 && id < MAX_TASKS && thread_infos[id].used) {
        func = thread_infos[id].func_addr;
        stack = thread_infos[id].stack_top;
    }
    if (!func || !stack) {
        serial_puts("[spawn] refuse null thread entry/stack for task=");
        serial_puthex((uint32_t)id);
        serial_puts("\n");
        task_exit_code(-1);
        for (;;)
            __asm__ volatile("hlt");
    }
    user_enter(func, stack);
}

intptr_t sys_spawn(uintptr_t func_addr, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)c; (void)d; (void)e;
    uintptr_t return_addr = b;
    if (!user_range_ok(func_addr, 1) || !user_range_ok(return_addr, 1))
        return -1;
    int owner = current_task ? current_task->fd_owner : 0;
    if (owner < 0 || owner >= MAX_TASKS)
        return -1;

    int slot = -1;
    for (int i = 0; i < (int)USER_THREAD_STACK_SLOTS; i++) {
        if (!(process_thread_slots[owner][(unsigned)i / 64u] &
              (UINT64_C(1) << ((unsigned)i % 64u)))) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -1;
    process_thread_slots[owner][(unsigned)slot / 64u] |=
        UINT64_C(1) << ((unsigned)slot % 64u);
    uintptr_t user_stack = USER_DEFAULT_STACK_TOP - USER_MAIN_STACK_SIZE -
                           (uintptr_t)slot * USER_THREAD_STACK_SIZE;
    if (user_stack < USER_LOAD_END + USER_THREAD_STACK_SIZE ||
        paging_map_user_range(user_stack - USER_THREAD_STACK_SIZE,
                              USER_THREAD_STACK_SIZE) < 0) {
        process_thread_slots[owner][(unsigned)slot / 64u] &=
            ~(UINT64_C(1) << ((unsigned)slot % 64u));
        return -1;
    }
    user_stack -= sizeof(uintptr_t);
    *(uintptr_t *)user_stack = return_addr;

    uint32_t irq_flags = irq_save();
    int id = task_create(thread_trampoline, "user_thread");
    if (id < 0) {
        process_thread_slots[owner][(unsigned)slot / 64u] &=
            ~(UINT64_C(1) << ((unsigned)slot % 64u));
        irq_restore(irq_flags);
        return -1;
    }

    thread_infos[id].func_addr = func_addr;
    thread_infos[id].stack_top = user_stack;
    thread_infos[id].owner = owner;
    thread_infos[id].stack_slot = slot;
    thread_infos[id].used = 1;
    task_set_fd_owner(id, owner);
    /* Arm only after thread_infos is fully populated. */
    task_make_ready(id);
    irq_restore(irq_flags);
    return id;
}

intptr_t sys_yield(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    task_yield();
    return 0;
}

intptr_t sys_join(uintptr_t tid_arg, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
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

intptr_t sys_sleep(uintptr_t ms, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)b; (void)c; (void)d; (void)e;
    timer_sleep_ms(ms);
    return 0;
}

intptr_t sys_realtime(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return rtc_unix_time();
}

intptr_t sys_kill(uintptr_t pid, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)b; (void)c; (void)d; (void)e;
    return task_kill((int)pid);
}

intptr_t sys_getpid(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return task_get_pid();
}

intptr_t sys_gettid(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return task_get_tid();
}

intptr_t sys_chdir(uintptr_t path, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)b; (void)c; (void)d; (void)e;
    if (!user_string_ok((const char *)(uintptr_t)path))
        return -1;
    return vfs_chdir((const char *)(uintptr_t)path);
}

intptr_t sys_getcwd(uintptr_t buf, uintptr_t size, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)c; (void)d; (void)e;
    if (!user_range_writable(buf, size))
        return -1;
    return vfs_getcwd((char *)(uintptr_t)buf, (size_t)size);
}

intptr_t sys_waitpid(uintptr_t pid, uintptr_t status, uintptr_t options, uintptr_t d, uintptr_t e) {
    (void)d; (void)e;
    if (status && !user_range_writable(status, sizeof(int)))
        return -1;
    return task_wait_pid((int)pid, (int *)(uintptr_t)status, (int)options);
}
