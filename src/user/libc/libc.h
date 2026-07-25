#ifndef BUZZOS_LIBC_H
#define BUZZOS_LIBC_H

/* BuzzOS user-space mini libc.
 * Provides syscall wrappers, basic I/O, and string utilities so user
 * programs can be written like normal C without inline assembly. */

#include <stddef.h>
#include <stdint.h>

#define S_IFMT  0170000u
#define S_IFCHR 0020000u
#define S_IFDIR 0040000u
#define S_IFREG 0100000u

#define DT_UNKNOWN 0u
#define DT_CHR     2u
#define DT_DIR     4u
#define DT_REG     8u

#define O_RDONLY 0x0000u
#define O_WRONLY 0x0001u
#define O_RDWR   0x0002u
#define O_CREAT  0x0100u
#define O_TRUNC  0x0200u
#define O_APPEND 0x0400u

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define SPAWN_FLAG_SILENT      1
#define SPAWN_FLAG_INHERIT_FDS 2
#define SPAWN_FLAG_INHERIT_STDIO 4
#define SPAWN_FLAG_SERIAL_STDIO 8
#define WNOHANG 1

#define AF_INET 2
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOCK_RAW 3
#define IPPROTO_ICMP 1
#define IPPROTO_UDP 17
#define INADDR_BROADCAST 0xFFFFFFFFu

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
};

struct stat {
    uint32_t st_mode;
    uint32_t st_size;
    uint32_t st_type;
    int32_t st_mtime;
};

struct fs_info {
    uint32_t magic;
    uint32_t inode_count;
    uint32_t used_inodes;
    uint32_t dir_count;
    uint32_t file_count;
    uint32_t block_count;
    uint32_t used_blocks;
    uint32_t free_blocks;
    uint32_t data_lba;
    uint32_t max_file_size;
};

struct dirent {
    uint32_t d_type;
    uint32_t d_size;
    char d_name[24];
};

struct mouse_state {
    int x;
    int y;
    int buttons;
    int dx;
    int dy;
    uint32_t seq;
    int wheel;
    uint32_t wheel_seq;
};

struct gfx_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
};
struct shm_mapping {
    uint32_t token;
    void *address;
    uint32_t size;
};

#define FONT_GLYPH_HEIGHT 28
#define FONT_GLYPH_MAX_WIDTH 30
#define FONT_GLYPH_STRIDE 4
#define FONT_GLYPH_BYTES (FONT_GLYPH_HEIGHT * FONT_GLYPH_STRIDE)

/* --- Syscalls --- */
void exit(int code) __attribute__((noreturn));
int  atexit(void (*function)(void));
int  open(const char *path, int flags, ...);
int  close(int fd);
int  dup(int fd);
int  dup2(int oldfd, int newfd);
int  stat(const char *path, struct stat *st);
int  fsstat(struct fs_info *info);
int  getdents(int fd, struct dirent *ents, size_t count);
int  spawn_process(const char *path, int flags);
int  spawn_process_args(const char *path, char *const argv[], int argc, int flags);
int  ps(char *buf, size_t size, int show_dead);
void reboot(void) __attribute__((noreturn));
int  mkdir(const char *path);
int  buzz_mkdir_mode(const char *path, unsigned int mode);
int  unlink(const char *path);
int  rmdir(const char *path);
int  rename(const char *old_path, const char *new_path);
int  create(const char *path);
int  read(int fd, void *buf, size_t count);
int  write(int fd, const void *buf, size_t count);
int  lseek(int fd, int offset, int whence);
int  kill(int pid);
int  getpid(void);
int  gettid(void);
int  chdir(const char *path);
char *getcwd(char *buf, size_t size);
int  waitpid(int pid, int *status, int options);
int  pipe(int fds[2]);
int  futex_wait(int *addr, int expected);
int  futex_wait_timeout(int *addr, int expected, unsigned int timeout_ms);
int  futex_wake(int *addr, int count);
int  shm_create(size_t size, struct shm_mapping *mapping);
int  shm_map(uint32_t token, struct shm_mapping *mapping);
int  shm_unmap(uint32_t token);
/* Unsigned 8-bit mono PCM. Select 11025, 22050 or 44100 Hz before writing. */
int  audio_config(unsigned int sample_rate);
int  audio_config_latency(unsigned int sample_rate, unsigned int latency_ms);
int  audio_write(const uint8_t *samples, size_t count);
int  audio_queued(void);
int  audio_flush(void);
int  socket(int domain, int type, int protocol);
int  bind(int sd, const struct sockaddr_in *addr, size_t addrlen);
int  connect(int sd, const struct sockaddr_in *addr, size_t addrlen);
int  send(int sd, const void *buf, size_t len, int flags);
int  recv(int sd, void *buf, size_t len, int flags);
int  sendto(int sd, const void *buf, size_t len, int flags,
            const struct sockaddr_in *addr, size_t addrlen);
