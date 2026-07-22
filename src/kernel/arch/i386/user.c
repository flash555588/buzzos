#include <stddef.h>
#include <stdint.h>
#include "gdt.h"
#include "paging.h"
#include "serial.h"
#include "task.h"
#include "user.h"

extern tss32_t tss;

enum {
    USER_MODE_EFLAGS = 0x0202,
};

#if defined(__INTELLISENSE__)
#define GNU_ASM(...)
#else
#define GNU_ASM(...) __asm__ volatile(__VA_ARGS__)
#endif

static const uint8_t trampoline_code[] = {
    0x66, 0xB8, 0x23, 0x00, /* mov ax, 0x23 */
    0x8E, 0xD8,             /* mov ds, ax */
    0x8E, 0xC0,             /* mov es, ax */
    0x8E, 0xE0,             /* mov fs, ax */
    0x8E, 0xE8,             /* mov gs, ax */
    0xFF, 0xE2,             /* jmp edx (final user entry) */
};

static void copy_bytes(uint8_t *dst, const uint8_t *src, size_t len) {
    for (size_t i = 0; i < len; i++)
        dst[i] = src[i];
}

int user_install_trampoline(void) {
    uint8_t *trampoline = (uint8_t *)USER_TRAMPOLINE_BASE;

    copy_bytes(trampoline, trampoline_code, sizeof(trampoline_code));
    return 0;
}

int user_install_trampoline_in_space(uint32_t cr3) {
    return paging_copy_to_user_space(cr3, USER_TRAMPOLINE_BASE,
                                     trampoline_code,
                                     (uint32_t)sizeof(trampoline_code));
}

void user_enter(uint32_t entry, uint32_t stack_top) {
    uint32_t kernel_esp;

    GNU_ASM("mov %%esp, %0" : "=r"(kernel_esp));
    current_task->esp0 = kernel_esp;
    tss.esp0 = kernel_esp;

    if (!current_task->console_silent) {
        serial_puts("[user] enter entry=");
        serial_puthex(entry);
        serial_puts(" stack=");
        serial_puthex(stack_top);
        serial_puts("\n");
    }

    GNU_ASM(
        "pushl %0\n"
        "pushl %1\n"
        "pushl %2\n"
        "pushl %3\n"
        "pushl %4\n"
        "iretl\n"
        :
        : "i"(GDT_SEL_UDATA32),
          "r"(stack_top),
          "i"(USER_MODE_EFLAGS),
          "i"(GDT_SEL_UCODE32),
          "r"(USER_TRAMPOLINE_BASE),
          "d"(entry)
        : "esp", "memory"
    );

    __builtin_unreachable();
}
