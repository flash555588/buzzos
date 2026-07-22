#ifndef BUZZOS_SYS_STAT_COMPAT_H
#define BUZZOS_SYS_STAT_COMPAT_H

#include "../libc.h"

typedef unsigned int mode_t;

#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IRWXU 0700

#define AT_SYMLINK_NOFOLLOW 0x100
int fstatat(int directory_fd, const char *path, struct stat *status, int flags);

int buzz_mkdir_mode(const char *path, mode_t mode);
#define mkdir(path, mode) buzz_mkdir_mode((path), (mode))

#endif
