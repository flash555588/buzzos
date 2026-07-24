#include "libc.h"
#include <stdio.h>
#include <time.h>
#include <signal.h>
#include <locale.h>
#include <iconv.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/utsname.h>
#include <sys/time.h>

/* ================================================================
 *  Syscall wrappers (int 0x80, Linux-like ABI)
 * ================================================================ */

static int syscall0(int nr) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr) : "memory");
    return ret;
}

static int syscall1(int nr, int a1) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr), "b"(a1) : "memory");
    return ret;
}

static int syscall2(int nr, int a1, int a2) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret)
                     : "a"(nr), "b"(a1), "c"(a2) : "memory");
    return ret;
}

static int syscall3(int nr, int a1, int a2, int a3) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret)
                     : "a"(nr), "b"(a1), "c"(a2), "d"(a3) : "memory");
    return ret;
}

static int syscall5(int nr, int a1, int a2, int a3, int a4, int a5) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret)
                     : "a"(nr), "b"(a1), "c"(a2), "d"(a3), "S"(a4), "D"(a5)
                     : "memory");
    return ret;
}

static int gfx_origin_x;
static int gfx_origin_y;

enum { SYS_EXIT=1, SYS_OPEN=2, SYS_CLOSE=3, SYS_READ=4, SYS_WRITE=5,
       SYS_DUP=16, SYS_DUP2=17, SYS_STAT=18, SYS_GETDENTS=19,
       SYS_SPAWN_PROC=20, SYS_PS=21, SYS_REBOOT=22, SYS_MKDIR=23,
       SYS_UNLINK=24, SYS_CREATE=25, SYS_SPAWN_PROC_ARGS=26,
       SYS_LSEEK=27, SYS_RMDIR=28, SYS_RENAME=29, SYS_SOCKET=30,
       SYS_CONNECT=31, SYS_SEND=32, SYS_RECV=33, SYS_CLOSESOCKET=34,
       SYS_DNS_RESOLVE=35, SYS_BIND=36, SYS_SENDTO=37, SYS_RECVFROM=38,
       SYS_NETINFO=39, SYS_PIPE=40, SYS_FUTEX_WAIT=41, SYS_FUTEX_WAKE=42,
       SYS_GFX_CLEAR=44, SYS_GFX_PUTPIXEL=45,
       SYS_GFX_FILL_RECT=46, SYS_GFX_TEXT=47, SYS_FB_BLIT=48,
       SYS_MOUSE_GET=49, SYS_FSSTAT=50, SYS_FUTEX_WAIT_TIMEOUT=51,
       SYS_GFX_INFO=52, SYS_FONT_GLYPH=53, SYS_SBRK=54,
       SYS_SHM_CREATE=57, SYS_SHM_MAP=58, SYS_SHM_UNMAP=59,
       SYS_AUDIO_WRITE=60, SYS_AUDIO_CONFIG=61, SYS_FB_BLIT_STRIDE=62,
       SYS_AUDIO_QUEUED=63, SYS_AUDIO_FLUSH=64,
       SYS_GFX_ACQUIRE=65, SYS_GFX_RELEASE=66, SYS_GFX_SET_MODE=67 };

static void (*exit_handlers[16])(void);
static int exit_handler_count;

int atexit(void (*function)(void)) {
    if (!function || exit_handler_count >= 16) return -1;
    exit_handlers[exit_handler_count++] = function;
    return 0;
}

void exit(int code) {
    while (exit_handler_count > 0)
        exit_handlers[--exit_handler_count]();
    syscall1(SYS_EXIT, code);
    __builtin_unreachable();
}

int open(const char *path, int flags, ...) {
    return syscall2(SYS_OPEN, (int)(uintptr_t)path, flags);
}

int close(int fd) {
    return syscall1(SYS_CLOSE, fd);
}

int dup(int fd) {
    return syscall1(SYS_DUP, fd);
}

int dup2(int oldfd, int newfd) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret)
                     : "a"(SYS_DUP2), "b"(oldfd), "c"(newfd) : "memory");
    return ret;
}

int stat(const char *path, struct stat *st) {
    return syscall3(SYS_STAT, (int)(uintptr_t)path, (int)(uintptr_t)st, 0);
}

int fsstat(struct fs_info *info) {
    return syscall1(SYS_FSSTAT, (int)(uintptr_t)info);
}

int getdents(int fd, struct dirent *ents, size_t count) {
    return syscall3(SYS_GETDENTS, fd, (int)(uintptr_t)ents, (int)count);
}

int spawn_process(const char *path, int flags) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret)
                     : "a"(SYS_SPAWN_PROC), "b"((int)(uintptr_t)path), "c"(flags)
                     : "memory");
    return ret;
}

int spawn_process_args(const char *path, char *const argv[], int argc, int flags) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret)
                     : "a"(SYS_SPAWN_PROC_ARGS), "b"((int)(uintptr_t)path),
                       "c"((int)(uintptr_t)argv), "d"(argc), "S"(flags)
                     : "memory");
    return ret;
}

int ps(char *buf, size_t size, int show_dead) {
    return syscall3(SYS_PS, (int)(uintptr_t)buf, (int)size, show_dead);
}

void reboot(void) {
    syscall0(SYS_REBOOT);
    __builtin_unreachable();
}

int mkdir(const char *path) {
    return syscall1(SYS_MKDIR, (int)(uintptr_t)path);
}

int unlink(const char *path) {
    return syscall1(SYS_UNLINK, (int)(uintptr_t)path);
}

int rmdir(const char *path) {
    return syscall1(SYS_RMDIR, (int)(uintptr_t)path);
}

int rename(const char *old_path, const char *new_path) {
    return syscall2(SYS_RENAME, (int)(uintptr_t)old_path, (int)(uintptr_t)new_path);
}

int create(const char *path) {
    int fd = open(path, O_CREAT | O_WRONLY);
    if (fd < 0)
        return -1;
    return close(fd);
}

int read(int fd, void *buf, size_t count) {
    return syscall3(SYS_READ, fd, (int)(uintptr_t)buf, (int)count);
}

int write(int fd, const void *buf, size_t count) {
    return syscall3(SYS_WRITE, fd, (int)(uintptr_t)buf, (int)count);
}

int lseek(int fd, int offset, int whence) {
    return syscall3(SYS_LSEEK, fd, offset, whence);
}

int socket(int domain, int type, int protocol) {
    return syscall3(SYS_SOCKET, domain, type, protocol);
}

int bind(int sd, const struct sockaddr_in *addr, size_t addrlen) {
    return syscall3(SYS_BIND, sd, (int)(uintptr_t)addr, (int)addrlen);
}

int connect(int sd, const struct sockaddr_in *addr, size_t addrlen) {
    return syscall3(SYS_CONNECT, sd, (int)(uintptr_t)addr, (int)addrlen);
}

int send(int sd, const void *buf, size_t len, int flags) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret)
                     : "a"(SYS_SEND), "b"(sd), "c"((int)(uintptr_t)buf),
                       "d"((int)len), "S"(flags)
                     : "memory");
    return ret;
}

int recv(int sd, void *buf, size_t len, int flags) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret)
                     : "a"(SYS_RECV), "b"(sd), "c"((int)(uintptr_t)buf),
                       "d"((int)len), "S"(flags)
                     : "memory");
    return ret;
}

