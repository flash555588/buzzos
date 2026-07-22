#ifndef BUZZOS_NETSURF_TLS_H
#define BUZZOS_NETSURF_TLS_H

#include <stddef.h>

struct buzzos_tls;

struct buzzos_tls *buzzos_tls_open(int socket_fd, const char *host,
                                   int *error_code);
int buzzos_tls_write_all(struct buzzos_tls *tls, const void *data, size_t len);
int buzzos_tls_read(struct buzzos_tls *tls, void *data, size_t len);
int buzzos_tls_error(const struct buzzos_tls *tls);
void buzzos_tls_close(struct buzzos_tls *tls);

#endif
