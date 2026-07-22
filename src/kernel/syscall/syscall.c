#include "syscall.h"
#include "paging.h"
#include "serial.h"
#include "syscall_internal.h"

static int user_range_bounds_ok(uint32_t ptr, uint32_t len) {
    if (len == 0)
        return 1;
    if (ptr < USER_PTR_START || ptr >= USER_PTR_END)
        return 0;
    if (len > USER_PTR_END - ptr)
        return 0;
    return 1;
}

int user_range_ok(uint32_t ptr, uint32_t len) {
    return user_range_bounds_ok(ptr, len) &&
           paging_user_range_accessible(ptr, len, 0);
}

int user_range_writable(uint32_t ptr, uint32_t len) {
    return user_range_bounds_ok(ptr, len) &&
           paging_user_range_accessible(ptr, len, 1);
}

int user_string_ok(const char *s) {
    uint32_t ptr = (uint32_t)(uintptr_t)s;
    if (!user_range_ok(ptr, 1))
        return 0;
    for (uint32_t i = 0; i < 256; i++) {
        if (!user_range_ok(ptr + i, 1))
            return 0;
        if (s[i] == 0)
            return 1;
    }
    return 0;
}

static syscall_handler_fn syscall_table[256];

void syscall_handler(struct syscall_frame *frame) {
    uint32_t eax = frame->eax;
    uint32_t ebx = frame->ebx;
    uint32_t ecx = frame->ecx;
    uint32_t edx = frame->edx;
    uint32_t esi = frame->esi;
    uint32_t edi = frame->edi;
    (void)esi; (void)edi;

    int nr = (int)eax;
    int result = -1;
    if (nr < 256 && syscall_table[nr])
        result = syscall_table[nr](ebx, ecx, edx, esi, edi);

    frame->eax = (uint32_t)result;
}

void syscall_init(void) {
    syscall_table[SYS_EXIT]  = sys_exit;
    syscall_table[SYS_OPEN]  = sys_open_console_aware;
    syscall_table[SYS_CLOSE] = sys_close;
    syscall_table[SYS_READ]  = sys_read;
    syscall_table[SYS_WRITE] = sys_write;
    syscall_table[SYS_DUP]   = sys_dup;
    syscall_table[SYS_DUP2]  = sys_dup2;
    syscall_table[SYS_STAT]  = sys_stat;
    syscall_table[SYS_GETDENTS] = sys_getdents;
    syscall_table[SYS_SPAWN_PROC] = sys_spawn_proc;
    syscall_table[SYS_PS] = sys_ps;
    syscall_table[SYS_REBOOT] = sys_reboot;
    syscall_table[SYS_MKDIR] = sys_mkdir;
    syscall_table[SYS_UNLINK] = sys_unlink;
    syscall_table[SYS_CREATE] = sys_create;
    syscall_table[SYS_SPAWN_PROC_ARGS] = sys_spawn_proc_args;
    syscall_table[SYS_LSEEK] = sys_lseek;
    syscall_table[SYS_RMDIR] = sys_rmdir;
    syscall_table[SYS_RENAME] = sys_rename;
    syscall_table[SYS_SOCKET] = sys_socket;
    syscall_table[SYS_CONNECT] = sys_connect;
    syscall_table[SYS_SEND] = sys_send;
    syscall_table[SYS_RECV] = sys_recv;
    syscall_table[SYS_CLOSESOCKET] = sys_closesocket;
    syscall_table[SYS_DNS_RESOLVE] = sys_dns_resolve;
    syscall_table[SYS_BIND] = sys_bind;
    syscall_table[SYS_SENDTO] = sys_sendto;
    syscall_table[SYS_RECVFROM] = sys_recvfrom;
    syscall_table[SYS_NETINFO] = sys_netinfo;
    syscall_table[SYS_PIPE] = sys_pipe;
    syscall_table[SYS_FUTEX_WAIT] = sys_futex_wait;
    syscall_table[SYS_FUTEX_WAKE] = sys_futex_wake;
    syscall_table[SYS_FUTEX_WAIT_TIMEOUT] = sys_futex_wait_timeout;
    syscall_table[SYS_GFX_CLEAR] = sys_gfx_clear;
    syscall_table[SYS_GFX_PUTPIXEL] = sys_gfx_putpixel;
    syscall_table[SYS_GFX_FILL_RECT] = sys_gfx_fill_rect;
    syscall_table[SYS_GFX_TEXT] = sys_gfx_text;
    syscall_table[SYS_FB_BLIT] = sys_fb_blit;
    syscall_table[SYS_MOUSE_GET] = sys_mouse_get;
    syscall_table[SYS_FSSTAT] = sys_fsstat;
    syscall_table[SYS_GFX_INFO] = sys_gfx_info;
    syscall_table[SYS_FONT_GLYPH] = sys_font_glyph;
    syscall_table[SYS_SPAWN] = sys_spawn;
    syscall_table[SYS_YIELD] = sys_yield;
    syscall_table[SYS_JOIN]  = sys_join;
    syscall_table[SYS_SLEEP] = sys_sleep;
    syscall_table[SYS_KILL]  = sys_kill;
    syscall_table[SYS_GETPID] = sys_getpid;
    syscall_table[SYS_GETTID] = sys_gettid;
    syscall_table[SYS_CHDIR]  = sys_chdir;
    syscall_table[SYS_GETCWD] = sys_getcwd;
    syscall_table[SYS_WAITPID] = sys_waitpid;
    syscall_table[SYS_SBRK] = sys_sbrk;
    syscall_table[SYS_MONOTONIC_MS] = sys_monotonic_ms;
    syscall_table[SYS_REALTIME] = sys_realtime;
    syscall_table[SYS_SHM_CREATE] = sys_shm_create;
    syscall_table[SYS_SHM_MAP] = sys_shm_map;
    syscall_table[SYS_SHM_UNMAP] = sys_shm_unmap;
    syscall_table[SYS_AUDIO_WRITE] = sys_audio_write;
    serial_puts("[syscall] VFS-backed syscalls ready\n");
}