int sendto(int sd, const void *buf, size_t len, int flags,
           const struct sockaddr_in *addr, size_t addrlen) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret)
                     : "a"(SYS_SENDTO), "b"(sd), "c"((int)(uintptr_t)buf),
                       "d"((int)len), "S"((int)(uintptr_t)addr), "D"((int)addrlen)
                     : "memory");
    (void)flags;
    return ret;
}

int recvfrom(int sd, void *buf, size_t len, int flags,
             struct sockaddr_in *addr, size_t addrlen) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret)
                     : "a"(SYS_RECVFROM), "b"(sd), "c"((int)(uintptr_t)buf),
                       "d"((int)len), "S"((int)(uintptr_t)addr), "D"((int)addrlen)
                     : "memory");
    (void)flags;
    return ret;
}

int closesocket(int sd) {
    return syscall1(SYS_CLOSESOCKET, sd);
}

int dns_resolve(const char *host, uint32_t *ip_out) {
    return syscall2(SYS_DNS_RESOLVE, (int)(uintptr_t)host, (int)(uintptr_t)ip_out);
}

int net_info(uint8_t mac[6], uint32_t *ip_out) {
    return syscall2(SYS_NETINFO, (int)(uintptr_t)mac, (int)(uintptr_t)ip_out);
}

uint16_t htons(uint16_t v) {
    return (uint16_t)((v << 8) | (v >> 8));
}

uint16_t ntohs(uint16_t v) {
    return htons(v);
}

int gfx_clear(int color) {
    return syscall1(SYS_GFX_CLEAR, color);
}

int gfx_putpixel(int x, int y, int color) {
    return syscall3(SYS_GFX_PUTPIXEL, x + gfx_origin_x, y + gfx_origin_y, color);
}

int buzz_mkdir_mode(const char *path, unsigned int mode) {
    (void)mode;
    return mkdir(path);
}

int gfx_fill_rect(int x, int y, int w, int h, int color) {
    return syscall5(SYS_GFX_FILL_RECT, x + gfx_origin_x, y + gfx_origin_y, w, h, color);
}

int gfx_text(int x, int y, const char *s, int fg, int bg) {
    return syscall5(SYS_GFX_TEXT, x + gfx_origin_x, y + gfx_origin_y,
                    (int)(uintptr_t)s, fg, bg);
}

int fb_blit(int x, int y, int w, int h, const uint8_t *pixels) {
    return syscall5(SYS_FB_BLIT, x + gfx_origin_x, y + gfx_origin_y, w, h,
                    (int)(uintptr_t)pixels);
}

int fb_blit_stride(int x, int y, int w, int h, const uint8_t *pixels,
                   int stride) {
    if (w <= 0 || h <= 0 || w > 0xFFFF || h > 0xFFFF || stride < w)
        return -1;
    unsigned int packed = (unsigned int)w | ((unsigned int)h << 16);
    return syscall5(SYS_FB_BLIT_STRIDE, x + gfx_origin_x, y + gfx_origin_y,
                    (int)packed, (int)(uintptr_t)pixels, stride);
}

int mouse_get(struct mouse_state *out) {
    int ret = syscall1(SYS_MOUSE_GET, (int)(uintptr_t)out);
    if (ret == 0 && out) {
        out->x -= gfx_origin_x;
        out->y -= gfx_origin_y;
    }
    return ret;
}

int gfx_info(struct gfx_info *out) {
    return syscall1(SYS_GFX_INFO, (int)(uintptr_t)out);
}

int gfx_acquire_display(void) {
    return syscall0(SYS_GFX_ACQUIRE);
}

int gfx_release_display(void) {
    return syscall0(SYS_GFX_RELEASE);
}

int gfx_set_mode(int width, int height) {
    return syscall2(SYS_GFX_SET_MODE, width, height);
}

int font_glyph(uint32_t codepoint, uint8_t *bits, size_t cap) {
    return syscall3(SYS_FONT_GLYPH, (int)codepoint,
                    (int)(uintptr_t)bits, (int)cap);
}

void gfx_set_origin(int x, int y) {
    gfx_origin_x = x;
    gfx_origin_y = y;
}

void gfx_get_origin(int *x_out, int *y_out) {
    if (x_out)
        *x_out = gfx_origin_x;
    if (y_out)
        *y_out = gfx_origin_y;
}

enum { SYS_SPAWN=6, SYS_YIELD=7, SYS_JOIN=8, SYS_SLEEP=9, SYS_KILL=10,
       SYS_GETPID=11, SYS_GETTID=12, SYS_CHDIR=13, SYS_GETCWD=14,
       SYS_WAITPID=15, SYS_MONOTONIC_MS=55, SYS_REALTIME=56 };

int kill(int pid) {
    return syscall1(SYS_KILL, pid);
}

int getpid(void) {
    return syscall0(SYS_GETPID);
}

int gettid(void) {
    return syscall0(SYS_GETTID);
}

int chdir(const char *path) {
    return syscall1(SYS_CHDIR, (int)(uintptr_t)path);
}

char *getcwd(char *buf, size_t size) {
    if (syscall3(SYS_GETCWD, (int)(uintptr_t)buf, (int)size, 0) < 0)
        return (char *)0;
    return buf;
}

int waitpid(int pid, int *status, int options) {
    return syscall3(SYS_WAITPID, pid, (int)(uintptr_t)status, options);
}

static void thread_return_trampoline(void) {
    /*
     * A returning user thread must not run process-wide atexit handlers.
     * SYS_EXIT already distinguishes a worker TID from the process leader
     * and retires only that thread. Calling exit() here used to run handlers
     * such as the GUI display release while the main thread was still alive.
     */
    syscall1(SYS_EXIT, 0);
    __builtin_unreachable();
}

int pipe(int fds[2]) {
    return syscall1(SYS_PIPE, (int)(uintptr_t)fds);
}

int futex_wait(int *addr, int expected) {
    return syscall2(SYS_FUTEX_WAIT, (int)(uintptr_t)addr, expected);
}

int futex_wait_timeout(int *addr, int expected, unsigned int timeout_ms) {
    return syscall3(SYS_FUTEX_WAIT_TIMEOUT, (int)(uintptr_t)addr, expected, (int)timeout_ms);
}

int futex_wake(int *addr, int count) {
    return syscall2(SYS_FUTEX_WAKE, (int)(uintptr_t)addr, count);
}

int spawn(thread_fn func) {
    return syscall2(SYS_SPAWN, (int)(uintptr_t)func, (int)(uintptr_t)thread_return_trampoline);
}

void yield(void) {
    syscall0(SYS_YIELD);
}

int join(int tid) {
    return syscall1(SYS_JOIN, tid);
}

void sleep_ms(unsigned int ms) {
    syscall1(SYS_SLEEP, (int)ms);
}

int shm_create(size_t size, struct shm_mapping *mapping) {
    return syscall2(SYS_SHM_CREATE, (int)size, (int)(uintptr_t)mapping);
}

int shm_map(uint32_t token, struct shm_mapping *mapping) {
    return syscall2(SYS_SHM_MAP, (int)token, (int)(uintptr_t)mapping);
}

int shm_unmap(uint32_t token) {
    return syscall1(SYS_SHM_UNMAP, (int)token);
}

