#ifndef BUZZOS_SYSCALL_INTERNAL_H
#define BUZZOS_SYSCALL_INTERNAL_H

#include <stdint.h>
#include "syscall.h"
#include "user_bounds.h"

typedef intptr_t (*syscall_handler_fn)(uintptr_t, uintptr_t, uintptr_t,
                                       uintptr_t, uintptr_t);

int user_range_ok(uintptr_t ptr, size_t len);
int user_range_writable(uintptr_t ptr, size_t len);
int user_string_ok(const char *s);
int copy_from_user(void *dst, uintptr_t src, size_t len);
int copy_to_user(uintptr_t dst, const void *src, size_t len);
int copy_string_from_user(char *dst, size_t capacity, uintptr_t src);

intptr_t sys_open_console_aware(uintptr_t path_arg, uintptr_t flags, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_close(uintptr_t fd, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_read(uintptr_t fd, uintptr_t buf, uintptr_t count, uintptr_t d, uintptr_t e);
intptr_t sys_write(uintptr_t fd, uintptr_t buf, uintptr_t count, uintptr_t d, uintptr_t e);
intptr_t sys_dup(uintptr_t fd, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_dup2(uintptr_t oldfd, uintptr_t newfd, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_stat(uintptr_t path, uintptr_t st, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_getdents(uintptr_t fd, uintptr_t ents, uintptr_t count, uintptr_t d, uintptr_t e);
intptr_t sys_mkdir(uintptr_t path, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_unlink(uintptr_t path, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_create(uintptr_t path, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_lseek(uintptr_t fd, uintptr_t offset, uintptr_t whence, uintptr_t d, uintptr_t e);
intptr_t sys_rmdir(uintptr_t path, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_rename(uintptr_t old_path, uintptr_t new_path, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_fsstat(uintptr_t info_arg, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);

intptr_t sys_exit(uintptr_t code, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_spawn_proc(uintptr_t path_arg, uintptr_t flags, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_spawn_proc_args(uintptr_t path_arg, uintptr_t argv_arg, uintptr_t argc_arg,
                        uintptr_t flags, uintptr_t e);
intptr_t sys_ps(uintptr_t buf, uintptr_t size, uintptr_t show_dead, uintptr_t d, uintptr_t e);
intptr_t sys_reboot(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_spawn(uintptr_t func_addr, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_yield(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_join(uintptr_t tid_arg, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_sleep(uintptr_t ms, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_kill(uintptr_t pid, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_getpid(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_gettid(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_chdir(uintptr_t path, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_getcwd(uintptr_t buf, uintptr_t size, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_waitpid(uintptr_t pid, uintptr_t status, uintptr_t options, uintptr_t d, uintptr_t e);
intptr_t sys_sbrk(uintptr_t increment, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_monotonic_ms(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_realtime(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_shm_create(uintptr_t size, uintptr_t out_arg, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_shm_map(uintptr_t token, uintptr_t out_arg, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_shm_unmap(uintptr_t token, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_audio_write(uintptr_t data_arg, uintptr_t size, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_audio_config(uintptr_t rate, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_audio_queued(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_audio_flush(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);

intptr_t sys_socket(uintptr_t domain, uintptr_t type, uintptr_t protocol, uintptr_t d, uintptr_t e);
intptr_t sys_connect(uintptr_t sd_arg, uintptr_t addr_arg, uintptr_t addrlen, uintptr_t d, uintptr_t e);
intptr_t sys_send(uintptr_t sd_arg, uintptr_t buf, uintptr_t len, uintptr_t flags, uintptr_t e);
intptr_t sys_recv(uintptr_t sd_arg, uintptr_t buf, uintptr_t len, uintptr_t flags, uintptr_t e);
intptr_t sys_bind(uintptr_t sd_arg, uintptr_t addr_arg, uintptr_t addrlen, uintptr_t d, uintptr_t e);
intptr_t sys_sendto(uintptr_t sd_arg, uintptr_t buf, uintptr_t len, uintptr_t addr_arg, uintptr_t addrlen);
intptr_t sys_recvfrom(uintptr_t sd_arg, uintptr_t buf, uintptr_t len, uintptr_t addr_arg, uintptr_t addrlen);
intptr_t sys_closesocket(uintptr_t sd_arg, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_dns_resolve(uintptr_t host_arg, uintptr_t ip_out_arg, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_netinfo(uintptr_t mac_arg, uintptr_t ip_arg, uintptr_t c, uintptr_t d, uintptr_t e);
void sys_net_cleanup_owner(int owner);

intptr_t sys_pipe(uintptr_t fds_arg, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_futex_wait(uintptr_t addr_arg, uintptr_t expected, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_futex_wait_timeout(uintptr_t addr_arg, uintptr_t expected, uintptr_t timeout_ms, uintptr_t d, uintptr_t e);
intptr_t sys_futex_wake(uintptr_t addr_arg, uintptr_t count, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_gfx_clear(uintptr_t color, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_gfx_putpixel(uintptr_t x, uintptr_t y, uintptr_t color, uintptr_t d, uintptr_t e);
intptr_t sys_gfx_fill_rect(uintptr_t x, uintptr_t y, uintptr_t w, uintptr_t h, uintptr_t color);
intptr_t sys_gfx_text(uintptr_t x, uintptr_t y, uintptr_t s_arg, uintptr_t fg, uintptr_t bg);
intptr_t sys_fb_blit(uintptr_t x, uintptr_t y, uintptr_t w, uintptr_t h, uintptr_t pixels_arg);
intptr_t sys_fb_blit_stride(uintptr_t x, uintptr_t y, uintptr_t packed_wh,
                       uintptr_t pixels_arg, uintptr_t stride);
intptr_t sys_mouse_get(uintptr_t out_arg, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_gfx_info(uintptr_t out_arg, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_font_glyph(uintptr_t codepoint, uintptr_t out_arg, uintptr_t cap,
                   uintptr_t d, uintptr_t e);
intptr_t sys_gfx_acquire(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_gfx_release(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e);
intptr_t sys_gfx_set_mode(uintptr_t width, uintptr_t height, uintptr_t c,
                     uintptr_t d, uintptr_t e);
intptr_t sys_gfx_map_surface(uintptr_t out_arg, uintptr_t b, uintptr_t c,
                        uintptr_t d, uintptr_t e);
intptr_t sys_gfx_present(uintptr_t x, uintptr_t y, uintptr_t w, uintptr_t h,
                    uintptr_t e);
intptr_t sys_gfx_cursor_define(uintptr_t pixels_arg, uintptr_t packed_wh,
                          uintptr_t packed_hot, uintptr_t packed_xy,
                          uintptr_t e);
intptr_t sys_gfx_cursor_move(uintptr_t x, uintptr_t y, uintptr_t visible,
                        uintptr_t d, uintptr_t e);

intptr_t sys_gpu3d_info(uintptr_t out_arg, uintptr_t b, uintptr_t c, uintptr_t d,
                   uintptr_t e);
intptr_t sys_gpu3d_resource_create(uintptr_t io_arg, uintptr_t b, uintptr_t c,
                              uintptr_t d, uintptr_t e);
intptr_t sys_gpu3d_resource_destroy(uintptr_t id, uintptr_t b, uintptr_t c,
                               uintptr_t d, uintptr_t e);
intptr_t sys_gpu3d_import_shm(uintptr_t io_arg, uintptr_t b, uintptr_t c,
                         uintptr_t d, uintptr_t e);
intptr_t sys_gui_event_sequence(uintptr_t a, uintptr_t b, uintptr_t c,
                           uintptr_t d, uintptr_t e);
intptr_t sys_gui_event_wait(uintptr_t expected, uintptr_t timeout_ms, uintptr_t c,
                       uintptr_t d, uintptr_t e);
intptr_t sys_gui_event_signal(uintptr_t a, uintptr_t b, uintptr_t c,
                         uintptr_t d, uintptr_t e);
intptr_t sys_gpu3d_upload(uintptr_t id, uintptr_t packed_xy, uintptr_t packed_wh,
                     uintptr_t d, uintptr_t e);
intptr_t sys_gpu3d_submit(uintptr_t cmds_arg, uintptr_t dwords, uintptr_t c,
                     uintptr_t d, uintptr_t e);
intptr_t sys_gpu3d_present(uintptr_t x, uintptr_t y, uintptr_t w, uintptr_t h,
                      uintptr_t e);
intptr_t sys_gpu3d_scanout(uintptr_t enable, uintptr_t b, uintptr_t c, uintptr_t d,
                      uintptr_t e);

#endif
