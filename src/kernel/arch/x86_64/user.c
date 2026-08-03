#include <stddef.h>
#include <stdint.h>
#include "gdt.h"
#include "paging.h"
#include "serial.h"
#include "task.h"
#include "user.h"

extern tss64_t tss;

/* jmp rdx: the kernel enters this read-only page with the ELF64 entry point
 * in RDX.  Keeping a fixed trampoline makes process startup independent of
 * compiler-generated kernel code addresses. */
static const uint8_t trampoline_code[] = { 0xFF, 0xE2 };

int user_install_trampoline(void) {
    uint8_t *dst = (uint8_t *)(uintptr_t)USER_TRAMPOLINE_BASE;
    for (size_t i = 0; i < sizeof(trampoline_code); i++) dst[i] = trampoline_code[i];
    return 0;
}

int user_install_trampoline_in_space(uintptr_t cr3) {
    return paging_copy_to_user_space(cr3, USER_TRAMPOLINE_BASE,
                                     trampoline_code, sizeof(trampoline_code));
}

void user_enter(uintptr_t entry, uintptr_t stack_top) {
    uintptr_t kernel_stack = current_task->esp0;
    if (!kernel_stack) kernel_stack = current_task->kstack;
    tss.rsp0 = kernel_stack;

    if (!current_task->console_silent) {
        serial_puts("[user] enter64 entry=");
        serial_puthex64(entry);
        serial_puts(" stack=");
        serial_puthex64(stack_top);
        serial_puts("\n");
    }

    __asm__ volatile(
        "movw %[udata], %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "pushq %[udataq]\n"
        "pushq %[stack]\n"
        "pushq $0x202\n"
        "pushq %[ucode]\n"
        "pushq %[trampoline]\n"
        "iretq\n"
        :
        : [udata] "i" ((uint16_t)GDT_SEL_UDATA64),
          [udataq] "i" ((uint64_t)GDT_SEL_UDATA64),
          [ucode] "i" ((uint64_t)GDT_SEL_UCODE64),
          [stack] "r" (stack_top),
          [trampoline] "r" ((uintptr_t)USER_TRAMPOLINE_BASE),
          "d" (entry)
        : "rax", "memory");
    __builtin_unreachable();
}
