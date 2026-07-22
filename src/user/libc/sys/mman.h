#ifndef BUZZOS_SYS_MMAN_COMPAT_H
#define BUZZOS_SYS_MMAN_COMPAT_H

#include <stddef.h>

#define PROT_READ 1
#define MAP_SHARED 1
#define MAP_FAILED ((void *)-1)

void *mmap(void *address, size_t length, int protection, int flags,
           int fd, long offset);
int munmap(void *address, size_t length);

#endif
