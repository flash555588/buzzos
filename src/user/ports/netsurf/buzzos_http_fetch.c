#include "libc.h"
#include <stdio.h>

#include <libwapcaplet/libwapcaplet.h>

#include "content/fetch.h"
#include "content/fetchers.h"
#include "utils/corestrings.h"
#include "utils/nsurl.h"

#include "buzzos_http_fetch.h"
#include "buzzos_tls.h"

#define HTTP_HOST_CAP 256
#define HTTP_PATH_CAP 2048
#define HTTP_RESPONSE_INITIAL 16384
#define HTTP_RESPONSE_MAX (4 * 1024 * 1024)
#define DNS_CACHE_SLOTS 8
#define HTTP_USER_AGENT "NetSurf-BuzzOS/0.1"

struct buzzos_http_context {
    struct fetch *parent;
    nsurl *url;
    int aborted;
    struct buzzos_http_context *next;
};

static struct buzzos_http_context *pending;

struct dns_cache_entry {
    char host[HTTP_HOST_CAP];
    uint32_t ip;
};
static struct dns_cache_entry dns_cache[DNS_CACHE_SLOTS];
static unsigned int dns_cache_next;

static int parse_ipv4(const char *s, uint32_t *out) {
    uint32_t parts[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9') return -1;
        while (*s >= '0' && *s <= '9') {
            parts[i] = parts[i] * 10u + (uint32_t)(*s++ - '0');
            if (parts[i] > 255u) return -1;
        }
        if (i < 3) {
            if (*s++ != '.') return -1;
        } else if (*s) {
            return -1;
        }
    }
    *out = parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
    return 0;
}

static int resolve_host(const char *host, uint32_t *ip) {
    if (parse_ipv4(host, ip) == 0) return 0;
    for (unsigned int i = 0; i < DNS_CACHE_SLOTS; i++) {
        if (dns_cache[i].host[0] && !strcasecmp(dns_cache[i].host, host)) {
            *ip = dns_cache[i].ip;
            return 0;
        }
    }
    if (dns_resolve(host, ip) < 0) return -1;
    struct dns_cache_entry *entry = &dns_cache[dns_cache_next++ % DNS_CACHE_SLOTS];
    strncpy(entry->host, host, sizeof(entry->host) - 1);
    entry->host[sizeof(entry->host) - 1] = 0;
    entry->ip = *ip;
    return 0;
}

static int parse_http_url(const char *url, char *host, int *port, char *path,
                          int *secure) {
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) {
        p += 7; *port = 80; *secure = 0;
    } else if (strncmp(p, "https://", 8) == 0) {
        p += 8; *port = 443; *secure = 1;
    } else {
        return -1;
    }
    int hn = 0;
    while (*p && *p != '/' && *p != ':' && hn + 1 < HTTP_HOST_CAP)
        host[hn++] = *p++;
    host[hn] = 0;
    if (!host[0]) return -1;
    if (*p == ':') {
        p++;
        int value = 0;
        if (*p < '0' || *p > '9') return -1;
        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (*p++ - '0');
            if (value > 65535) return -1;
        }
        if (value < 1) return -1;
        *port = value;
    }
    if (!*p) p = "/";
    if (*p != '/' || strlen(p) + 1 > HTTP_PATH_CAP) return -1;
    strcpy(path, p);
    return 0;
}

static int send_all(int sd, const void *buffer, int length) {
    int done = 0;
    while (done < length) {
        int amount = send(sd, (const uint8_t *)buffer + done,
                          (size_t)(length - done), 0);
        if (amount <= 0) return -1;
        done += amount;
    }
    return 0;
}

static void send_error(struct buzzos_http_context *ctx, const char *text) {
    if (ctx->aborted) return;
    fprintf(stderr, "[http] %s: %s\n", nsurl_access(ctx->url), text);
    fetch_msg msg;
    msg.type = FETCH_ERROR;
    msg.data.error = text;
    fetch_send_callback(&msg, ctx->parent);
}

static int find_header_end(const uint8_t *data, int length) {
    for (int i = 0; i + 3 < length; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n' &&
            data[i + 2] == '\r' && data[i + 3] == '\n')
            return i + 4;
    }
    return -1;
}