int audio_write(const uint8_t *samples, size_t count) {
    return syscall2(SYS_AUDIO_WRITE, (int)(uintptr_t)samples, (int)count);
}

int audio_config(unsigned int sample_rate) {
    /* SYS_AUDIO_CONFIG also accepts an optional latency in ECX.  Pass an
     * explicit zero so the one-argument API never leaks a stale register
     * value into the kernel. */
    return syscall2(SYS_AUDIO_CONFIG, (int)sample_rate, 0);
}

int audio_config_latency(unsigned int sample_rate, unsigned int latency_ms) {
    return syscall2(SYS_AUDIO_CONFIG, (int)sample_rate, (int)latency_ms);
}

int audio_queued(void) {
    return syscall0(SYS_AUDIO_QUEUED);
}

int audio_flush(void) {
    return syscall0(SYS_AUDIO_FLUSH);
}

uint32_t monotonic_ms(void) {
    return (uint32_t)syscall0(SYS_MONOTONIC_MS);
}

int32_t time(int32_t *result) {
    int32_t seconds = (int32_t)syscall0(SYS_REALTIME);
    if (result)
        *result = seconds;
    return seconds;
}

int gettimeofday(struct timeval *value, void *timezone) {
    (void)timezone;
    if (!value)
        return -1;
    uint32_t milliseconds = monotonic_ms();
    value->tv_sec = (int32_t)syscall0(SYS_REALTIME);
    if (value->tv_sec < 0)
        return -1;
    value->tv_usec = (int32_t)((milliseconds % 1000u) * 1000u);
    return 0;
}

/* ================================================================
 *  String functions
 * ================================================================ */

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

void *memset(void *d, int c, size_t n) {
    unsigned char *p = (unsigned char *)d;
    size_t words = n >> 2;
    uint32_t value = (uint8_t)c;
    value |= value << 8;
    value |= value << 16;
    __asm__ volatile ("cld; rep stosl"
                      : "+D"(p), "+c"(words)
                      : "a"(value)
                      : "memory");
    n &= 3u;
    __asm__ volatile ("rep stosb"
                      : "+D"(p), "+c"(n)
                      : "a"((uint8_t)c)
                      : "memory");
    return d;
}

void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    size_t words = n >> 2;
    __asm__ volatile ("cld; rep movsl"
                      : "+D"(dp), "+S"(sp), "+c"(words)
                      :
                      : "memory");
    n &= 3u;
    __asm__ volatile ("rep movsb"
                      : "+D"(dp), "+S"(sp), "+c"(n)
                      :
                      : "memory");
    return d;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d == s || n == 0)
        return dst;
    if (d < s) {
        for (size_t i = 0; i < n; i++)
            d[i] = s[i];
    } else {
        while (n) {
            n--;
            d[n] = s[n];
        }
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *left = a;
    const unsigned char *right = b;
    for (size_t i = 0; i < n; i++) {
        if (left[i] != right[i])
            return (int)left[i] - (int)right[i];
    }
    return 0;
}

void *memchr(const void *memory, int value, size_t size) {
    const unsigned char *bytes = (const unsigned char *)memory;
    for (size_t i = 0; i < size; i++)
        if (bytes[i] == (unsigned char)value) return (void *)(bytes + i);
    return NULL;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *(unsigned char *)a - *(unsigned char *)b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char left = (unsigned char)a[i];
        unsigned char right = (unsigned char)b[i];
        if (left != right)
            return (int)left - (int)right;
        if (!left)
            return 0;
    }
    return 0;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int left = tolower((unsigned char)a[i]);
        int right = tolower((unsigned char)b[i]);
        if (left != right)
            return left - right;
        if (!left)
            return 0;
    }
    return 0;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    while (i < n && src[i]) {
        dst[i] = src[i];
        i++;
    }
    while (i < n)
        dst[i++] = 0;
    return dst;
}

char *strchr(const char *s, int c) {
    char ch = (char)c;
    for (;;) {
        if (*s == ch)
            return (char *)s;
        if (!*s)
            return 0;
        s++;
    }
}

char *strrchr(const char *s, int c) {
    char ch = (char)c;
    const char *found = 0;
    do {
        if (*s == ch)
            found = s;
    } while (*s++);
    return (char *)found;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    size_t length = strlen(needle);
    for (; *haystack; haystack++)
        if (*haystack == *needle && memcmp(haystack, needle, length) == 0)
            return (char *)haystack;
    return NULL;
}

char *strpbrk(const char *s, const char *accept) {
    for (; *s; s++)
        if (strchr(accept, *s)) return (char *)s;
    return NULL;
}

size_t strspn(const char *s, const char *accept) {
    const char *start = s;
    while (*s && strchr(accept, *s)) s++;
    return (size_t)(s - start);
}

size_t strcspn(const char *s, const char *reject) {
    const char *start = s;
    while (*s && !strchr(reject, *s)) s++;
    return (size_t)(s - start);
}

char *strtok(char *s, const char *delimiters) {
    static char *next;
    if (s) next = s;
    if (!next) return NULL;
    next += strspn(next, delimiters);
    if (!*next) { next = NULL; return NULL; }
    char *token = next;
    next += strcspn(next, delimiters);
    if (*next) *next++ = 0;
    else next = NULL;
    return token;
}

char *strdup(const char *s) {
    size_t length = strlen(s) + 1u;
    char *copy = malloc(length);
    if (copy)
        memcpy(copy, s, length);
    return copy;
}

int atoi(const char *s) {
    int neg = 0, v = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

double atof(const char *s) {
    return strtod(s, NULL);
}

int abs(int value) {
    return value < 0 ? -value : value;
}

static unsigned int random_state = 1;

void srand(unsigned int seed) {
    random_state = seed ? seed : 1;
}

int rand(void) {
    random_state = random_state * 1103515245u + 12345u;
    return (int)(random_state & 0x7fffffffu);
}

static unsigned long parse_unsigned(const char *s, char **end, int base,
                                    int *negative) {
    while (isspace((unsigned char)*s))
        s++;
    *negative = 0;
    if (*s == '+' || *s == '-') {
        *negative = *s == '-';
        s++;
    }
    if ((base == 0 || base == 16) && s[0] == '0' &&
        (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    } else if (base == 0) {
        base = s[0] == '0' ? 8 : 10;
    }
    const char *start = s;
    unsigned long value = 0;
    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9')
            digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z')
            digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z')
            digit = *s - 'A' + 10;
        else
            break;
        if (digit >= base)
            break;
        value = value * (unsigned)base + (unsigned)digit;
        s++;
    }
    if (end)
        *end = (char *)(s == start ? start : s);
    return value;
}

long strtol(const char *s, char **end, int base) {
    int negative;
    unsigned long value = parse_unsigned(s, end, base, &negative);
    return negative ? -(long)value : (long)value;
}

unsigned long strtoul(const char *s, char **end, int base) {
    int negative;
    unsigned long value = parse_unsigned(s, end, base, &negative);
    return negative ? (unsigned long)(-(long)value) : value;
}

unsigned long long strtoull(const char *s, char **end, int base) {
    while (isspace((unsigned char)*s)) s++;
    int negative = 0;
    if (*s == '+' || *s == '-') { negative = *s == '-'; s++; }
    if ((base == 0 || base == 16) && s[0] == '0' &&
        (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    } else if (base == 0) {
        base = s[0] == '0' ? 8 : 10;
    }
    const char *start = s;
    unsigned long long value = 0;
    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9') digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z') digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') digit = *s - 'A' + 10;
        else break;
        if (digit >= base) break;
        value = value * (unsigned)base + (unsigned)digit;
        s++;
    }
    if (end) *end = (char *)(s == start ? start : s);
    return negative ? (unsigned long long)(-(long long)value) : value;
}

