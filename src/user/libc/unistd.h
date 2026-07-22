#ifndef BUZZOS_UNISTD_COMPAT_H
#define BUZZOS_UNISTD_COMPAT_H

#include "libc.h"
#include <sys/types.h>

#define F_OK 0
#define R_OK 4
#define W_OK 2

int unlinkat(int directory_fd, const char *path, int flags);
int ftruncate(int fd, off_t length);

#endif