static int response_content_length(const uint8_t *data, int header_end) {
    int pos = 0;
    while (pos + 1 < header_end) {
        int start = pos;
        while (pos + 1 < header_end &&
               !(data[pos] == '\r' && data[pos + 1] == '\n')) pos++;
        int line_length = pos - start;
        if (line_length > 15 &&
            strncasecmp((const char *)data + start, "Content-Length:", 15) == 0) {
            int value = start + 15;
            while (value < pos && (data[value] == ' ' || data[value] == '\t')) value++;
            int amount = 0;
            if (value == pos) return -1;
            while (value < pos && data[value] >= '0' && data[value] <= '9') {
                if (amount > (HTTP_RESPONSE_MAX - (data[value] - '0')) / 10)
                    return -1;
                amount = amount * 10 + (data[value++] - '0');
            }
            return amount;
        }
        pos += 2;
    }
    return -1;
}

static int response_is_chunked(const uint8_t *data, int header_end) {
    int pos = 0;
    while (pos + 1 < header_end) {
        int start = pos;
        while (pos + 1 < header_end &&
               !(data[pos] == '\r' && data[pos + 1] == '\n')) pos++;
        int line_length = pos - start;
        if (line_length > 18 &&
            strncasecmp((const char *)data + start, "Transfer-Encoding:", 18) == 0) {
            for (int i = start + 18; i + 7 <= pos; i++)
                if (strncasecmp((const char *)data + i, "chunked", 7) == 0)
                    return 1;
        }
        pos += 2;
    }
    return 0;
}

