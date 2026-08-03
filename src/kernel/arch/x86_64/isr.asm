bits 64
default rel
section .text

extern exception_handler
extern syscall_handler
extern irq_dispatch
extern keyboard_handler
extern mouse_handler
extern timer_irq

%macro SAVE_GPRS 0
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
%endmacro

%macro RESTORE_GPRS 0
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
%endmacro

%macro EXC_NOERR 1
global exc_stub_%1
exc_stub_%1:
    push qword 0
    push qword %1
    jmp common_exception
%endmacro

%macro EXC_ERR 1
global exc_stub_%1
exc_stub_%1:
    push qword %1
    jmp common_exception
%endmacro

common_exception:
    cld
    SAVE_GPRS
    mov rdi, [rsp + 120]
    mov rsi, [rsp + 128]
    mov rdx, rsp
    mov r12, rsp
    and rsp, -16
    call exception_handler
    mov rsp, r12
    RESTORE_GPRS
    add rsp, 16
    iretq

EXC_NOERR 0
EXC_NOERR 1
EXC_NOERR 2
EXC_NOERR 3
EXC_NOERR 4
EXC_NOERR 5
EXC_NOERR 6
EXC_NOERR 7
EXC_ERR   8
EXC_NOERR 9
EXC_ERR   10
EXC_ERR   11
EXC_ERR   12
EXC_ERR   13
EXC_ERR   14
EXC_NOERR 15
EXC_NOERR 16
EXC_ERR   17
EXC_NOERR 18
EXC_NOERR 19
EXC_NOERR 20
EXC_ERR   21
EXC_NOERR 22
EXC_NOERR 23
EXC_NOERR 24
EXC_NOERR 25
EXC_NOERR 26
EXC_NOERR 27
EXC_NOERR 28
EXC_ERR   29
EXC_ERR   30
EXC_NOERR 31

%assign default_vector 0
%rep 256
global default_stub_%+default_vector
default_stub_%+default_vector:
    push qword 0
    push qword default_vector
    jmp common_exception
%assign default_vector default_vector+1
%endrep

section .rodata
align 8
global default_stub_table
default_stub_table:
%assign default_vector 0
%rep 256
    dq default_stub_%+default_vector
%assign default_vector default_vector+1
%endrep

section .text

%macro GENERIC_IRQ 1
global irq_stub_%1
irq_stub_%1:
    cld
    SAVE_GPRS
    mov edi, (%1 - 32)
    mov r12, rsp
    and rsp, -16
    call irq_dispatch
    mov rsp, r12
    RESTORE_GPRS
    iretq
%endmacro

global irq_stub_32
irq_stub_32:
    cld
    SAVE_GPRS
    mov al, 0x20
    out 0x20, al
    mov r12, rsp
    and rsp, -16
    call timer_irq
    mov rsp, r12
    RESTORE_GPRS
    iretq

global irq_stub_33
irq_stub_33:
    cld
    SAVE_GPRS
    xor eax, eax
    in al, 0x64
    test al, 0x01
    jz .kbd_done
    test al, 0x20
    jnz .kbd_done
    xor eax, eax
    in al, 0x60
    mov edi, eax
    mov r12, rsp
    and rsp, -16
    call keyboard_handler
    mov rsp, r12
.kbd_done:
    mov al, 0x20
    out 0x20, al
    RESTORE_GPRS
    iretq

GENERIC_IRQ 34
GENERIC_IRQ 35
GENERIC_IRQ 36

global irq_stub_37
irq_stub_37:
    SAVE_GPRS
    mov al, 0x20
    out 0x20, al
    RESTORE_GPRS
    iretq

GENERIC_IRQ 38
GENERIC_IRQ 39
GENERIC_IRQ 40
GENERIC_IRQ 41
GENERIC_IRQ 42
GENERIC_IRQ 43

global irq_stub_44
irq_stub_44:
    cld
    SAVE_GPRS
    xor eax, eax
    in al, 0x64
    test al, 0x01
    jz .mouse_done
    test al, 0x20
    jz .mouse_done
    xor eax, eax
    in al, 0x60
    mov edi, eax
    mov r12, rsp
    and rsp, -16
    call mouse_handler
    mov rsp, r12
.mouse_done:
    mov al, 0x20
    out 0xA0, al
    out 0x20, al
    RESTORE_GPRS
    iretq

GENERIC_IRQ 45
GENERIC_IRQ 46
GENERIC_IRQ 47

global apic_spurious_stub
apic_spurious_stub:
    iretq

; int 0x80 uses the x86_64 Linux register convention:
;   rax=number, rdi,rsi,rdx,r10,r8,r9=arguments.
global syscall_stub
syscall_stub:
    cld
    push qword 0
    push qword 0x80
    SAVE_GPRS
    mov r12, rsp
    mov rdi, r12
    and rsp, -16
    call syscall_handler
    mov rsp, r12
    RESTORE_GPRS
    add rsp, 16
    iretq