long long strtoll(const char *s, char **end, int base) {
    while (isspace((unsigned char)*s)) s++;
    int negative = *s == '-';
    unsigned long long value = strtoull(s, end, base);
    return negative ? -(long long)(0 - value) : (long long)value;
}

double strtod(const char *s, char **end) {
    while (isspace((unsigned char)*s)) s++;
    int negative = 0;
    if (*s == '+' || *s == '-') { negative = *s == '-'; s++; }
    const char *start = s;
    double value = 0.0;
    while (isdigit((unsigned char)*s)) value = value * 10.0 + (*s++ - '0');
    if (*s == '.') {
        double place = 0.1;
        s++;
        while (isdigit((unsigned char)*s)) {
            value += (*s++ - '0') * place;
            place *= 0.1;
        }
    }
    if (end) *end = (char *)(s == start ? start : s);
    return negative ? -value : value;
}

float strtof(const char *s, char **end) {
    return (float)strtod(s, end);
}

/* ================================================================
 *  Standard I/O
 * ================================================================ */

static int console_fd = 1;

int errno;

static int ensure_console(void) {
    if (console_fd < 0)
        console_fd = open("/dev/console", O_WRONLY);
    return console_fd;
}

int putchar(int c) {
    char ch = (char)c;
    if (ensure_console() < 0) return -1;
    write(console_fd, &ch, 1);
    return c;
}

int puts(const char *s) {
    if (ensure_console() < 0) return -1;
    write(console_fd, s, strlen(s));
    write(console_fd, "\n", 1);
    return 0;
}

static void out_ch(char *out, int *pos, int cap, char c) {
    if (*pos < cap - 1)
        out[*pos] = c;
    (*pos)++;
}

static void out_str(char *out, int *pos, int cap, const char *s) {
    while (*s)
        out_ch(out, pos, cap, *s++);
}

static void out_uint(char *out, int *pos, int cap, unsigned int v, int base) {
    char buf[12];
    int i = 0;
    if (v == 0) {
        out_ch(out, pos, cap, '0');
        return;
    }
    while (v && i < (int)sizeof(buf)) {
        int d = (int)(v % (unsigned)base);
        buf[i++] = (char)((d < 10) ? ('0' + d) : ('a' + d - 10));
        v /= (unsigned)base;
    }
    while (i > 0)
        out_ch(out, pos, cap, buf[--i]);
}

static void out_double(char *out, int *pos, int cap, double v, int prec) {
    if (v < 0) {
        out_ch(out, pos, cap, '-');
        v = -v;
    }
    unsigned int integer = (unsigned int)v;
    out_uint(out, pos, cap, integer, 10);
    out_ch(out, pos, cap, '.');
    v -= (double)integer;
    for (int i = 0; i < prec; i++) {
        v *= 10.0;
        int d = (int)v;
        out_ch(out, pos, cap, (char)('0' + d));
        v -= (double)d;
    }
}

static void out_uint64(char *out, int *pos, int cap,
                       unsigned long long v, int base, int upper) {
    char buf[24];
    int i = 0;
    if (v == 0) { out_ch(out, pos, cap, '0'); return; }
    while (v && i < (int)sizeof(buf)) {
        int d = (int)(v % (unsigned)base);
        buf[i++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
        v /= (unsigned)base;
    }
    while (i) out_ch(out, pos, cap, buf[--i]);
}

static int uint64_digits(unsigned long long value, int base) {
    int digits = 1;
    while (value >= (unsigned)base) {
        value /= (unsigned)base;
        digits++;
    }
    return digits;
}

int snprintf(char *buffer, size_t size, const char *fmt, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int result = vsnprintf(buffer, size, fmt, ap);
    __builtin_va_end(ap);
    return result;
}

int vsnprintf(char *buffer, size_t size, const char *fmt,
              __builtin_va_list source_args) {
    int cap = size > 0x7fffffffu ? 0x7fffffff : (int)size;
    int pos = 0;
    __builtin_va_list args;
    __builtin_va_copy(args, source_args);
    while (*fmt) {
        if (*fmt++ != '%') { out_ch(buffer, &pos, cap, fmt[-1]); continue; }
        if (*fmt == '%') { out_ch(buffer, &pos, cap, *fmt++); continue; }
        int left = 0, plus = 0, blank = 0, alternate = 0, zero = 0;
        for (;;) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') blank = 1;
            else if (*fmt == '#') alternate = 1;
            else if (*fmt == '0') zero = 1;
            else break;
            fmt++;
        }
        int width = 0;
        if (*fmt == '*') { width = __builtin_va_arg(args, int); fmt++; }
        else while (isdigit((unsigned char)*fmt)) width = width * 10 + (*fmt++ - '0');
        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') { precision = __builtin_va_arg(args, int); fmt++; }
            else while (isdigit((unsigned char)*fmt)) precision = precision * 10 + (*fmt++ - '0');
        }
        int length = 0;
        if (*fmt == 'l') { length = 1; fmt++; if (*fmt == 'l') { length = 2; fmt++; } }
        else if (*fmt == 'z' || *fmt == 't') { length = 1; fmt++; }
        char spec = *fmt ? *fmt++ : 0;
        if (spec == 's') {
            const char *text = __builtin_va_arg(args, const char *);
            if (!text) text = "(null)";
            int count = (int)strlen(text);
            if (precision >= 0 && count > precision) count = precision;
            while (width-- > count) out_ch(buffer, &pos, cap, ' ');
            for (int i = 0; i < count; i++) out_ch(buffer, &pos, cap, text[i]);
        } else if (spec == 'c') {
            out_ch(buffer, &pos, cap, (char)__builtin_va_arg(args, int));
        } else if (spec == 'p') {
            unsigned long value = (unsigned long)__builtin_va_arg(args, void *);
            out_str(buffer, &pos, cap, "0x");
            out_uint64(buffer, &pos, cap, value, 16, 0);
        } else if (spec == 'd' || spec == 'i') {
            long long value = length == 2 ? __builtin_va_arg(args, long long) :
                (length ? __builtin_va_arg(args, long) : __builtin_va_arg(args, int));
            int negative = value < 0;
            unsigned long long magnitude = negative ?
                (unsigned long long)(-(value + 1)) + 1u :
                (unsigned long long)value;
            int digits = uint64_digits(magnitude, 10);
            int number_width = precision > digits ? precision : digits;
            int sign_width = negative || plus || blank;
            int padding = width - number_width - sign_width;
            char sign = negative ? '-' : (plus ? '+' : (blank ? ' ' : 0));
            if (!left && (!zero || precision >= 0))
                while (padding-- > 0) out_ch(buffer, &pos, cap, ' ');
            if (sign) out_ch(buffer, &pos, cap, sign);
            if (!left && zero && precision < 0)
                while (padding-- > 0) out_ch(buffer, &pos, cap, '0');
            for (int i = digits; i < number_width; i++)
                out_ch(buffer, &pos, cap, '0');
            out_uint64(buffer, &pos, cap, magnitude, 10, 0);
            if (left)
                while (padding-- > 0) out_ch(buffer, &pos, cap, ' ');
        } else if (spec == 'u' || spec == 'x' || spec == 'X' || spec == 'o') {
            unsigned long long value = length == 2 ? __builtin_va_arg(args, unsigned long long) :
                (length ? __builtin_va_arg(args, unsigned long) : __builtin_va_arg(args, unsigned int));
            int base = spec == 'o' ? 8 : ((spec == 'x' || spec == 'X') ? 16 : 10);
            int prefix = alternate && value && (spec == 'x' || spec == 'X') ? 2 :
                         (alternate && value && spec == 'o' ? 1 : 0);
            int digits = uint64_digits(value, base);
            int number_width = precision > digits ? precision : digits;
            int padding = width - number_width - prefix;
            if (!left && (!zero || precision >= 0))
                while (padding-- > 0) out_ch(buffer, &pos, cap, ' ');
            if (prefix == 2) {
                out_ch(buffer, &pos, cap, '0');
                out_ch(buffer, &pos, cap, spec);
            } else if (prefix == 1) {
                out_ch(buffer, &pos, cap, '0');
            }
            if (!left && zero && precision < 0)
                while (padding-- > 0) out_ch(buffer, &pos, cap, '0');
            for (int i = digits; i < number_width; i++)
                out_ch(buffer, &pos, cap, '0');
            out_uint64(buffer, &pos, cap, value, base, spec == 'X');
            if (left)
                while (padding-- > 0) out_ch(buffer, &pos, cap, ' ');
        } else if (spec == 'f' || spec == 'g') {
            double value = __builtin_va_arg(args, double);
            out_double(buffer, &pos, cap, value, precision >= 0 ? precision : 6);
        } else {
            out_ch(buffer, &pos, cap, '%');
            if (spec) out_ch(buffer, &pos, cap, spec);
        }
    }
    __builtin_va_end(args);
    if (cap > 0 && buffer) buffer[pos < cap ? pos : cap - 1] = 0;
    return pos;
}

