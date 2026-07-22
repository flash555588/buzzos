#ifndef BUZZOS_SYS_SELECT_COMPAT_H
#define BUZZOS_SYS_SELECT_COMPAT_H

#include "time.h"

#define FD_SETSIZE 64
typedef struct { unsigned long bits[2]; } fd_set;

#define FD_ZERO(set) do { (set)->bits[0] = 0; (set)->bits[1] = 0; } while (0)
#define FD_SET(fd, set) ((set)->bits[(unsigned)(fd) / 32] |= (1ul << ((unsigned)(fd) % 32)))
#define FD_CLR(fd, set) ((set)->bits[(unsigned)(fd) / 32] &= ~(1ul << ((unsigned)(fd) % 32)))
#define FD_ISSET(fd, set) (((set)->bits[(unsigned)(fd) / 32] >> ((unsigned)(fd) % 32)) & 1ul)

int select(int count, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout);

#endif
