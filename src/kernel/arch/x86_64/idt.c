#include <stddef.h>
#include <stdint.h>
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

static idt_entry_t idt[IDT_ENTRY_COUNT];
static idt_ptr_t descriptor;

static void idt_set_gate(uint8_t vector, uintptr_t handler, uint8_t type_attr) {
    idt[vector].offset_low = (uint16_t)handler;
    idt[vector].offset_mid = (uint16_t)(handler >> 16);
    idt[vector].offset_high = (uint32_t)(handler >> 32);
    idt[vector].selector = 0x08;
    idt[vector].ist = 0;
    idt[vector].type_attr = type_attr;
    idt[vector].zero = 0;
}

static const char *exception_names[32] = {
    "#DE", "#DB", "NMI", "#BP", "#OF", "#BR", "#UD", "#NM",
    "#DF", 0, "#TS", "#NP", "#SS", "#GP", "#PF", 0,
    "#MF", "#AC", "#MC", "#XM", "#VE", "#CP", 0, 0,
    0, 0, 0, 0, "#HV", "#VC", "#SX", 0,
};

/* Layout produced by SAVE_GPRS in isr.asm. */
struct exception_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
};

void exception_handler(uint64_t vector, uint64_t error,
                       const struct exception_frame *frame) {
    serial_puts("\n=== EXCEPTION x86_64 ===\nVector: ");
    serial_puthex((uint32_t)vector);
    if (vector < 32 && exception_names[vector]) {
        serial_puts(" ");
        serial_puts(exception_names[vector]);
    }
    serial_puts("\nError: ");
    serial_puthex((uint32_t)error);
    if (frame) {
        serial_puts("\nRIP=");
        serial_puthex64(frame->rip);
        serial_puts(" CS=");
        serial_puthex((uint32_t)frame->cs);
        serial_puts(" RFLAGS=");
        serial_puthex64(frame->rflags);
        serial_puts("\nRSP=");
        serial_puthex64(frame->rsp);
        serial_puts(" RAX=");
        serial_puthex64(frame->rax);
    }
    if (vector == 14) {
        uintptr_t fault_addr;
        __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));
        serial_puts("\nCR2=");
        serial_puthex64(fault_addr);
    }
    if (frame && (frame->cs & 3u) == 3u && current_task && current_task->id > 0) {
        serial_puts("\nTerminating faulting user task.\n");
        task_exit_process_code(vector ? -(int)vector : -1);
    }
    serial_puts("\nHalted.\n");
    for (;;) __asm__ volatile("cli; hlt");
}

#define EXTERN_EXC(n) extern void exc_stub_##n(void)
EXTERN_EXC(0); EXTERN_EXC(1); EXTERN_EXC(2); EXTERN_EXC(3);
EXTERN_EXC(4); EXTERN_EXC(5); EXTERN_EXC(6); EXTERN_EXC(7);
EXTERN_EXC(8); EXTERN_EXC(10); EXTERN_EXC(11); EXTERN_EXC(12);
EXTERN_EXC(13); EXTERN_EXC(14); EXTERN_EXC(15); EXTERN_EXC(16);
EXTERN_EXC(17); EXTERN_EXC(18); EXTERN_EXC(19); EXTERN_EXC(20);
EXTERN_EXC(21); EXTERN_EXC(22); EXTERN_EXC(23); EXTERN_EXC(24);
EXTERN_EXC(25); EXTERN_EXC(26); EXTERN_EXC(27); EXTERN_EXC(28);
EXTERN_EXC(29); EXTERN_EXC(30); EXTERN_EXC(31);

extern void (*default_stub_table[IDT_ENTRY_COUNT])(void);
#define EXTERN_IRQ(n) extern void irq_stub_##n(void)
EXTERN_IRQ(32); EXTERN_IRQ(33); EXTERN_IRQ(34); EXTERN_IRQ(35);
EXTERN_IRQ(36); EXTERN_IRQ(37); EXTERN_IRQ(38); EXTERN_IRQ(39);
EXTERN_IRQ(40); EXTERN_IRQ(41); EXTERN_IRQ(42); EXTERN_IRQ(43);
EXTERN_IRQ(44); EXTERN_IRQ(45); EXTERN_IRQ(46); EXTERN_IRQ(47);
extern void apic_spurious_stub(void);
extern void syscall_stub(void);