int printf(const char *fmt, ...) {
    if (ensure_console() < 0) return -1;
    char out[512];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int length = vsnprintf(out, sizeof(out), fmt, ap);
    __builtin_va_end(ap);
    int written = length < (int)sizeof(out) ? length : (int)sizeof(out) - 1;
    if (written > 0) write(console_fd, out, (size_t)written);
    return length;
}

static FILE standard_input = { 0, 0, 0 };
static FILE standard_output = { 1, 0, 0 };
static FILE standard_error = { 2, 0, 0 };
FILE *stdin = &standard_input;
FILE *stdout = &standard_output;
FILE *stderr = &standard_error;

FILE *fopen(const char *path, const char *mode) {
    int flags = O_RDONLY;
    if (mode && mode[0] == 'w') flags = O_WRONLY | O_CREAT | O_TRUNC;
    else if (mode && mode[0] == 'a') flags = O_WRONLY | O_CREAT | O_APPEND;
    else if (mode && strchr(mode, '+')) flags = O_RDWR | O_CREAT;
    int fd = open(path, flags, 0666);
    if (fd < 0) return NULL;
    FILE *stream = malloc(sizeof(*stream));
    if (!stream) { close(fd); return NULL; }
    stream->fd = fd; stream->eof = 0; stream->error = 0;
    return stream;
}

int fclose(FILE *stream) {
    if (!stream) return -1;
    if (stream == stdin || stream == stdout || stream == stderr) return 0;
    int result = close(stream->fd);
    free(stream);
    return result;
}

size_t fread(void *ptr, size_t size, size_t count, FILE *stream) {
    if (!stream || !size || !count) return 0;
    int result = read(stream->fd, ptr, size * count);
    if (result <= 0) { stream->eof = result == 0; stream->error = result < 0; return 0; }
    return (size_t)result / size;
}

size_t fwrite(const void *ptr, size_t size, size_t count, FILE *stream) {
    if (!stream || !size || !count) return 0;
    int result = write(stream->fd, ptr, size * count);
    if (result < 0) { stream->error = 1; return 0; }
    return (size_t)result / size;
}

int fseek(FILE *stream, long offset, int whence) {
    if (!stream) return -1;
    stream->eof = 0;
    return lseek(stream->fd, (int)offset, whence) < 0 ? -1 : 0;
}

long ftell(FILE *stream) { return stream ? lseek(stream->fd, 0, SEEK_CUR) : -1; }
int fgetc(FILE *stream) {
    unsigned char byte;
    return fread(&byte, 1, 1, stream) == 1 ? byte : EOF;
}
int fputc(int character, FILE *stream) {
    unsigned char byte = (unsigned char)character;
    return fwrite(&byte, 1, 1, stream) == 1 ? character : EOF;
}
char *fgets(char *buffer, int size, FILE *stream) {
    if (!buffer || size <= 0 || !stream) return NULL;
    int used = 0;
    while (used < size - 1) {
        int c = fgetc(stream);
        if (c == EOF) break;
        buffer[used++] = (char)c;
        if (c == '\n') break;
    }
    buffer[used] = 0;
    return used ? buffer : NULL;
}
int feof(FILE *stream) { return stream ? stream->eof : 1; }
int ferror(FILE *stream) { return stream ? stream->error : 1; }
int fflush(FILE *stream) { (void)stream; return 0; }
void setbuf(FILE *stream, char *buffer) { (void)stream; (void)buffer; }
int fputs(const char *text, FILE *stream) {
    size_t length = strlen(text);
    return fwrite(text, 1, length, stream) == length ? 0 : EOF;
}
int vfprintf(FILE *stream, const char *fmt, __builtin_va_list args) {
    char buffer[2048];
    int length = vsnprintf(buffer, sizeof(buffer), fmt, args);
    int written = length < (int)sizeof(buffer) ? length : (int)sizeof(buffer) - 1;
    return fwrite(buffer, 1, (size_t)written, stream) == (size_t)written ? length : -1;
}
int fprintf(FILE *stream, const char *fmt, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    int result = vfprintf(stream, fmt, args);
    __builtin_va_end(args);
    return result;
}
int sprintf(char *buffer, const char *fmt, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    int result = vsnprintf(buffer, 0x7fffffffu, fmt, args);
    __builtin_va_end(args);
    return result;
}
int remove(const char *path) { return unlink(path); }
void perror(const char *prefix) {
    if (prefix && *prefix) fprintf(stderr, "%s: %s\n", prefix, strerror(errno));
    else fprintf(stderr, "%s\n", strerror(errno));
}

