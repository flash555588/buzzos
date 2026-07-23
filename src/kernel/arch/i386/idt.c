#include <stddef.h>
#include "idt.h"
#include "io.h"
#include "serial.h"
#include "syscall.h"
#include "task.h"

enum {
    IDT_ENTRY_COUNT = 256,
    IDT_GATE_INT = 0x8E,
    IDT_GATE_INT_USER = 0xEE,
    IDT_GATE_TRAP_USER = 0xEF,
};

struct idt_gate_init {
    uint8_t vector;
    void (*handler)(void);
    uint8_t type_attr;
};

#if defined(__INTELLISENSE__)
#define GNU_ASM(...)
#else
#define GNU_ASM(...) __asm__ volatile(__VA_ARGS__)
#endif

static idt_entry_t idt[IDT_ENTRY_COUNT];
static idt_ptr_t   idt_ptr;

static void idt_set_gate(uint8_t num, uint32_t handler, uint8_t type_attr) {
    idt[num].offset_low  = (uint16_t)(handler & 0xFFFF);
    idt[num].offset_high = (uint16_t)(handler >> 16);
    idt[num].selector    = 0x08;
    idt[num].zero        = 0;
    idt[num].type_attr   = type_attr;
}

static const char *exception_names[32] = {
    "#DE", "#DB", "NMI", "#BP", "#OF", "#BR", "#UD", "#NM",
    "#DF", 0, "#TS", "#NP", "#SS", "#GP", "#PF", 0,
    "#MF", "#AC", "#MC", "#XM",
};

void exception_handler(uint32_t vector, uint32_t error, const uint32_t *frame) {
    serial_puts("\n=== EXCEPTION ===\nVector: ");
    serial_puthex(vector);
    serial_puts("\n");
    if (vector < 32 && exception_names[vector])
        serial_puts(exception_names[vector]);
    serial_puts("\nError: ");
    serial_puthex(error);
    if (frame) {
        serial_puts("\nEIP=");
        serial_puthex(frame[10]);
        serial_puts(" CS=");
        serial_puthex(frame[11]);
        serial_puts(" EFLAGS=");
        serial_puthex(frame[12]);
        serial_puts("\nESP=");
        serial_puthex(frame[13]);
        serial_puts(" SS=");
        serial_puthex(frame[14]);
        serial_puts(" EAX=");
        serial_puthex(frame[7]);
    }
    if (vector == 14) {
        uint32_t fault_addr;
        GNU_ASM("mov %%cr2, %0" : "=r"(fault_addr));
        serial_puts("\nCR2=");
        serial_puthex(fault_addr);
    }
    if (frame && (frame[11] & 3u) == 3u && current_task && current_task->id > 0) {
        serial_puts("\nTerminating faulting user task.\n");
        task_exit_process_code(vector ? -(int)vector : -1);
    }
    serial_puts("\nHalted.\n");
    for (;;) { GNU_ASM("hlt"); }
}

extern void exc_stub_0(void);
extern void exc_stub_1(void);
extern void exc_stub_2(void);
extern void exc_stub_3(void);
extern void exc_stub_4(void);
extern void exc_stub_5(void);
extern void exc_stub_6(void);
extern void exc_stub_7(void);
extern void exc_stub_8(void);
extern void exc_stub_10(void);
extern void exc_stub_11(void);
extern void exc_stub_12(void);
extern void exc_stub_13(void);
extern void exc_stub_14(void);
extern void exc_stub_15(void);
extern void exc_stub_16(void);
extern void exc_stub_17(void);
extern void exc_stub_18(void);
extern void exc_stub_19(void);
extern void exc_stub_20(void);
extern void exc_stub_21(void);
extern void exc_stub_22(void);
extern void exc_stub_23(void);
extern void exc_stub_24(void);
extern void exc_stub_25(void);
extern void exc_stub_26(void);
extern void exc_stub_27(void);
extern void exc_stub_28(void);
extern void exc_stub_29(void);
extern void exc_stub_30(void);
extern void exc_stub_31(void);
extern void (*default_stub_table[IDT_ENTRY_COUNT])(void);
extern void irq_stub_32(void);
extern void irq_stub_33(void);
extern void irq_stub_34(void);
extern void irq_stub_35(void);
extern void irq_stub_36(void);
extern void irq_stub_37(void);
extern void irq_stub_38(void);
extern void irq_stub_39(void);
extern void irq_stub_40(void);
extern void irq_stub_41(void);
extern void irq_stub_42(void);
extern void irq_stub_43(void);
extern void irq_stub_44(void);
extern void irq_stub_45(void);
extern void irq_stub_46(void);
extern void irq_stub_47(void);
extern void apic_spurious_stub(void);
extern void syscall_stub(void);