int  recvfrom(int sd, void *buf, size_t len, int flags,
              struct sockaddr_in *addr, size_t addrlen);
int  closesocket(int sd);
int  dns_resolve(const char *host, uint32_t *ip_out);
int  net_info(uint8_t mac[6], uint32_t *ip_out);
uint16_t htons(uint16_t v);
uint16_t ntohs(uint16_t v);
int  gfx_clear(int color);
int  gfx_putpixel(int x, int y, int color);
int  gfx_fill_rect(int x, int y, int w, int h, int color);
int  gfx_text(int x, int y, const char *s, int fg, int bg);
int  fb_blit(int x, int y, int w, int h, const uint8_t *pixels);
int  fb_blit_stride(int x, int y, int w, int h, const uint8_t *pixels,
                    int stride);
int  mouse_get(struct mouse_state *out);
int  gfx_info(struct gfx_info *out);
int  gfx_acquire_display(void);
int  gfx_release_display(void);
int  gfx_set_mode(int width, int height);
int  font_glyph(uint32_t codepoint, uint8_t *bits, size_t cap);
void gfx_set_origin(int x, int y);
void gfx_get_origin(int *x_out, int *y_out);

/* --- Threads --- */
typedef void (*thread_fn)(void);
int  spawn(thread_fn func);   /* create thread, returns tid */
void yield(void);             /* yield CPU */
int  join(int tid);           /* wait for thread to exit */
void sleep_ms(unsigned int ms);
uint32_t monotonic_ms(void);
int32_t time(int32_t *result);
struct timeval;
int gettimeofday(struct timeval *value, void *timezone);

/* --- Standard I/O (uses /dev/console) --- */
int  putchar(int c);
int  puts(const char *s);
int  printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  snprintf(char *buffer, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* --- String --- */
size_t strlen(const char *s);
void  *memset(void *d, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    strcasecmp(const char *a, const char *b);
int    strncasecmp(const char *a, const char *b, size_t n);
char  *strcpy(char *dst, const char *src);
char  *strncpy(char *dst, const char *src, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
char  *strstr(const char *haystack, const char *needle);
char  *strpbrk(const char *s, const char *accept);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char  *strtok(char *s, const char *delimiters);
char  *strerror(int error);
char  *strdup(const char *s);
void  *memmove(void *dst, const void *src, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
void  *memchr(const void *memory, int value, size_t size);

/* --- Utility --- */
int    atoi(const char *s);
double atof(const char *s);
int    abs(int value);
long   strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
double strtod(const char *s, char **end);
float strtof(const char *s, char **end);
void  *bsearch(const void *key, const void *base, size_t count, size_t size,
               int (*compare)(const void *, const void *));
void   qsort(void *base, size_t count, size_t size,
             int (*compare)(const void *, const void *));
void   abort(void) __attribute__((noreturn));
char  *getenv(const char *name);
char  *realpath(const char *path, char *resolved);
int    access(const char *path, int mode);
int    tolower(int c);
int    toupper(int c);
int    isalpha(int c);
int    isalnum(int c);
int    isspace(int c);
int    isdigit(int c);
int    isxdigit(int c);
int    isascii(int c);
int    isprint(int c);
int    isgraph(int c);
int    iscntrl(int c);
int    ispunct(int c);
int    isupper(int c);
int    islower(int c);
int    strcoll(const char *a, const char *b);

/* --- Memory allocation --- */
void  *malloc(size_t size);
void   free(void *ptr);
void  *calloc(size_t count, size_t size);
void  *realloc(void *ptr, size_t size);

/* --- Math (x87 FPU) --- */
double sin(double x);
double cos(double x);
double sqrt(double x);
double fabs(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double exp(double x);
double log(double x);
double log2(double x);
double log10(double x);
double frexp(double x, int *exponent);
double ldexp(double x, int exponent);
double floor(double x);
double ceil(double x);
float ceilf(float x);
double round(double x);
double fmod(double x, double divisor);
double pow(double x, double exponent);

#endif /* BUZZOS_LIBC_H */
