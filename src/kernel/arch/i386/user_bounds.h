#ifndef BUZZOS_USER_BOUNDS_H
#define BUZZOS_USER_BOUNDS_H

/* User VA layout (i386).  SHM slots must fit a full 32bpp GUIAPP surface:
 *   header 4 KiB + 1920*1200*4 ≈ 9 MiB  → 10 MiB slots.
 * Load region still has ~128 MiB for ELF BSS (desktop backbuffer ~9 MiB). */
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
    USER_SPACE_END         = 0x30000000u,
    USER_PTR_START         = 0x20000000u,
    USER_PTR_END           = 0x30000000u,
};

#endif /* BUZZOS_USER_BOUNDS_H */