static int scan_text(const char *text, const char *fmt, __builtin_va_list source) {
    __builtin_va_list args;
    __builtin_va_copy(args, source);
    int assigned = 0;
    while (*fmt) {
        if (isspace((unsigned char)*fmt)) {
            while (isspace((unsigned char)*fmt)) fmt++;
            while (isspace((unsigned char)*text)) text++;
            continue;
        }
        if (*fmt != '%') { if (*text++ != *fmt++) break; continue; }
        fmt++;
        if (*fmt == '%') { if (*text++ != *fmt++) break; continue; }
        int suppress = 0;
        if (*fmt == '*') { suppress = 1; fmt++; }
        int width = 0;
        while (isdigit((unsigned char)*fmt)) width = width * 10 + (*fmt++ - '0');
        int length = 0;
        if (*fmt == 'l') { length = 1; fmt++; if (*fmt == 'l') { length = 2; fmt++; } }
        else if (*fmt == 'z') { length = 1; fmt++; }
        char spec = *fmt++;
        if (spec != 'c' && spec != 'n') while (isspace((unsigned char)*text)) text++;
        if (spec == 's') {
            char *out = suppress ? NULL : __builtin_va_arg(args, char *);
            int n = 0;
            while (*text && !isspace((unsigned char)*text) && (!width || n < width)) {
                if (out) out[n] = *text;
                n++; text++;
            }
            if (!n) break;
            if (out) { out[n] = 0; assigned++; }
        } else if (spec == 'c') {
            if (!*text) break;
            if (!suppress) { *__builtin_va_arg(args, char *) = *text; assigned++; }
            text++;
        } else if (spec == 'd' || spec == 'i' || spec == 'u' || spec == 'x' || spec == 'X') {
            char *end;
            int base = (spec == 'x' || spec == 'X') ? 16 : (spec == 'i' ? 0 : 10);
            unsigned long long value = strtoull(text, &end, base);
            if (end == text) break;
            text = end;
            if (!suppress) {
                if (spec == 'd' || spec == 'i') {
                    if (length == 2) *__builtin_va_arg(args, long long *) = (long long)value;
                    else if (length) *__builtin_va_arg(args, long *) = (long)value;
                    else *__builtin_va_arg(args, int *) = (int)value;
                } else {
                    if (length == 2) *__builtin_va_arg(args, unsigned long long *) = value;
                    else if (length) *__builtin_va_arg(args, unsigned long *) = (unsigned long)value;
                    else *__builtin_va_arg(args, unsigned int *) = (unsigned int)value;
                }
                assigned++;
            }
        } else break;
    }
    __builtin_va_end(args);
    return assigned;
}

int sscanf(const char *text, const char *fmt, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    int result = scan_text(text, fmt, args);
    __builtin_va_end(args);
    return result;
}

int fscanf(FILE *stream, const char *fmt, ...) {
    char line[2048];
    if (!fgets(line, sizeof(line), stream)) return EOF;
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    int result = scan_text(line, fmt, args);
    __builtin_va_end(args);
    return result;
}

/* ================================================================
 *  Memory allocation — reusable blocks backed by an sbrk syscall
 * ================================================================ */

#define HEAP_ALIGN 8u
#define HEAP_CHUNK (64u * 1024u)
#define HEAP_MAGIC 0x42555A5Au

struct heap_block {
    size_t size;
    struct heap_block *next;
    struct heap_block *prev;
    uint32_t magic;
    uint32_t is_free;
    uint32_t reserved;
};

static struct heap_block *heap_head;
static struct heap_block *heap_tail;
static struct heap_block *heap_rover;
static volatile int heap_lock;

static void heap_acquire(void) {
    while (__sync_lock_test_and_set(&heap_lock, 1))
        yield();
}

void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *)) {
    size_t low = 0;
    size_t high = count;
    const unsigned char *bytes = base;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        const void *item = bytes + middle * size;
        int order = compare(key, item);
        if (order < 0)
            high = middle;
        else if (order > 0)
            low = middle + 1;
        else
            return (void *)item;
    }
    return 0;
}

void qsort(void *base, size_t count, size_t size,
           int (*compare)(const void *, const void *)) {
    unsigned char *bytes = (unsigned char *)base;
    for (size_t i = 1; i < count; i++) {
        size_t j = i;
        while (j > 0 && compare(bytes + (j - 1) * size,
                                bytes + j * size) > 0) {
            for (size_t k = 0; k < size; k++) {
                unsigned char temp = bytes[(j - 1) * size + k];
                bytes[(j - 1) * size + k] = bytes[j * size + k];
                bytes[j * size + k] = temp;
            }
            j--;
        }
    }
}

void abort(void) {
    exit(134);
}

