#include "libc.h"
#include <time.h>

#include "bearssl.h"
#include "buzzos_tls.h"

#include "buzzos_ca_bundle.inc"

struct buzzos_tls {
    br_ssl_client_context client;
    br_x509_minimal_context x509;
    br_sslio_context io;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    int socket_fd;
};

static int cpu_has_rdrand(void) {
    uint32_t eax = 1, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    (void)ebx; (void)edx;
    return (ecx & (1u << 30)) != 0;
}

static int rdrand32(uint32_t *value) {
    unsigned char ok;
    uint32_t result;
    /* RDRAND EAX. Encoding it explicitly keeps the freestanding compiler
     * independent of the host compiler's selected CPU feature set. */
    __asm__ volatile(".byte 0x0f,0xc7,0xf0; setc %1"
                     : "=a"(result), "=qm"(ok) : : "cc");
    *value = result;
    return ok != 0;
}

static int secure_entropy(unsigned char output[32]) {
    if (!cpu_has_rdrand())
        return -1;
    for (size_t offset = 0; offset < 32; offset += sizeof(uint32_t)) {
        uint32_t value = 0;
        int valid = 0;
        for (int attempt = 0; attempt < 10; attempt++) {
            if (rdrand32(&value) && value != 0 && value != 0xffffffffu) {
                valid = 1;
                break;
            }
        }
        if (!valid)
            return -1;
        memcpy(output + offset, &value, sizeof(value));
    }
    return 0;
}

static int socket_read(void *context, unsigned char *buffer, size_t length) {
    int result = recv(*(int *)context, buffer, length, 0);
    return result > 0 ? result : -1;
}

static int socket_write(void *context, const unsigned char *buffer,
                        size_t length) {
    int result = send(*(int *)context, buffer, length, 0);
    return result > 0 ? result : -1;
}

struct buzzos_tls *buzzos_tls_open(int socket_fd, const char *host,
                                   int *error_code) {
    if (error_code) *error_code = 0;
    int32_t now = time(NULL);
    if (now < 1577836800) {
        if (error_code) *error_code = -1001; /* RTC unavailable/invalid. */
        return NULL;
    }
    unsigned char entropy[32];
    if (secure_entropy(entropy) < 0) {
        if (error_code) *error_code = -1002; /* No secure entropy source. */
        return NULL;
    }

    struct buzzos_tls *tls = calloc(1, sizeof(*tls));
    if (!tls) {
        if (error_code) *error_code = -1003;
        return NULL;
    }
    tls->socket_fd = socket_fd;
    br_ssl_client_init_full(&tls->client, &tls->x509, TAs, TAs_NUM);
    br_ssl_engine_set_versions(&tls->client.eng, BR_TLS12, BR_TLS12);
    br_x509_minimal_set_time(&tls->x509,
        719528u + (uint32_t)now / 86400u, (uint32_t)now % 86400u);
    br_ssl_engine_set_buffer(&tls->client.eng, tls->iobuf,
                             sizeof(tls->iobuf), 1);
    br_ssl_engine_inject_entropy(&tls->client.eng, entropy, sizeof(entropy));
    memset(entropy, 0, sizeof(entropy));
    if (!br_ssl_client_reset(&tls->client, host, 0)) {
        if (error_code) *error_code = br_ssl_engine_last_error(&tls->client.eng);
        free(tls);
        return NULL;
    }
    br_sslio_init(&tls->io, &tls->client.eng,
                  socket_read, &tls->socket_fd,
                  socket_write, &tls->socket_fd);
    return tls;
}

int buzzos_tls_write_all(struct buzzos_tls *tls, const void *data, size_t len) {
    if (!tls || br_sslio_write_all(&tls->io, data, len) < 0)
        return -1;
    return br_sslio_flush(&tls->io) < 0 ? -1 : 0;
}

int buzzos_tls_read(struct buzzos_tls *tls, void *data, size_t len) {
    if (!tls) return -1;
    return br_sslio_read(&tls->io, data, len);
}

int buzzos_tls_error(const struct buzzos_tls *tls) {
    if (!tls) return -1;
    return br_ssl_engine_last_error(&tls->client.eng);
}

void buzzos_tls_close(struct buzzos_tls *tls) {
    if (!tls) return;
    memset(tls, 0, sizeof(*tls));
    free(tls);
}
