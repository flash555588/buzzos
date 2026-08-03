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
    uint32_t backend;
};
struct syscall_gfx_surface {
    uint32_t address;
    uint32_t width;
    uint32_t height;
    uint32_t stride_pixels;
    uint32_t bytes;
    uint32_t backend;
};
struct syscall_shm_mapping {
    uint32_t token;
    uint32_t address;
    uint32_t size;
};
/* virgl 3D capability, and the resource handed back by gpu3d_resource_create.
 * `address` maps the resource's own backing store, so texture uploads are
 * zero-copy: write pixels there, then upload just the damaged box. */
struct syscall_gpu3d_info {
    uint32_t available;
    uint32_t width;
    uint32_t height;
    uint32_t scanout_resource;
    uint32_t max_resources;
    uint32_t command_capacity;
};
struct syscall_gpu3d_resource {
    uint32_t target;  /* in: 0 = linear buffer, 2 = 2D texture */
    uint32_t format;  /* in: virgl format */
    uint32_t bind;    /* in: virgl bind flags */
    uint32_t width;   /* in: pixels, or bytes when target == 0 */
    uint32_t height;  /* in */
    uint32_t id;      /* out */
    uint32_t address; /* out: mapped backing store */
    uint32_t bytes;   /* out */
};
struct syscall_gpu3d_import {
    uint32_t shm_token;  /* in: object already mapped by the caller */
    uint32_t shm_offset; /* in: first texture byte inside the object */
    uint32_t target;
    uint32_t format;
    uint32_t bind;
    uint32_t width;
    uint32_t height;
    uint32_t id;         /* out */
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
       SYS_AUDIO_WRITE=60, SYS_AUDIO_CONFIG=61, SYS_FB_BLIT_STRIDE=62,
       SYS_AUDIO_QUEUED=63, SYS_AUDIO_FLUSH=64,
       SYS_GFX_ACQUIRE=65, SYS_GFX_RELEASE=66, SYS_GFX_SET_MODE=67,
       SYS_GFX_MAP_SURFACE=68, SYS_GFX_PRESENT=69,
       SYS_GPU3D_INFO=70, SYS_GPU3D_RESOURCE_CREATE=71,
       SYS_GPU3D_RESOURCE_DESTROY=72, SYS_GPU3D_UPLOAD=73,
       SYS_GPU3D_SUBMIT=74, SYS_GPU3D_PRESENT=75, SYS_GPU3D_SCANOUT=76,
       SYS_GPU3D_IMPORT_SHM=77, SYS_GUI_EVENT_SEQUENCE=78,
       SYS_GUI_EVENT_WAIT=79, SYS_GUI_EVENT_SIGNAL=80,
       SYS_GFX_CURSOR_DEFINE=81, SYS_GFX_CURSOR_MOVE=82 };

void syscall_init(void);
void syscall_handler(struct syscall_frame *frame);
void syscall_reset_process(int task_id);
void syscall_set_heap_start(int task_id, uint32_t start);
void syscall_cleanup_process(int task_id);
void syscall_release_thread(int task_id);
void syscall_process_exited(int task_id);
#endif