int tolower(int c) {
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

int toupper(int c) {
    return c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c;
}

int isalpha(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

int isdigit(int c) {
    return c >= '0' && c <= '9';
}

int isalnum(int c) {
    return isalpha(c) || isdigit(c);
}

int isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\f' || c == '\v';
}

int isxdigit(int c) {
    return isdigit(c) || (c >= 'A' && c <= 'F') ||
           (c >= 'a' && c <= 'f');
}

int isascii(int c) {
    return (unsigned)c < 128u;
}

static unsigned long long udivmod64(unsigned long long numerator,
                                    unsigned long long denominator,
                                    unsigned long long *remainder) {
    if (denominator == 0)
        abort();
    unsigned long long quotient = 0;
    unsigned long long rest = 0;
    for (int bit = 63; bit >= 0; bit--) {
        rest = (rest << 1) | ((numerator >> bit) & 1u);
        if (rest >= denominator) {
            rest -= denominator;
            quotient |= 1ull << bit;
        }
    }
    if (remainder)
        *remainder = rest;
    return quotient;
}

unsigned long long __udivdi3(unsigned long long numerator,
                             unsigned long long denominator) {
    return udivmod64(numerator, denominator, 0);
}

unsigned long long __umoddi3(unsigned long long numerator,
                             unsigned long long denominator) {
    unsigned long long remainder;
    (void)udivmod64(numerator, denominator, &remainder);
    return remainder;
}

long long __divdi3(long long numerator, long long denominator) {
    int negative = (numerator < 0) != (denominator < 0);
    unsigned long long left = numerator < 0 ?
        (unsigned long long)(-(numerator + 1)) + 1u :
        (unsigned long long)numerator;
    unsigned long long right = denominator < 0 ?
        (unsigned long long)(-(denominator + 1)) + 1u :
        (unsigned long long)denominator;
    unsigned long long value = udivmod64(left, right, 0);
    return negative ? -(long long)value : (long long)value;
}

long long __moddi3(long long numerator, long long denominator) {
    int negative = numerator < 0;
    unsigned long long left = numerator < 0 ?
        (unsigned long long)(-(numerator + 1)) + 1u :
        (unsigned long long)numerator;
    unsigned long long right = denominator < 0 ?
        (unsigned long long)(-(denominator + 1)) + 1u :
        (unsigned long long)denominator;
    unsigned long long remainder;
    (void)udivmod64(left, right, &remainder);
    return negative ? -(long long)remainder : (long long)remainder;
}

static void heap_release(void) {
    __sync_lock_release(&heap_lock);
}

static size_t heap_align(size_t size) {
    if (size > (size_t)-1 - (HEAP_ALIGN - 1u))
        return 0;
    return (size + HEAP_ALIGN - 1u) & ~(HEAP_ALIGN - 1u);
}

static void heap_split(struct heap_block *block, size_t size) {
    if (block->size < size + sizeof(struct heap_block) + HEAP_ALIGN)
        return;
    struct heap_block *tail = (struct heap_block *)((uint8_t *)(block + 1) + size);
    tail->size = block->size - size - sizeof(struct heap_block);
    tail->next = block->next;
    tail->prev = block;
    tail->magic = HEAP_MAGIC;
    tail->is_free = 1;
    tail->reserved = 0;
    if (tail->next)
        tail->next->prev = tail;
    else
        heap_tail = tail;
    block->size = size;
    block->next = tail;
}

static void heap_merge_next(struct heap_block *block) {
    while (block->next && block->next->is_free && block->next->magic == HEAP_MAGIC) {
        struct heap_block *victim = block->next;
        if (heap_rover == victim)
            heap_rover = block;
        block->size += sizeof(struct heap_block) + victim->size;
        block->next = victim->next;
        if (block->next)
            block->next->prev = block;
        else
            heap_tail = block;
    }
}

static struct heap_block *heap_grow(size_t size) {
    if (size > (size_t)-1 - sizeof(struct heap_block))
        return (void *)0;
    size_t total = size + sizeof(struct heap_block);
    if (total < HEAP_CHUNK)
        total = HEAP_CHUNK;
    if (total > 0x7FFFFFFFu)
        return (void *)0;
    int old_break = syscall1(SYS_SBRK, (int)total);
    if (old_break < 0)
        return (void *)0;

    struct heap_block *block = (struct heap_block *)(uintptr_t)(uint32_t)old_break;
    block->size = total - sizeof(struct heap_block);
    block->next = 0;
    block->prev = heap_tail;
    block->magic = HEAP_MAGIC;
    block->is_free = 1;
    block->reserved = 0;
    if (!heap_head) {
        heap_head = block;
        heap_tail = block;
        heap_rover = block;
    } else {
        heap_tail->next = block;
        if (heap_tail->is_free) {
            block = heap_tail;
            heap_merge_next(block);
        } else {
            heap_tail = block;
        }
    }
    return block;
}

static void *heap_malloc_unlocked(size_t size) {
    size = heap_align(size);
    if (!size)
        return (void *)0;
    struct heap_block *start = heap_rover ? heap_rover : heap_head;
    struct heap_block *block = start;
    if (block) {
        do {
            if (block->is_free && block->size >= size)
                break;
            block = block->next ? block->next : heap_head;
        } while (block != start);
        if (!block->is_free || block->size < size)
            block = 0;
    }
    if (!block)
        block = heap_grow(size);
    if (!block)
        return (void *)0;
    heap_split(block, size);
    block->is_free = 0;
    heap_rover = block->next ? block->next : heap_head;
    return block + 1;
}

void *malloc(size_t size) {
    if (!size)
        return (void *)0;
    heap_acquire();
    void *ptr = heap_malloc_unlocked(size);
    heap_release();
    return ptr;
}

void free(void *ptr) {
    if (!ptr)
        return;
    heap_acquire();
    struct heap_block *block = ((struct heap_block *)ptr) - 1;
    if (block->magic == HEAP_MAGIC) {
        block->is_free = 1;
        heap_merge_next(block);
        struct heap_block *prev = block->prev;
        if (prev && prev->is_free)
            heap_merge_next(prev);
        heap_rover = prev && prev->is_free ? prev : block;
    }
    heap_release();
}

void *calloc(size_t count, size_t size) {
    if (count && size > (size_t)-1 / count)
        return (void *)0;
    size_t total = count * size;
    void *ptr = malloc(total);
    if (ptr)
        memset(ptr, 0, total);
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr)
        return malloc(size);
    if (!size) {
        free(ptr);
        return (void *)0;
    }
    size = heap_align(size);
    if (!size)
        return (void *)0;

    heap_acquire();
    struct heap_block *block = ((struct heap_block *)ptr) - 1;
    if (block->magic != HEAP_MAGIC) {
        heap_release();
        return (void *)0;
    }
    if (block->size >= size) {
        heap_split(block, size);
        heap_release();
        return ptr;
    }
    if (block->next && block->next->is_free) {
        heap_merge_next(block);
        if (block->size >= size) {
            heap_split(block, size);
            block->is_free = 0;
            heap_release();
            return ptr;
        }
    }
    size_t old_size = block->size;
    void *replacement = heap_malloc_unlocked(size);
    if (replacement) {
        memcpy(replacement, ptr, old_size);
        block->is_free = 1;
        heap_merge_next(block);
    }
    heap_release();
    return replacement;
}

char *strerror(int error) {
    switch (error) {
    case 0: return "Success";
    case 2: return "No such file or directory";
    case 5: return "I/O error";
    case 12: return "Out of memory";
    case 13: return "Permission denied";
    case 22: return "Invalid argument";
    case 36: return "File name too long";
    case 84: return "Invalid byte sequence";
    default: return "BuzzOS error";
    }
}

char *getenv(const char *name) {
    if (!strcmp(name, "HOME")) return "/";
    if (!strcmp(name, "NETSURFRES")) return "/res/netsurf";
    if (!strcmp(name, "LANG") || !strcmp(name, "LC_ALL") ||
        !strcmp(name, "LC_MESSAGES")) return "C";
    return NULL;
}

char *realpath(const char *path, char *resolved) {
    if (!path || !resolved) return NULL;
    if (path[0] == '/') {
        strncpy(resolved, path, 255);
        resolved[255] = 0;
        return resolved;
    }
    char cwd[256];
    if (!getcwd(cwd, sizeof(cwd))) return NULL;
    snprintf(resolved, 256, "%s%s%s", cwd,
             cwd[strlen(cwd) - 1] == '/' ? "" : "/", path);
    return resolved;
}

int access(const char *path, int mode) {
    (void)mode;
    struct stat status;
    return stat(path, &status);
}

struct buzz_dir {
    int fd;
    int used;
    int count;
    struct dirent entries[8];
};

DIR *opendir(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    DIR *directory = malloc(sizeof(*directory));
    if (!directory) { close(fd); return NULL; }
    directory->fd = fd; directory->used = 0; directory->count = 0;
    return directory;
}

struct dirent *readdir(DIR *directory) {
    if (!directory) return NULL;
    if (directory->used >= directory->count) {
        int bytes = getdents(directory->fd, directory->entries,
                             sizeof(directory->entries));
        if (bytes <= 0) return NULL;
        directory->count = bytes / (int)sizeof(struct dirent);
        directory->used = 0;
    }
    return &directory->entries[directory->used++];
}

int closedir(DIR *directory) {
    if (!directory) return -1;
    int result = close(directory->fd);
    free(directory);
    return result;
}
int dirfd(DIR *directory) { return directory ? directory->fd : -1; }
int fstatat(int directory_fd, const char *path, struct stat *status, int flags) {
    (void)directory_fd; (void)flags;
    return stat(path, status);
}
int unlinkat(int directory_fd, const char *path, int flags) {
    (void)directory_fd; (void)flags;
    return unlink(path);
}
int ftruncate(int fd, off_t length) {
    (void)fd; (void)length;
    return 0;
}

void *mmap(void *address, size_t length, int protection, int flags,
           int fd, long offset) {
    (void)address; (void)protection; (void)flags;
    void *memory = malloc(length ? length : 1);
    if (!memory) return MAP_FAILED;
    int saved = lseek(fd, 0, SEEK_CUR);
    if (lseek(fd, (int)offset, SEEK_SET) < 0 ||
        read(fd, memory, length) < 0) {
        free(memory);
        return MAP_FAILED;
    }
    if (saved >= 0) lseek(fd, saved, SEEK_SET);
    return memory;
}
int munmap(void *address, size_t length) { (void)length; free(address); return 0; }

