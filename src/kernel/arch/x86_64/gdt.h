#ifndef BUZZOS_X86_64_GDT_H
#define BUZZOS_X86_64_GDT_H

#include <stdint.h>

struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));
typedef struct tss64 tss64_t;

#define GDT_SEL_KCODE64 0x08
#define GDT_SEL_KDATA64 0x10
#define GDT_SEL_UDATA64 0x1B
#define GDT_SEL_UCODE64 0x23
#define GDT_SEL_TSS     0x28

extern tss64_t tss;
void gdt_install(void);

#endif
