#include "libc.h"

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
       SYS_GFX_INFO=52, SYS_FONT_GLYPH=53 };

void exit(int code) {
    syscall1(SYS_EXIT, code);
    __builtin_unreachable();
}

int open(const char *path, int flags) {
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
       SYS_WAITPID=15 };

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
    exit(0);
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
    while (n--) *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    while (n--) *dp++ = *sp++;
    return d;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *(unsigned char *)a - *(unsigned char *)b;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}

int atoi(const char *s) {
    int neg = 0, v = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

/* ================================================================
 *  Standard I/O
 * ================================================================ */

static int console_fd = 1;

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

int printf(const char *fmt, ...) {
    if (ensure_console() < 0) return -1;

    char out[512];
    int pos = 0;

    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            out_ch(out, &pos, (int)sizeof(out), *fmt);
            continue;
        }
        fmt++;
        switch (*fmt) {
        case 'd': case 'i': {
            int v = __builtin_va_arg(ap, int);
            if (v < 0) {
                out_ch(out, &pos, (int)sizeof(out), '-');
                v = -v;
            }
            out_uint(out, &pos, (int)sizeof(out), (unsigned)v, 10);
            break;
        }
        case 'u':
            out_uint(out, &pos, (int)sizeof(out),
                     __builtin_va_arg(ap, unsigned int), 10);
            break;
        case 'x':
            out_uint(out, &pos, (int)sizeof(out),
                     __builtin_va_arg(ap, unsigned int), 16);
            break;
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s) s = "(null)";
            out_str(out, &pos, (int)sizeof(out), s);
            break;
        }
        case 'c':
            out_ch(out, &pos, (int)sizeof(out),
                   (char)__builtin_va_arg(ap, int));
            break;
        case 'f': {
            double v = __builtin_va_arg(ap, double);
            out_double(out, &pos, (int)sizeof(out), v, 6);
            break;
        }
        case '%':
            out_ch(out, &pos, (int)sizeof(out), '%');
            break;
        default:
            out_ch(out, &pos, (int)sizeof(out), '%');
            out_ch(out, &pos, (int)sizeof(out), *fmt);
            break;
        }
    }
    __builtin_va_end(ap);

    int n = pos;
    if (n > (int)sizeof(out) - 1)
        n = (int)sizeof(out) - 1;
    out[n] = 0;
    if (n > 0)
        write(console_fd, out, (size_t)n);
    return pos;
}

/* ================================================================
 *  Memory allocation — bump allocator with 64 KiB heap
 * ================================================================ */

#define HEAP_SIZE (64 * 1024)
static uint8_t heap[HEAP_SIZE] __attribute__((aligned(16)));
static size_t heap_offset;

void *malloc(size_t size) {
    /* Align to 8 bytes */
    size = (size + 7) & ~(size_t)7;
    if (heap_offset + size > HEAP_SIZE) return (void *)0;
    void *p = &heap[heap_offset];
    heap_offset += size;
    return p;
}

void free(void *ptr) {
    /* Bump allocator doesn't free — acceptable for short-lived programs. */
    (void)ptr;
}

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