static const struct idt_gate_init early_gates[] = {
    {0,  exc_stub_0,  IDT_GATE_INT},
    {1,  exc_stub_1,  IDT_GATE_INT},
    {2,  exc_stub_2,  IDT_GATE_INT},
    {3,  exc_stub_3,  IDT_GATE_TRAP_USER},
    {4,  exc_stub_4,  IDT_GATE_INT},
    {5,  exc_stub_5,  IDT_GATE_INT},
    {6,  exc_stub_6,  IDT_GATE_INT},
    {7,  exc_stub_7,  IDT_GATE_INT},
    {8,  exc_stub_8,  IDT_GATE_INT},
    {10, exc_stub_10, IDT_GATE_INT},
    {11, exc_stub_11, IDT_GATE_INT},
    {12, exc_stub_12, IDT_GATE_INT},
    {13, exc_stub_13, IDT_GATE_INT},
    {14, exc_stub_14, IDT_GATE_INT},
    {15, exc_stub_15, IDT_GATE_INT},
    {16, exc_stub_16, IDT_GATE_INT},
    {17, exc_stub_17, IDT_GATE_INT},
    {18, exc_stub_18, IDT_GATE_INT},
    {19, exc_stub_19, IDT_GATE_INT},
    {20, exc_stub_20, IDT_GATE_INT},
    {21, exc_stub_21, IDT_GATE_INT},
    {22, exc_stub_22, IDT_GATE_INT},
    {23, exc_stub_23, IDT_GATE_INT},
    {24, exc_stub_24, IDT_GATE_INT},
    {25, exc_stub_25, IDT_GATE_INT},
    {26, exc_stub_26, IDT_GATE_INT},
    {27, exc_stub_27, IDT_GATE_INT},
    {28, exc_stub_28, IDT_GATE_INT},
    {29, exc_stub_29, IDT_GATE_INT},
    {30, exc_stub_30, IDT_GATE_INT},
    {31, exc_stub_31, IDT_GATE_INT},
    {32, irq_stub_32, IDT_GATE_INT},
    {33, irq_stub_33, IDT_GATE_INT},
    {34, irq_stub_34, IDT_GATE_INT},
    {35, irq_stub_35, IDT_GATE_INT},
    {36, irq_stub_36, IDT_GATE_INT},
    {37, irq_stub_37, IDT_GATE_INT},
    {38, irq_stub_38, IDT_GATE_INT},
    {39, irq_stub_39, IDT_GATE_INT},
    {40, irq_stub_40, IDT_GATE_INT},
    {41, irq_stub_41, IDT_GATE_INT},
    {42, irq_stub_42, IDT_GATE_INT},
    {43, irq_stub_43, IDT_GATE_INT},
    {44, irq_stub_44, IDT_GATE_INT},
    {45, irq_stub_45, IDT_GATE_INT},
    {46, irq_stub_46, IDT_GATE_INT},
    {47, irq_stub_47, IDT_GATE_INT},
    {255, apic_spurious_stub, IDT_GATE_INT},
    {SYSCALL_VECTOR_LEGACY, syscall_stub, IDT_GATE_TRAP_USER},
    {SYSCALL_VECTOR, syscall_stub, IDT_GATE_TRAP_USER},
};

static void idt_fill_default_gates(void) {
    for (int i = 0; i < IDT_ENTRY_COUNT; i++)
        idt_set_gate((uint8_t)i, (uint32_t)(uintptr_t)default_stub_table[i],
                     IDT_GATE_INT);
}

static void idt_install_named_gates(void) {
    for (size_t i = 0; i < sizeof(early_gates) / sizeof(early_gates[0]); i++) {
        const struct idt_gate_init *gate = &early_gates[i];
        idt_set_gate(gate->vector, (uint32_t)(uintptr_t)gate->handler, gate->type_attr);
    }
}

static void pic_remap(void) {
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();
    outb(0xA1, 0x28); io_wait();
    outb(0x21, 0x04); io_wait();
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();
    outb(0x21, 0xFD);  /* unmask IRQ1; timer/mouse are enabled by drivers */
    outb(0xA1, 0xFF);  /* mask all slave */
}

void idt_install(void) {
    idt_fill_default_gates();
    idt_install_named_gates();

    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (uint32_t)&idt[0];
    GNU_ASM("lidt %0" : : "m"(idt_ptr));

    pic_remap();
    GNU_ASM("sti");
}
