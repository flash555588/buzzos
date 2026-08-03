#ifndef BUZZOS_USER_BOUNDS_H
#define BUZZOS_USER_BOUNDS_H

/* User VA layout (i386).  SHM slots must fit a full 32bpp GUIAPP surface:
 *   header 4 KiB + 1920*1200*4 ≈ 9 MiB  → 10 MiB slots.
 * Load region still has ~128 MiB for ELF BSS (desktop backbuffer ~9 MiB).
 *
 * The GPU arena above 0x30000000 maps virtio-gpu 3D resource backings
 * straight into the compositor so texture uploads are zero-copy: the
 * compositor writes pixels into the resource's own backing store and only
 * issues TRANSFER_TO_HOST_3D for the damaged box.  Slots are address-space
 * reservations only -- page tables and physical backing are allocated on
 * demand, and total backing is capped by USER_GPU_BUDGET_BYTES. */
enum {
    USER_SPACE_START       = 0x20000000u,
    USER_LOAD_START        = 0x20000000u,
    /* Display scanout map for the compositor (RGB32, up to ~16 MiB). */
    USER_DISPLAY_START     = 0x27000000u,
    USER_DISPLAY_SIZE      = 0x01000000u,
    USER_LOAD_END          = 0x27000000u,
    USER_SHM_START         = 0x28000000u,
    USER_SHM_SLOT_SIZE     = 0x00A00000u, /* 10 MiB — RGB32 max surface */
    USER_SHM_SLOTS         = 12u,
    USER_SHM_END           = 0x2F800000u, /* START + 12 * 10 MiB */
    USER_DEFAULT_STACK_TOP = 0x2FFF0000u,
    USER_MAIN_STACK_SIZE   = 0x00010000u,
    USER_THREAD_STACK_SIZE = 0x00004000u,
    USER_THREAD_STACK_SLOTS = 24u,
    USER_TRAMPOLINE_BASE   = 0x2FFFF000u,
    /* virtio-gpu 3D resource backings, mapped into the display owner. */
    USER_GPU_START         = 0x30000000u,
    USER_GPU_SLOT_SIZE     = 0x00A00000u, /* 10 MiB — full-screen RGB32 */
    USER_GPU_SLOTS         = 24u,
    USER_GPU_END           = 0x3F000000u, /* START + 24 * 10 MiB */
    /* Ceiling on physical memory handed to GPU resources at once, so a
     * runaway compositor cannot starve the 256 MiB managed pool. */
    USER_GPU_BUDGET_BYTES  = 0x04000000u, /* 64 MiB */
    USER_SPACE_END         = 0x3F000000u,
    USER_PTR_START         = 0x20000000u,
    USER_PTR_END           = 0x3F000000u,
};

#endif /* BUZZOS_USER_BOUNDS_H */
