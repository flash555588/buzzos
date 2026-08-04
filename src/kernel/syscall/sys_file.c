#include <stddef.h>
#include <stdint.h>
#include "minifs.h"
#include "syscall_internal.h"
#include "task.h"
#include "vfs.h"

enum { SYSCALL_PATH_MAX = 256, SYSCALL_IO_CHUNK = 4096 };

static int copy_path(char path[SYSCALL_PATH_MAX], uintptr_t user_path) {
    return copy_string_from_user(path, SYSCALL_PATH_MAX, user_path);
}

static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

intptr_t sys_open_console_aware(uintptr_t path_arg, uintptr_t flags, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)c; (void)d; (void)e;
    char path[SYSCALL_PATH_MAX];
    if (copy_path(path, path_arg) < 0)
        return -1;
    if (current_task && current_task->console_silent && streq(path, "/dev/console"))
        return vfs_open_flags("/dev/null", (int)flags);
    return vfs_open_flags(path, (int)flags);
}

intptr_t sys_close(uintptr_t fd, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)b; (void)c; (void)d; (void)e;
    return vfs_close((int)fd);
}

intptr_t sys_read(uintptr_t fd, uintptr_t buf, uintptr_t count, uintptr_t d, uintptr_t e) {
    (void)d; (void)e;
    uint8_t bounce[SYSCALL_IO_CHUNK];
    size_t done = 0;
    while (done < count) {
        size_t chunk = count - done;
        if (chunk > sizeof(bounce)) chunk = sizeof(bounce);
        int n = vfs_read((int)fd, bounce, chunk);
        if (n <= 0)
            return done ? (intptr_t)done : n;
        if (copy_to_user(buf + done, bounce, (size_t)n) < 0)
            return done ? (intptr_t)done : -1;
        done += (size_t)n;
        if ((size_t)n < chunk)
            break;
    }
    return (intptr_t)done;
}

intptr_t sys_write(uintptr_t fd, uintptr_t buf, uintptr_t count, uintptr_t d, uintptr_t e) {
    (void)d; (void)e;
    uint8_t bounce[SYSCALL_IO_CHUNK];
    size_t done = 0;
    while (done < count) {
        size_t chunk = count - done;
        if (chunk > sizeof(bounce)) chunk = sizeof(bounce);
        if (copy_from_user(bounce, buf + done, chunk) < 0)
            return done ? (intptr_t)done : -1;
        int n = vfs_write((int)fd, bounce, chunk);
        if (n <= 0)
            return done ? (intptr_t)done : n;
        done += (size_t)n;
        if ((size_t)n < chunk)
            break;
    }
    return (intptr_t)done;
}

intptr_t sys_dup(uintptr_t fd, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)b; (void)c; (void)d; (void)e;
    return vfs_dup((int)fd);
}

intptr_t sys_dup2(uintptr_t oldfd, uintptr_t newfd, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)c; (void)d; (void)e;
    return vfs_dup2((int)oldfd, (int)newfd);
}

intptr_t sys_stat(uintptr_t path, uintptr_t st, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)c; (void)d; (void)e;
    char kernel_path[SYSCALL_PATH_MAX];
    struct stat kernel_stat;
    if (copy_path(kernel_path, path) < 0)
        return -1;
    int ret = vfs_stat(kernel_path, &kernel_stat);
    if (ret < 0)
        return ret;
    return copy_to_user(st, &kernel_stat, sizeof(kernel_stat));
}

intptr_t sys_getdents(uintptr_t fd, uintptr_t ents, uintptr_t count, uintptr_t d, uintptr_t e) {
    (void)d; (void)e;
    uint8_t bounce[SYSCALL_IO_CHUNK];
    size_t chunk = count < sizeof(bounce) ? count : sizeof(bounce);
    int ret = vfs_getdents((int)fd, (struct dirent *)bounce, chunk);
    if (ret <= 0)
        return ret;
    return copy_to_user(ents, bounce, (size_t)ret) < 0 ? -1 : ret;
}

intptr_t sys_mkdir(uintptr_t path, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)b; (void)c; (void)d; (void)e;
    char kernel_path[SYSCALL_PATH_MAX];
    if (copy_path(kernel_path, path) < 0)
        return -1;
    return vfs_mkdir(kernel_path);
}

intptr_t sys_unlink(uintptr_t path, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)b; (void)c; (void)d; (void)e;
    char kernel_path[SYSCALL_PATH_MAX];
    if (copy_path(kernel_path, path) < 0)
        return -1;
    return vfs_remove(kernel_path);
}

intptr_t sys_create(uintptr_t path, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)b; (void)c; (void)d; (void)e;
    char kernel_path[SYSCALL_PATH_MAX];
    if (copy_path(kernel_path, path) < 0)
        return -1;
    return vfs_create(kernel_path);
}

intptr_t sys_lseek(uintptr_t fd, uintptr_t offset, uintptr_t whence, uintptr_t d, uintptr_t e) {
    (void)d; (void)e;
    return vfs_lseek((int)fd, (int)offset, (int)whence);
}

intptr_t sys_rmdir(uintptr_t path, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)b; (void)c; (void)d; (void)e;
    char kernel_path[SYSCALL_PATH_MAX];
    if (copy_path(kernel_path, path) < 0)
        return -1;
    return vfs_rmdir(kernel_path);
}

intptr_t sys_rename(uintptr_t old_path, uintptr_t new_path, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)c; (void)d; (void)e;
    char kernel_old[SYSCALL_PATH_MAX];
    char kernel_new[SYSCALL_PATH_MAX];
    if (copy_path(kernel_old, old_path) < 0 ||
        copy_path(kernel_new, new_path) < 0)
        return -1;
    return vfs_rename(kernel_old, kernel_new);
}

intptr_t sys_fsstat(uintptr_t info_arg, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)b; (void)c; (void)d; (void)e;
    struct fs_info info;
    if (minifs_info(&info) < 0)
        return -1;
    return copy_to_user(info_arg, &info, sizeof(info));
}
