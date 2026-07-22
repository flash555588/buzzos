#include "exec.h"
#include "irq.h"
#include "elf.h"
#include "paging.h"
#include "serial.h"
#include "syscall.h"
#include "task.h"
#include "user.h"
#include "vfs.h"

static uint32_t proc_entry[MAX_TASKS];
static uint32_t proc_stack[MAX_TASKS];

static void user_process_trampoline(void) {
    int id = current_task->id;
    uint32_t entry = (id >= 0 && id < MAX_TASKS) ? proc_entry[id] : 0;
    uint32_t stack = (id >= 0 && id < MAX_TASKS) ? proc_stack[id] : 0;
    if (!stack)
        stack = USER_DEFAULT_STACK_TOP;
    if (!entry || !stack) {
        serial_puts("[exec] refuse null user entry/stack for task=");
        serial_puthex((uint32_t)id);
        serial_puts("\n");
        task_exit_process_code(-1);
        for (;;)
            __asm__ volatile("hlt");
    }
    user_enter(entry, stack);
}

static int str_len(const char *s) {
    int n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static int write_user_u32(uint32_t cr3, uint32_t address, uint32_t value) {
    return paging_copy_to_user_space(cr3, address, &value, sizeof(value));
}

static uint32_t build_user_stack(uint32_t cr3, int argc,
                                 const char *const argv[]) {
    if (argc < 0)
        argc = 0;
    if (argc > 15)
        argc = 15;

    uint32_t arg_ptrs[16];
    uint32_t sp = USER_DEFAULT_STACK_TOP;

    for (int i = argc - 1; i >= 0; i--) {
        int len = str_len(argv[i]) + 1;
        sp -= (uint32_t)len;
        if (paging_copy_to_user_space(cr3, sp, argv[i] ? argv[i] : "",
                                      (uint32_t)len) < 0)
            return 0;
        arg_ptrs[i] = sp;
    }

    sp &= ~3u;
    sp -= 4;
    if (write_user_u32(cr3, sp, 0) < 0)
        return 0;
    for (int i = argc - 1; i >= 0; i--) {
        sp -= 4;
        if (write_user_u32(cr3, sp, arg_ptrs[i]) < 0)
            return 0;
    }
    uint32_t argv_user = sp;
    sp -= 4;
    if (write_user_u32(cr3, sp, argv_user) < 0)
        return 0;
    sp -= 4;
    if (write_user_u32(cr3, sp, (uint32_t)argc) < 0)
        return 0;
    sp -= 4;
    if (write_user_u32(cr3, sp, 0) < 0)
        return 0;
    return sp;
}

static uint32_t create_user_address_space(void) {
    uint32_t cr3 = paging_create_user_space();
    if (!cr3)
        return 0;
    if (paging_map_user_range_in_space(
            cr3, USER_TRAMPOLINE_BASE, 0x1000u) < 0 ||
        user_install_trampoline_in_space(cr3) < 0 ||
        paging_set_user_range_writable_in_space(
            cr3, USER_TRAMPOLINE_BASE, 0x1000u, 0) < 0 ||
        paging_map_user_range_in_space(
            cr3, USER_DEFAULT_STACK_TOP - USER_MAIN_STACK_SIZE,
            USER_MAIN_STACK_SIZE) < 0) {
        paging_destroy_user_space(cr3);
        return 0;
    }
    return cr3;
}

static int launch_prepared_space(uint32_t proc_cr3, uint32_t entry,
                                 uint32_t image_end, uint32_t stack,
                                 const char *name, int console_silent,
                                 int inherit_fd_owner,
                                 int inherit_stdio_only) {
    uint32_t irq_flags = irq_save();
    int id = task_create_ex(user_process_trampoline,
                            name ? name : "user_proc", console_silent);
    if (id < 0) {
        paging_destroy_user_space(proc_cr3);
        irq_restore(irq_flags);
        return -1;
    }

    proc_entry[id] = entry;
    proc_stack[id] = stack;
    syscall_reset_process(id);
    syscall_set_heap_start(id, (image_end + 0xFFFu) & ~0xFFFu);
    task_set_cr3(id, proc_cr3);
    task_set_console_silent(id, console_silent);
    task_set_fd_owner(id, id);
    if (inherit_fd_owner >= 0) {
        int ok = inherit_stdio_only
            ? vfs_clone_stdio(id, inherit_fd_owner)
            : vfs_clone_fd_table(id, inherit_fd_owner);
        if (ok < 0) {
            serial_puts("[exec] fd clone failed; falling back to stdio\n");
            vfs_setup_stdio(id, console_silent);
        }
    } else {
        vfs_setup_stdio(id, console_silent);
    }
    /* Arm only after entry/stack/CR3/fds are fully installed. */
    task_make_ready(id);
    irq_restore(irq_flags);

    serial_puts("[exec] entry=");
    serial_puthex(entry);
    serial_puts(" task=");
    serial_puthex((uint32_t)id);
    serial_puts("\n");
    return id;
}

int exec_start_args_with_fds(const uint8_t *elf_data, size_t elf_size, const char *name,
                             int console_silent, int argc, const char *const argv[],
                             int inherit_fd_owner, int inherit_stdio_only) {
    uint32_t proc_cr3 = create_user_address_space();
    if (!proc_cr3) {
        serial_puts("[exec] out of user bootstrap pages\n");
        return -1;
    }
    uint32_t image_end = 0;
    uint32_t entry = elf_load_into_space(
        proc_cr3, elf_data, elf_size, &image_end);
    uint32_t stack = build_user_stack(proc_cr3, argc, argv);

    if (!entry || !stack) {
        serial_puts("[exec] bad ELF\n");
        paging_destroy_user_space(proc_cr3);
        return -1;
    }

    return launch_prepared_space(proc_cr3, entry, image_end, stack, name,
                                 console_silent, inherit_fd_owner,
                                 inherit_stdio_only);
}

int exec_start_file_args_with_fds(int fd, size_t elf_size, const char *name,
                                  int console_silent, int argc,
                                  const char *const argv[], int inherit_fd_owner,
                                  int inherit_stdio_only) {
    uint32_t proc_cr3 = create_user_address_space();
    if (!proc_cr3) {
        vfs_close(fd);
        serial_puts("[exec] out of user bootstrap pages\n");
        return -1;
    }
    uint32_t image_end = 0;
    uint32_t entry = elf_load_file_into_space(
        proc_cr3, fd, elf_size, &image_end);
    uint32_t stack = build_user_stack(proc_cr3, argc, argv);
    vfs_close(fd);
    if (!entry || !stack) {
        serial_puts("[exec] bad ELF\n");
        paging_destroy_user_space(proc_cr3);
        return -1;
    }
    return launch_prepared_space(proc_cr3, entry, image_end, stack, name,
                                 console_silent, inherit_fd_owner,
                                 inherit_stdio_only);
}

int exec_start_args(const uint8_t *elf_data, size_t elf_size, const char *name,
                    int console_silent, int argc, const char *const argv[]) {
    return exec_start_args_with_fds(elf_data, elf_size, name, console_silent,
                                    argc, argv, -1, 0);
}

int exec_start(const uint8_t *elf_data, size_t elf_size, const char *name, int console_silent) {
    const char *argv0 = name ? name : "user_proc";
    const char *argv[1] = { argv0 };
    return exec_start_args(elf_data, elf_size, name, console_silent, 1, argv);
}

int exec_elf(const uint8_t *elf_data, size_t elf_size) {
    int id = exec_start(elf_data, elf_size, "user_proc", 0);
    if (id < 0)
        return -1;

    int code = -1;
    if (task_wait_pid(id, &code, 0) < 0)
        return -1;
    serial_puts("[exec] program exited, code=");
    serial_puthex((uint32_t)code);
    serial_puts("\n");
    return code;
}
