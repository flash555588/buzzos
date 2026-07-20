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

int sys_open_console_aware(uint32_t path_arg, uint32_t flags, uint32_t c, uint32_t d, uint32_t e) {
    (void)c; (void)d; (void)e;
    const char *path = (const char *)(uintptr_t)path_arg;
    if (!user_string_ok(path))
        return -1;
    if (current_task && current_task->console_silent && path && streq(path, "/dev/console"))
        return vfs_open_flags("/dev/null", (int)flags);
    return vfs_open_flags(path, (int)flags);
}

int sys_close(uint32_t fd, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)b; (void)c; (void)d; (void)e;
    return vfs_close((int)fd);
}

int sys_read(uint32_t fd, uint32_t buf, uint32_t count, uint32_t d, uint32_t e) {
    (void)d; (void)e;
    if (!user_range_writable(buf, count))
        return -1;
    return vfs_read((int)fd, (void *)(uintptr_t)buf, (size_t)count);
}

int sys_write(uint32_t fd, uint32_t buf, uint32_t count, uint32_t d, uint32_t e) {
    (void)d; (void)e;
    if (!user_range_ok(buf, count))
        return -1;
    return vfs_write((int)fd, (const void *)(uintptr_t)buf, (size_t)count);
}

int sys_dup(uint32_t fd, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)b; (void)c; (void)d; (void)e;
    return vfs_dup((int)fd);
}

int sys_dup2(uint32_t oldfd, uint32_t newfd, uint32_t c, uint32_t d, uint32_t e) {
    (void)c; (void)d; (void)e;
    return vfs_dup2((int)oldfd, (int)newfd);
}

int sys_stat(uint32_t path, uint32_t st, uint32_t c, uint32_t d, uint32_t e) {
    (void)c; (void)d; (void)e;
    if (!user_string_ok((const char *)(uintptr_t)path) || !user_range_writable(st, sizeof(struct stat)))
        return -1;
    return vfs_stat((const char *)(uintptr_t)path, (struct stat *)(uintptr_t)st);
}

int sys_getdents(uint32_t fd, uint32_t ents, uint32_t count, uint32_t d, uint32_t e) {
    (void)d; (void)e;
    if (!user_range_writable(ents, count))
        return -1;
    return vfs_getdents((int)fd, (struct dirent *)(uintptr_t)ents, (size_t)count);
}

int sys_mkdir(uint32_t path, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)b; (void)c; (void)d; (void)e;
    if (!user_string_ok((const char *)(uintptr_t)path))
        return -1;
    return vfs_mkdir((const char *)(uintptr_t)path);
}

int sys_unlink(uint32_t path, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)b; (void)c; (void)d; (void)e;
    if (!user_string_ok((const char *)(uintptr_t)path))
        return -1;
    return vfs_remove((const char *)(uintptr_t)path);
}

int sys_create(uint32_t path, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)b; (void)c; (void)d; (void)e;
    if (!user_string_ok((const char *)(uintptr_t)path))
        return -1;
    return vfs_create((const char *)(uintptr_t)path);
}

int sys_lseek(uint32_t fd, uint32_t offset, uint32_t whence, uint32_t d, uint32_t e) {
    (void)d; (void)e;
    return vfs_lseek((int)fd, (int)offset, (int)whence);
}

int sys_rmdir(uint32_t path, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)b; (void)c; (void)d; (void)e;
    if (!user_string_ok((const char *)(uintptr_t)path))
        return -1;
    return vfs_rmdir((const char *)(uintptr_t)path);
}

int sys_rename(uint32_t old_path, uint32_t new_path, uint32_t c, uint32_t d, uint32_t e) {
    (void)c; (void)d; (void)e;
    if (!user_string_ok((const char *)(uintptr_t)old_path) ||
        !user_string_ok((const char *)(uintptr_t)new_path))
        return -1;
    return vfs_rename((const char *)(uintptr_t)old_path, (const char *)(uintptr_t)new_path);
}

int sys_fsstat(uint32_t info_arg, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    (void)b; (void)c; (void)d; (void)e;
    if (!user_range_writable(info_arg, sizeof(struct fs_info)))
        return -1;
    return minifs_info((struct fs_info *)(uintptr_t)info_arg);
}
