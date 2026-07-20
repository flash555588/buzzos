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

#define FONT_GLYPH_HEIGHT 22
#define FONT_GLYPH_MAX_WIDTH 24
#define FONT_GLYPH_STRIDE 3
#define FONT_GLYPH_BYTES (FONT_GLYPH_HEIGHT * FONT_GLYPH_STRIDE)

/* --- Syscalls --- */
void exit(int code) __attribute__((noreturn));
int  open(const char *path, int flags);
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
int  mouse_get(struct mouse_state *out);
int  gfx_info(struct gfx_info *out);
int  font_glyph(uint32_t codepoint, uint8_t *bits, size_t cap);
void gfx_set_origin(int x, int y);
void gfx_get_origin(int *x_out, int *y_out);

/* --- Threads --- */
typedef void (*thread_fn)(void);
int  spawn(thread_fn func);   /* create thread, returns tid */
void yield(void);             /* yield CPU */
int  join(int tid);           /* wait for thread to exit */
void sleep_ms(unsigned int ms);

/* --- Standard I/O (uses /dev/console) --- */
int  putchar(int c);
int  puts(const char *s);
int  printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* --- String --- */
size_t strlen(const char *s);
void  *memset(void *d, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
int    strcmp(const char *a, const char *b);
char  *strcpy(char *dst, const char *src);

/* --- Utility --- */
int    atoi(const char *s);

/* --- Memory allocation (simple bump allocator, 64 KiB heap) --- */
void  *malloc(size_t size);
void   free(void *ptr);

/* --- Math (x87 FPU) --- */
double sin(double x);
double cos(double x);
double sqrt(double x);
double fabs(double x);

#endif /* BUZZOS_LIBC_H */