static int hex_value(uint8_t value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

/* Return the byte after the complete chunked message, or -1 if more data is
 * needed. Extensions and trailers are accepted and discarded. */
static int chunked_message_end(const uint8_t *data, int body, int length) {
    int pos = body;
    for (;;) {
        int line_end = pos;
        while (line_end + 1 < length &&
               !(data[line_end] == '\r' && data[line_end + 1] == '\n')) line_end++;
        if (line_end + 1 >= length) return -1;
        int size = 0;
        int digits = 0;
        for (int i = pos; i < line_end && data[i] != ';' && data[i] != ' '; i++) {
            int digit = hex_value(data[i]);
            if (digit < 0 || size > (HTTP_RESPONSE_MAX - digit) / 16) return -1;
            size = size * 16 + digit;
            digits++;
        }
        if (!digits) return -1;
        pos = line_end + 2;
        if (size == 0) {
            if (pos + 1 < length && data[pos] == '\r' && data[pos + 1] == '\n')
                return pos + 2;
            for (int i = pos; i + 3 < length; i++)
                if (data[i] == '\r' && data[i + 1] == '\n' &&
                    data[i + 2] == '\r' && data[i + 3] == '\n') return i + 4;
            return -1;
        }
        if (size > length - pos || pos + size + 1 >= length) return -1;
        pos += size;
        if (data[pos] != '\r' || data[pos + 1] != '\n') return -1;
        pos += 2;
    }
}

static int decode_chunked(uint8_t *data, int body, int message_end) {
    int read_pos = body;
    int write_pos = body;
    while (read_pos < message_end) {
        int line_end = read_pos;
        while (!(data[line_end] == '\r' && data[line_end + 1] == '\n')) line_end++;
        int size = 0;
        for (int i = read_pos; i < line_end && data[i] != ';' && data[i] != ' '; i++)
            size = size * 16 + hex_value(data[i]);
        read_pos = line_end + 2;
        if (size == 0) break;
        memmove(data + write_pos, data + read_pos, (size_t)size);
        write_pos += size;
        read_pos += size + 2;
    }
    return write_pos;
}

static int status_code(const uint8_t *data, int length) {
    int i = 0;
    while (i < length && data[i] != ' ') i++;
    while (i < length && data[i] == ' ') i++;
    if (i + 2 >= length || data[i] < '0' || data[i] > '9') return 0;
    return (data[i] - '0') * 100 + (data[i + 1] - '0') * 10 +
           (data[i + 2] - '0');
}

static int header_name_is(const uint8_t *line, int length, const char *name) {
    int n = (int)strlen(name);
    return length > n && line[n] == ':' && strncasecmp((const char *)line, name, n) == 0;
}

static void deliver_header(struct buzzos_http_context *ctx,
                           const uint8_t *line, int length) {
    uint8_t *copy = malloc((size_t)length + 3u);
    if (!copy) return;
    memcpy(copy, line, (size_t)length);
    copy[length] = '\r';
    copy[length + 1] = '\n';
    copy[length + 2] = 0;
    fetch_msg header;
    header.type = FETCH_HEADER;
    header.data.header_or_data.buf = copy;
    header.data.header_or_data.len = (size_t)length + 2u;
    fetch_send_callback(&header, ctx->parent);
    free(copy);
}

static void deliver_response(struct buzzos_http_context *ctx,
                             uint8_t *data, int length) {
    int body = find_header_end(data, length);
    if (body < 0) {
        send_error(ctx, "Malformed HTTP response");
        return;
    }
    int code = status_code(data, body);
    fetch_set_http_code(ctx->parent, code);

    const char *redirect = NULL;
    int pos = 0;
    while (pos + 1 < body && !(data[pos] == '\r' && data[pos + 1] == '\n'))
        pos++;
    deliver_header(ctx, data, pos);
    if (ctx->aborted) return;
    pos += 2;
    while (pos + 1 < body) {
        int start = pos;
        while (pos + 1 < body && !(data[pos] == '\r' && data[pos + 1] == '\n'))
            pos++;
        int line_length = pos - start;
        if (line_length == 0) break;
        if (header_name_is(data + start, line_length, "Location")) {
            int value = start + 9;
            while (value < pos && (data[value] == ' ' || data[value] == '\t')) value++;
            data[pos] = 0;
            redirect = (const char *)data + value;
        }
        deliver_header(ctx, data + start, line_length);
        if (ctx->aborted) return;
        pos += 2;
    }

    fetch_msg msg;
    if (code >= 300 && code < 400 && redirect) {
        msg.type = FETCH_REDIRECT;
        msg.data.redirect = redirect;
        fetch_send_callback(&msg, ctx->parent);
        return;
    }
    if (body < length) {
        msg.type = FETCH_DATA;
        msg.data.header_or_data.buf = data + body;
        msg.data.header_or_data.len = (size_t)(length - body);
        fetch_send_callback(&msg, ctx->parent);
        if (ctx->aborted) return;
    }
    msg.type = FETCH_FINISHED;
    fetch_send_callback(&msg, ctx->parent);
}

static void perform_fetch(struct buzzos_http_context *ctx) {
    char host[HTTP_HOST_CAP];
    char path[HTTP_PATH_CAP];
    int port;
    int secure;
    if (parse_http_url(nsurl_access(ctx->url), host, &port, path, &secure) < 0) {
        send_error(ctx, "Invalid HTTP/HTTPS URL");
        return;
    }
    fprintf(stderr, "[http] GET %s ua=%s\n",
            nsurl_access(ctx->url), HTTP_USER_AGENT);
    uint32_t ip;
    if (resolve_host(host, &ip) < 0) {
        send_error(ctx, "DNS lookup failed");
        return;
    }
    int sd = socket(AF_INET, SOCK_STREAM, 0);
    if (sd < 0) {
        send_error(ctx, "Could not create TCP socket");
        return;
    }
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr = ip;
    if (connect(sd, &address, sizeof(address)) < 0) {
        closesocket(sd);
        send_error(ctx, "TCP connection failed");
        return;
    }
    char host_header[HTTP_HOST_CAP + 8];
    if (port == (secure ? 443 : 80))
        snprintf(host_header, sizeof(host_header), "%s", host);
    else
        snprintf(host_header, sizeof(host_header), "%s:%d", host, port);
    char request[HTTP_PATH_CAP + HTTP_HOST_CAP + 160];
    int request_length = snprintf(
        request, sizeof(request),
        "GET %s HTTP/1.0\r\nHost: %s\r\n"
        "User-Agent: " HTTP_USER_AGENT "\r\nAccept: */*\r\n"
        "Connection: close\r\n\r\n",
        path, host_header);
    if (request_length <= 0 || request_length >= (int)sizeof(request)) {
        closesocket(sd);
        send_error(ctx, "HTTP request is too large");
        return;
    }
    struct buzzos_tls *tls = NULL;
    int tls_error = 0;
    if (secure) {
        tls = buzzos_tls_open(sd, host, &tls_error);
        if (!tls) {
            fprintf(stderr, "[tls] handshake setup failed error=%d host=%s\n",
                    tls_error, host);
            closesocket(sd);
            send_error(ctx, "TLS setup failed");
            return;
        }
    }
    int sent = secure ? buzzos_tls_write_all(tls, request, (size_t)request_length) :
                        send_all(sd, request, request_length);
    if (sent < 0) {
        if (tls) {
            fprintf(stderr, "[tls] write failed error=%d host=%s\n",
                    buzzos_tls_error(tls), host);
            buzzos_tls_close(tls);
        }
        closesocket(sd);
        send_error(ctx, secure ? "HTTPS request failed" : "HTTP request failed");
        return;
    }
    int capacity = HTTP_RESPONSE_INITIAL;
    int length = 0;
    int framing_logged = 0;
    uint8_t *response = malloc((size_t)capacity);
    if (!response) {
        buzzos_tls_close(tls);
        closesocket(sd);
        send_error(ctx, "Out of memory");
        return;
    }
    while (!ctx->aborted) {
        if (length == capacity) {
            if (capacity >= HTTP_RESPONSE_MAX) {
                send_error(ctx, "HTTP response is too large");
                break;
            }
            int next = capacity * 2;
            if (next > HTTP_RESPONSE_MAX) next = HTTP_RESPONSE_MAX;
            uint8_t *grown = realloc(response, (size_t)next);
            if (!grown) {
                send_error(ctx, "Out of memory");
                break;
            }
            response = grown;
            capacity = next;
        }
        int got = secure ? buzzos_tls_read(tls, response + length,
                                          (size_t)(capacity - length)) :
                           recv(sd, response + length,
                                (size_t)(capacity - length), 0);
        if (got < 0) {
            if (secure)
                fprintf(stderr, "[tls] read failed error=%d host=%s\n",
                        buzzos_tls_error(tls), host);
            send_error(ctx, secure ? "HTTPS receive failed" : "HTTP receive failed");
            break;
        }
        if (got == 0) {
            fprintf(stderr, "[http] peer-close bytes=%d %s\n",
                    length, nsurl_access(ctx->url));
            deliver_response(ctx, response, length);
            break;
        }
        length += got;
        int header_end = find_header_end(response, length);
        if (header_end >= 0) {
            int chunked = response_is_chunked(response, header_end);
            int content_length = response_content_length(response, header_end);
            if (!framing_logged) {
                fprintf(stderr,
                        "[http] response=%d framing=%s content-length=%d received=%d %s\n",
                        status_code(response, header_end),
                        chunked ? "chunked" : (content_length >= 0 ? "length" : "close"),
                        content_length, length - header_end, nsurl_access(ctx->url));
                framing_logged = 1;
            }
            if (chunked) {
                int message_end = chunked_message_end(response, header_end, length);
                if (message_end >= 0) {
                    int decoded_end = decode_chunked(response, header_end, message_end);
                    fprintf(stderr, "[http] chunked-done bytes=%d %s\n",
                            decoded_end - header_end, nsurl_access(ctx->url));
                    deliver_response(ctx, response, decoded_end);
                    break;
                }
            } else {
                if (content_length >= 0 && length - header_end >= content_length) {
                fprintf(stderr, "[http] length-done bytes=%d %s\n",
                        content_length, nsurl_access(ctx->url));
                deliver_response(ctx, response, header_end + content_length);
                break;
                }
            }
        }
    }
    free(response);
    buzzos_tls_close(tls);
    closesocket(sd);
}

static bool http_initialise(lwc_string *scheme) { (void)scheme; return true; }
static void http_finalise(lwc_string *scheme) { (void)scheme; }
static bool http_acceptable(const nsurl *url) { (void)url; return true; }

static void *http_setup(struct fetch *parent, nsurl *url, bool only_2xx,
                        bool downgrade_tls, const char *post_urlenc,
                        const struct fetch_multipart_data *post_multipart,
                        const char **headers) {
    (void)only_2xx; (void)downgrade_tls; (void)post_urlenc;
    (void)post_multipart; (void)headers;
    struct buzzos_http_context *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->parent = parent;
    ctx->url = nsurl_ref(url);
    ctx->next = pending;
    pending = ctx;
    return ctx;
}

static bool http_start(void *context) { (void)context; return true; }
static void http_abort(void *context) {
    ((struct buzzos_http_context *)context)->aborted = 1;
}
static void http_free(void *context) {
    struct buzzos_http_context *ctx = context;
    nsurl_unref(ctx->url);
    free(ctx);
}

static void http_poll(lwc_string *scheme) {
    (void)scheme;
    while (pending) {
        struct buzzos_http_context *ctx = pending;
        pending = ctx->next;
        if (!ctx->aborted)
            perform_fetch(ctx);
        fetch_remove_from_queues(ctx->parent);
        fetch_free(ctx->parent);
    }
}

nserror buzzos_http_fetch_register(void) {
    const struct fetcher_operation_table ops = {
        .initialise = http_initialise,
        .acceptable = http_acceptable,
        .setup = http_setup,
        .start = http_start,
        .abort = http_abort,
        .free = http_free,
        .poll = http_poll,
        .finalise = http_finalise,
    };
    nserror error = fetcher_add(lwc_string_ref(corestring_lwc_http), &ops);
    if (error != NSERROR_OK)
        return error;
    return fetcher_add(lwc_string_ref(corestring_lwc_https), &ops);
}
