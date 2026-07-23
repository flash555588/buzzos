#ifndef BUZZOS_SYSCALL_INTERNAL_H
#define BUZZOS_SYSCALL_INTERNAL_H

#include <stdint.h>
#include "syscall.h"
#include "user_bounds.h"

typedef int (*syscall_handler_fn)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

int user_range_ok(uint32_t ptr, uint32_t len);
int user_range_writable(uint32_t ptr, uint32_t len);
int user_string_ok(const char *s);

int sys_open_console_aware(uint32_t path_arg, uint32_t flags, uint32_t c, uint32_t d, uint32_t e);
int sys_close(uint32_t fd, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
int sys_read(uint32_t fd, uint32_t buf, uint32_t count, uint32_t d, uint32_t e);
int sys_write(uint32_t fd, uint32_t buf, uint32_t count, uint32_t d, uint32_t e);
int sys_dup(uint32_t fd, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
int sys_dup2(uint32_t oldfd, uint32_t newfd, uint32_t c, uint32_t d, uint32_t e);
int sys_stat(uint32_t path, uint32_t st, uint32_t c, uint32_t d, uint32_t e);
int sys_getdents(uint32_t fd, uint32_t ents, uint32_t count, uint32_t d, uint32_t e);
int sys_mkdir(uint32_t path, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
int sys_unlink(uint32_t path, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
int sys_create(uint32_t path, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
int sys_lseek(uint32_t fd, uint32_t offset, uint32_t whence, uint32_t d, uint32_t e);
int sys_rmdir(uint32_t path, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
int sys_rename(uint32_t old_path, uint32_t new_path, uint32_t c, uint32_t d, uint32_t e);
int sys_fsstat(uint32_t info_arg, uint32_t b, uint32_t c, uint32_t d, uint32_t e);

int sys_exit(uint32_t code, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
int sys_spawn_proc(uint32_t path_arg, uint32_t flags, uint32_t c, uint32_t d, uint32_t e);
int sys_spawn_proc_args(uint32_t path_arg, uint32_t argv_arg, uint32_t argc_arg,
                        uint32_t flags, uint32_t e);
int sys_ps(uint32_t buf, uint32_t size, uint32_t show_dead, uint32_t d, uint32_t e);
int sys_reboot(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
int sys_spawn(uint32_t func_addr, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
int sys_yield(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
int sys_join(uint32_t tid_arg, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
int sys_sleep(uint32_t ms, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
int sys_kill(uint32_t pid, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
int sys_getpid(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
int sys_gettid(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
int sys_chdir(uint32_t path, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
int sys_getcwd(uint32_t buf, uint32_t size, uint32_t c, uint32_t d, uint32_t e);
int sys_waitpid(uint32_t pid, uint32_t status, uint32_t options, uint32_t d, uint32_t e);
int sys_sbrk(uint32_t increment, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
int sys_monotonic_ms(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
int sys_realtime(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
int sys_shm_create(uint32_t size, uint32_t out_arg, uint32_t c, uint32_t d, uint32_t e);
int sys_shm_map(uint32_t token, uint32_t out_arg, uint32_t c, uint32_t d, uint32_t e);
int sys_shm_unmap(uint32_t token, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
int sys_audio_write(uint32_t data_arg, uint32_t size, uint32_t c, uint32_t d, uint32_t e);
int sys_audio_config(uint32_t rate, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
int sys_audio_queued(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
int sys_audio_flush(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e);

int sys_socket(uint32_t domain, uint32_t type, uint32_t protocol, uint32_t d, uint32_t e);
int sys_connect(uint32_t sd_arg, uint32_t addr_arg, uint32_t addrlen, uint32_t d, uint32_t e);
int sys_send(uint32_t sd_arg, uint32_t buf, uint32_t len, uint32_t flags, uint32_t e);
int sys_recv(uint32_t sd_arg, uint32_t buf, uint32_t len, uint32_t flags, uint32_t e);
int sys_bind(uint32_t sd_arg, uint32_t addr_arg, uint32_t addrlen, uint32_t d, uint32_t e);
int sys_sendto(uint32_t sd_arg, uint32_t buf, uint32_t len, uint32_t addr_arg, uint32_t addrlen);
int sys_recvfrom(uint32_t sd_arg, uint32_t buf, uint32_t len, uint32_t addr_arg, uint32_t addrlen);
int sys_closesocket(uint32_t sd_arg, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
int sys_dns_resolve(uint32_t host_arg, uint32_t ip_out_arg, uint32_t c, uint32_t d, uint32_t e);
int sys_netinfo(uint32_t mac_arg, uint32_t ip_arg, uint32_t c, uint32_t d, uint32_t e);
void sys_net_cleanup_owner(int owner);

int sys_pipe(uint32_t fds_arg, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
int sys_futex_wait(uint32_t addr_arg, uint32_t expected, uint32_t c, uint32_t d, uint32_t e);
int sys_futex_wait_timeout(uint32_t addr_arg, uint32_t expected, uint32_t timeout_ms, uint32_t d, uint32_t e);
int sys_futex_wake(uint32_t addr_arg, uint32_t count, uint32_t c, uint32_t d, uint32_t e);
int sys_gfx_clear(uint32_t color, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
int sys_gfx_putpixel(uint32_t x, uint32_t y, uint32_t color, uint32_t d, uint32_t e);
int sys_gfx_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
int sys_gfx_text(uint32_t x, uint32_t y, uint32_t s_arg, uint32_t fg, uint32_t bg);
int sys_fb_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t pixels_arg);
int sys_fb_blit_stride(uint32_t x, uint32_t y, uint32_t packed_wh,
                       uint32_t pixels_arg, uint32_t stride);
int sys_mouse_get(uint32_t out_arg, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
int sys_gfx_info(uint32_t out_arg, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
int sys_font_glyph(uint32_t codepoint, uint32_t out_arg, uint32_t cap,
                   uint32_t d, uint32_t e);
int sys_gfx_acquire(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
int sys_gfx_release(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e);

#endif
