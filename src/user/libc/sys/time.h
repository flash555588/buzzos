#ifndef BUZZOS_SYS_TIME_COMPAT_H
#define BUZZOS_SYS_TIME_COMPAT_H

#include <sys/types.h>

struct timeval {
    time_t tv_sec;
    int32_t tv_usec;
};

int gettimeofday(struct timeval *value, void *timezone);

#endif
