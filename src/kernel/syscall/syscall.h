#ifndef BUZZOS_SYSCALL_H
#define BUZZOS_SYSCALL_H
#include <stddef.h>
#include <stdint.h>

#define SYSCALL_VECTOR        0x80
#define SYSCALL_VECTOR_LEGACY 0x30

struct syscall_frame {
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
};

struct syscall_gfx_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
};
struct syscall_shm_mapping {
    uint32_t token;
    uint32_t address;
    uint32_t size;
};

enum { SYS_EXIT=1, SYS_OPEN=2, SYS_CLOSE=3, SYS_READ=4, SYS_WRITE=5,
       SYS_SPAWN=6, SYS_YIELD=7, SYS_JOIN=8, SYS_SLEEP=9, SYS_KILL=10,
       SYS_GETPID=11, SYS_GETTID=12, SYS_CHDIR=13, SYS_GETCWD=14,
       SYS_WAITPID=15, SYS_DUP=16, SYS_DUP2=17,
       SYS_STAT=18, SYS_GETDENTS=19, SYS_SPAWN_PROC=20,
       SYS_PS=21, SYS_REBOOT=22, SYS_MKDIR=23, SYS_UNLINK=24,
       SYS_CREATE=25, SYS_SPAWN_PROC_ARGS=26, SYS_LSEEK=27,
       SYS_RMDIR=28, SYS_RENAME=29, SYS_SOCKET=30,
       SYS_CONNECT=31, SYS_SEND=32, SYS_RECV=33,
       SYS_CLOSESOCKET=34, SYS_DNS_RESOLVE=35, SYS_BIND=36,
       SYS_SENDTO=37, SYS_RECVFROM=38, SYS_NETINFO=39,
       SYS_PIPE=40, SYS_FUTEX_WAIT=41, SYS_FUTEX_WAKE=42,
       SYS_GFX_CLEAR=44, SYS_GFX_PUTPIXEL=45,
       SYS_GFX_FILL_RECT=46, SYS_GFX_TEXT=47, SYS_FB_BLIT=48,
       SYS_MOUSE_GET=49, SYS_FSSTAT=50, SYS_FUTEX_WAIT_TIMEOUT=51,
       SYS_GFX_INFO=52, SYS_FONT_GLYPH=53, SYS_SBRK=54,
       SYS_MONOTONIC_MS=55, SYS_REALTIME=56,
       SYS_SHM_CREATE=57, SYS_SHM_MAP=58, SYS_SHM_UNMAP=59,
       SYS_AUDIO_WRITE=60 };

void syscall_init(void);
void syscall_handler(struct syscall_frame *frame);
void syscall_reset_process(int task_id);
void syscall_set_heap_start(int task_id, uint32_t start);
void syscall_cleanup_process(int task_id);
void syscall_release_thread(int task_id);
#endif
