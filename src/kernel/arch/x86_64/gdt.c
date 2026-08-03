#include <stddef.h>
#include <stdint.h>
#include "gdt.h"
#include "serial.h"

enum { GDT_ENTRY_COUNT = 7 };

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static uint64_t gdt[GDT_ENTRY_COUNT] __attribute__((aligned(16)));
static struct gdt_ptr descriptor;

__attribute__((aligned(16), used))
tss64_t tss;

static void set_tss_descriptor(uint64_t base, uint32_t limit) {
    uint64_t low = 0;
    low |= (uint64_t)(limit & 0xFFFFu);
    low |= (base & 0xFFFFFFu) << 16;
    low |= (uint64_t)0x89u << 40;       /* available 64-bit TSS */
    low |= (uint64_t)((limit >> 16) & 0xFu) << 48;
    low |= ((base >> 24) & 0xFFu) << 56;
    gdt[5] = low;
    gdt[6] = base >> 32;
}

static void load_segments(void) {
    __asm__ volatile(
        "pushq %[code]\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw %[data], %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%ss\n"
        "xor %%eax, %%eax\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        :
        : [code] "i" ((uint64_t)GDT_SEL_KCODE64),
          [data] "i" ((uint16_t)GDT_SEL_KDATA64)
        : "rax", "memory");
}

void gdt_install(void) {
    for (size_t i = 0; i < GDT_ENTRY_COUNT; i++)
        gdt[i] = 0;

    gdt[1] = 0x00AF9A000000FFFFull; /* kernel code, long mode */
    gdt[2] = 0x00CF92000000FFFFull; /* kernel data */
    gdt[3] = 0x00CFF2000000FFFFull; /* user data */
    gdt[4] = 0x00AFFA000000FFFFull; /* user code, long mode */

    for (size_t i = 0; i < sizeof(tss); i++)
        ((uint8_t *)&tss)[i] = 0;
    tss.iomap_base = sizeof(tss);
    set_tss_descriptor((uint64_t)(uintptr_t)&tss, sizeof(tss) - 1u);

    descriptor.limit = sizeof(gdt) - 1u;
    descriptor.base = (uint64_t)(uintptr_t)gdt;
    __asm__ volatile("lgdt %0" : : "m"(descriptor) : "memory");
    load_segments();
    __asm__ volatile("ltr %%ax" : : "a"((uint16_t)GDT_SEL_TSS) : "memory");

    serial_puts("[boot] x86_64 gdt + tss ok\n");
}
