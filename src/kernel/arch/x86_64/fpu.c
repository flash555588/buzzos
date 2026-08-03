#include "fpu.h"

/* FXSAVE/FXRSTOR cover x87, MMX, XMM, MXCSR and the associated control
 * state. Keeping one image per task is required before user programs may
 * use SSE (and therefore AES-NI). */
static uint8_t initial_state[FPU_STATE_SIZE] __attribute__((aligned(16)));
static int fxsave_enabled;

static void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                  uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(0));
}

int fpu_init(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    (void)eax;
    (void)ebx;
    (void)ecx;

    /* CPUID.01H:EDX.FXSR/SSE/SSE2. BuzzOS requires all three so the
     * execution environment exposed to applications is consistent. */
    const uint32_t required = (1u << 24) | (1u << 25) | (1u << 26);
    if ((edx & required) != required)
        return 0;

    uint64_t cr0;
    uint64_t cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~((1ull << 2) | (1ull << 3)); /* EM=0, TS=0. */
    cr0 |= (1ull << 1) | (1ull << 5);    /* MP=1, NE=1. */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");

    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ull << 9) | (1ull << 10); /* OSFXSR, OSXMMEXCPT. */
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");

    __asm__ volatile("fninit\n\tfxsave %0" : "=m"(initial_state) : : "memory");
    fxsave_enabled = 1;
    return 1;
}

int fpu_available(void) {
    return fxsave_enabled;
}

void fpu_state_init(void *state) {
    uint8_t *dst = (uint8_t *)state;
    for (int i = 0; i < FPU_STATE_SIZE; i++)
        dst[i] = initial_state[i];
}

void fpu_state_save(void *state) {
    if (fxsave_enabled)
        __asm__ volatile("fxsave (%0)" : : "r"(state) : "memory");
}

void fpu_state_restore(const void *state) {
    if (fxsave_enabled)
        __asm__ volatile("fxrstor (%0)" : : "r"(state) : "memory");
}
