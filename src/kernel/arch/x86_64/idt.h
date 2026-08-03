#ifndef BUZZOS_X86_64_IDT_H
#define BUZZOS_X86_64_IDT_H

#include <stdint.h>

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));
typedef struct idt_entry idt_entry_t;

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));
typedef struct idt_ptr idt_ptr_t;

void idt_install(void);

#endif
