#ifndef BUZZOS_NETINET_IN_COMPAT_H
#define BUZZOS_NETINET_IN_COMPAT_H

#include "../libc.h"

struct in_addr { uint32_t s_addr; };
struct in6_addr { uint8_t s6_addr[16]; };

#endif
