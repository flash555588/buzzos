#ifndef BUZZOS_ARPA_INET_COMPAT_H
#define BUZZOS_ARPA_INET_COMPAT_H

#include "../netinet/in.h"

int inet_aton(const char *text, struct in_addr *address);
int inet_pton(int family, const char *text, void *address);

#endif