static const struct idt_gate_init named_gates[] = {
    {0, exc_stub_0, IDT_GATE_INT}, {1, exc_stub_1, IDT_GATE_INT},
    {2, exc_stub_2, IDT_GATE_INT}, {3, exc_stub_3, IDT_GATE_TRAP_USER},
    {4, exc_stub_4, IDT_GATE_INT}, {5, exc_stub_5, IDT_GATE_INT},
    {6, exc_stub_6, IDT_GATE_INT}, {7, exc_stub_7, IDT_GATE_INT},
    {8, exc_stub_8, IDT_GATE_INT}, {10, exc_stub_10, IDT_GATE_INT},
    {11, exc_stub_11, IDT_GATE_INT}, {12, exc_stub_12, IDT_GATE_INT},
    {13, exc_stub_13, IDT_GATE_INT}, {14, exc_stub_14, IDT_GATE_INT},
    {15, exc_stub_15, IDT_GATE_INT}, {16, exc_stub_16, IDT_GATE_INT},
    {17, exc_stub_17, IDT_GATE_INT}, {18, exc_stub_18, IDT_GATE_INT},
    {19, exc_stub_19, IDT_GATE_INT}, {20, exc_stub_20, IDT_GATE_INT},
    {21, exc_stub_21, IDT_GATE_INT}, {22, exc_stub_22, IDT_GATE_INT},
    {23, exc_stub_23, IDT_GATE_INT}, {24, exc_stub_24, IDT_GATE_INT},
    {25, exc_stub_25, IDT_GATE_INT}, {26, exc_stub_26, IDT_GATE_INT},
    {27, exc_stub_27, IDT_GATE_INT}, {28, exc_stub_28, IDT_GATE_INT},
    {29, exc_stub_29, IDT_GATE_INT}, {30, exc_stub_30, IDT_GATE_INT},
    {31, exc_stub_31, IDT_GATE_INT},
    {32, irq_stub_32, IDT_GATE_INT}, {33, irq_stub_33, IDT_GATE_INT},
    {34, irq_stub_34, IDT_GATE_INT}, {35, irq_stub_35, IDT_GATE_INT},
    {36, irq_stub_36, IDT_GATE_INT}, {37, irq_stub_37, IDT_GATE_INT},
    {38, irq_stub_38, IDT_GATE_INT}, {39, irq_stub_39, IDT_GATE_INT},
    {40, irq_stub_40, IDT_GATE_INT}, {41, irq_stub_41, IDT_GATE_INT},
    {42, irq_stub_42, IDT_GATE_INT}, {43, irq_stub_43, IDT_GATE_INT},
    {44, irq_stub_44, IDT_GATE_INT}, {45, irq_stub_45, IDT_GATE_INT},
    {46, irq_stub_46, IDT_GATE_INT}, {47, irq_stub_47, IDT_GATE_INT},
    {255, apic_spurious_stub, IDT_GATE_INT},
    {SYSCALL_VECTOR_LEGACY, syscall_stub, IDT_GATE_TRAP_USER},
    {SYSCALL_VECTOR, syscall_stub, IDT_GATE_TRAP_USER},
};

static void pic_remap(void) {
    outb(0x20, 0x11); io_wait(); outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait(); outb(0xA1, 0x28); io_wait();
    outb(0x21, 0x04); io_wait(); outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait(); outb(0xA1, 0x01); io_wait();
    outb(0x21, 0xFD);
    outb(0xA1, 0xFF);
}

void idt_install(void) {
    for (int i = 0; i < IDT_ENTRY_COUNT; i++)
        idt_set_gate((uint8_t)i, (uintptr_t)default_stub_table[i], IDT_GATE_INT);
    for (size_t i = 0; i < sizeof(named_gates) / sizeof(named_gates[0]); i++)
        idt_set_gate(named_gates[i].vector, (uintptr_t)named_gates[i].handler,
                     named_gates[i].type_attr);
    descriptor.limit = sizeof(idt) - 1u;
    descriptor.base = (uint64_t)(uintptr_t)idt;
    __asm__ volatile("lidt %0" : : "m"(descriptor) : "memory");
    pic_remap();
    __asm__ volatile("sti");
}
