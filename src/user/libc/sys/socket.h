#ifndef BUZZOS_SYS_SOCKET_COMPAT_H
#define BUZZOS_SYS_SOCKET_COMPAT_H

#include "../libc.h"

typedef unsigned int socklen_t;

struct sockaddr {
    uint16_t sa_family;
    char sa_data[14];
};

#define AF_UNSPEC 0
#define AF_INET6 10

#endif