int select(int count, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout) {
    (void)count; (void)readfds; (void)writefds; (void)exceptfds;
    if (timeout) {
        unsigned int ms = (unsigned int)timeout->tv_sec * 1000u +
                          (unsigned int)timeout->tv_usec / 1000u;
        if (ms) sleep_ms(ms);
    } else {
        sleep_ms(10);
    }
    return 0;
}

sighandler_t signal(int number, sighandler_t handler) {
    (void)number; (void)handler;
    return SIG_IGN;
}
char *setlocale(int category, const char *locale) {
    (void)category; (void)locale;
    return "C";
}
int uname(struct utsname *name) {
    if (!name) return -1;
    strcpy(name->sysname, "BuzzOS"); strcpy(name->nodename, "buzzos");
    strcpy(name->release, "0.1"); strcpy(name->version, "NetSurf port");
    strcpy(name->machine, "i386");
    return 0;
}

iconv_t iconv_open(const char *to_encoding, const char *from_encoding) {
    (void)to_encoding; (void)from_encoding;
    return (iconv_t)1;
}
size_t iconv(iconv_t descriptor, char **input, size_t *input_left,
             char **output, size_t *output_left) {
    (void)descriptor;
    size_t count = *input_left < *output_left ? *input_left : *output_left;
    memcpy(*output, *input, count);
    *input += count; *output += count;
    *input_left -= count; *output_left -= count;
    return *input_left ? (size_t)-1 : 0;
}
int iconv_close(iconv_t descriptor) { (void)descriptor; return 0; }

static int is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}
static const int month_days[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
static struct tm shared_tm;

struct tm *gmtime(const time_t *value) {
    if (!value) return NULL;
    long seconds = *value;
    long days = seconds / 86400;
    long day_seconds = seconds % 86400;
    if (day_seconds < 0) { day_seconds += 86400; days--; }
    shared_tm.tm_hour = (int)(day_seconds / 3600);
    shared_tm.tm_min = (int)((day_seconds / 60) % 60);
    shared_tm.tm_sec = (int)(day_seconds % 60);
    shared_tm.tm_wday = (int)((days + 4) % 7);
    if (shared_tm.tm_wday < 0) shared_tm.tm_wday += 7;
    int year = 1970;
    while (days >= (is_leap_year(year) ? 366 : 365))
        days -= is_leap_year(year++) ? 366 : 365;
    while (days < 0) { year--; days += is_leap_year(year) ? 366 : 365; }
    shared_tm.tm_year = year - 1900;
    shared_tm.tm_yday = (int)days;
    int month = 0;
    while (month < 11) {
        int length = month_days[month] + (month == 1 && is_leap_year(year));
        if (days < length) break;
        days -= length; month++;
    }
    shared_tm.tm_mon = month; shared_tm.tm_mday = (int)days + 1;
    shared_tm.tm_isdst = 0;
    return &shared_tm;
}
struct tm *localtime(const time_t *value) { return gmtime(value); }
time_t mktime(struct tm *value) {
    if (!value) return (time_t)-1;
    long days = 0;
    int year = value->tm_year + 1900;
    for (int y = 1970; y < year; y++) days += is_leap_year(y) ? 366 : 365;
    for (int month = 0; month < value->tm_mon; month++)
        days += month_days[month] + (month == 1 && is_leap_year(year));
    days += value->tm_mday - 1;
    return (time_t)(days * 86400 + value->tm_hour * 3600 +
                    value->tm_min * 60 + value->tm_sec);
}
size_t strftime(char *buffer, size_t size, const char *format,
                const struct tm *value) {
    static const char *weekdays[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    int pos = 0;
    while (*format && pos + 1 < (int)size) {
        if (*format != '%') { buffer[pos++] = *format++; continue; }
        format++;
        char temp[32]; temp[0] = 0;
        switch (*format++) {
        case '%': strcpy(temp, "%"); break;
        case 'a': strcpy(temp, weekdays[value->tm_wday]); break;
        case 'b': strcpy(temp, months[value->tm_mon]); break;
        case 'd': snprintf(temp, sizeof(temp), "%02d", value->tm_mday); break;
        case 'H': snprintf(temp, sizeof(temp), "%02d", value->tm_hour); break;
        case 'M': snprintf(temp, sizeof(temp), "%02d", value->tm_min); break;
        case 'S': snprintf(temp, sizeof(temp), "%02d", value->tm_sec); break;
        case 'Y': snprintf(temp, sizeof(temp), "%04d", value->tm_year + 1900); break;
        case 's': snprintf(temp, sizeof(temp), "%d", (int)mktime((struct tm *)value)); break;
        default: break;
        }
        for (int i = 0; temp[i] && pos + 1 < (int)size; i++) buffer[pos++] = temp[i];
    }
    if (size) buffer[pos] = 0;
    return (size_t)pos;
}
char *strptime(const char *text, const char *format, struct tm *value) {
    if (!strcmp(format, "%s")) {
        time_t seconds = (time_t)strtol(text, (char **)&text, 10);
        struct tm *parsed = gmtime(&seconds);
        if (parsed) *value = *parsed;
        return (char *)text;
    }
    return NULL;
}
double difftime(time_t end, time_t beginning) { return (double)(end - beginning); }

/* ================================================================
 *  Math functions (x87 FPU instructions)
 * ================================================================ */

double sin(double x) {
    double result;
    __asm__ volatile("fldl %1; fsin; fstpl %0" : "=m"(result) : "m"(x));
    return result;
}

double cos(double x) {
    double result;
    __asm__ volatile("fldl %1; fcos; fstpl %0" : "=m"(result) : "m"(x));
    return result;
}

double sqrt(double x) {
    double result;
    __asm__ volatile("fldl %1; fsqrt; fstpl %0" : "=m"(result) : "m"(x));
    return result;
}

double fabs(double x) {
    double result;
    __asm__ volatile("fldl %1; fabs; fstpl %0" : "=m"(result) : "m"(x));
    return result;
}

double tan(double x) {
    double cosine = cos(x);
    return cosine == 0.0 ? 0.0 : sin(x) / cosine;
}

double floor(double x) {
    long value = (long)x;
    if ((double)value > x) value--;
    return (double)value;
}

double ceil(double x) {
    long value = (long)x;
    if ((double)value < x) value++;
    return (double)value;
}

float ceilf(float x) { return (float)ceil((double)x); }
double round(double x) { return x < 0.0 ? ceil(x - 0.5) : floor(x + 0.5); }
double fmod(double x, double divisor) {
    if (divisor == 0.0) return 0.0;
    return x - (double)((long)(x / divisor)) * divisor;
}
double pow(double x, double exponent) {
    long whole = (long)exponent;
    if ((double)whole != exponent) return 0.0;
    int negative = whole < 0;
    if (negative) whole = -whole;
    double result = 1.0;
    while (whole) {
        if (whole & 1) result *= x;
        x *= x;
        whole >>= 1;
    }
    return negative && result != 0.0 ? 1.0 / result : result;
}
