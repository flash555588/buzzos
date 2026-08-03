#include <stddef.h>
#include <stdint.h>
#include "minifs.h"
#include "syscall_internal.h"
#include "task.h"
#include "vfs.h"

static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

intptr_t sys_open_console_aware(uintptr_t path_arg, uintptr_t flags, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)c; (void)d; (void)e;
    const char *path = (const char *)(uintptr_t)path_arg;
    if (!user_string_ok(path))
        return -1;
    if (current_task && current_task->console_silent && path && streq(path, "/dev/console"))
        return vfs_open_flags("/dev/null", (int)flags);
    return vfs_open_flags(path, (int)flags);
}

intptr_t sys_close(uintptr_t fd, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)b; (void)c; (void)d; (void)e;
    return vfs_close((int)fd);
}

intptr_t sys_read(uintptr_t fd, uintptr_t buf, uintptr_t count, uintptr_t d, uintptr_t e) {
    (void)d; (void)e;
    if (!user_range_writable(buf, count))
        return -1;
    return vfs_read((int)fd, (void *)(uintptr_t)buf, (size_t)count);
}

intptr_t sys_write(uintptr_t fd, uintptr_t buf, uintptr_t count, uintptr_t d, uintptr_t e) {
    (void)d; (void)e;
    if (!user_range_ok(buf, count))
        return -1;
    return vfs_write((int)fd, (const void *)(uintptr_t)buf, (size_t)count);
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
    if (!user_string_ok((const char *)(uintptr_t)path) || !user_range_writable(st, sizeof(struct stat)))
        return -1;
    return vfs_stat((const char *)(uintptr_t)path, (struct stat *)(uintptr_t)st);
}

intptr_t sys_getdents(uintptr_t fd, uintptr_t ents, uintptr_t count, uintptr_t d, uintptr_t e) {
    (void)d; (void)e;
    if (!user_range_writable(ents, count))
        return -1;
    return vfs_getdents((int)fd, (struct dirent *)(uintptr_t)ents, (size_t)count);
}

intptr_t sys_mkdir(uintptr_t path, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)b; (void)c; (void)d; (void)e;
    if (!user_string_ok((const char *)(uintptr_t)path))
        return -1;
    return vfs_mkdir((const char *)(uintptr_t)path);
}

intptr_t sys_unlink(uintptr_t path, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)b; (void)c; (void)d; (void)e;
    if (!user_string_ok((const char *)(uintptr_t)path))
        return -1;
    return vfs_remove((const char *)(uintptr_t)path);
}

intptr_t sys_create(uintptr_t path, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)b; (void)c; (void)d; (void)e;
    if (!user_string_ok((const char *)(uintptr_t)path))
        return -1;
    return vfs_create((const char *)(uintptr_t)path);
}

intptr_t sys_lseek(uintptr_t fd, uintptr_t offset, uintptr_t whence, uintptr_t d, uintptr_t e) {
    (void)d; (void)e;
    return vfs_lseek((int)fd, (int)offset, (int)whence);
}

intptr_t sys_rmdir(uintptr_t path, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)b; (void)c; (void)d; (void)e;
    if (!user_string_ok((const char *)(uintptr_t)path))
        return -1;
    return vfs_rmdir((const char *)(uintptr_t)path);
}

intptr_t sys_rename(uintptr_t old_path, uintptr_t new_path, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)c; (void)d; (void)e;
    if (!user_string_ok((const char *)(uintptr_t)old_path) ||
        !user_string_ok((const char *)(uintptr_t)new_path))
        return -1;
    return vfs_rename((const char *)(uintptr_t)old_path, (const char *)(uintptr_t)new_path);
}

intptr_t sys_fsstat(uintptr_t info_arg, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)b; (void)c; (void)d; (void)e;
    if (!user_range_writable(info_arg, sizeof(struct fs_info)))
        return -1;
    return minifs_info((struct fs_info *)(uintptr_t)info_arg);
}
