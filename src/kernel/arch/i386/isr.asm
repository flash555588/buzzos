; === Exception / interrupt stub table =====================================
bits 32
section .text

extern exception_handler
extern syscall_handler

; Stack layout before common_exception:
;   no-error exceptions: [vector, error=0, cpu frame...]
;   error exceptions:    [vector, cpu error, cpu frame...]
; After pusha, vector is at [esp + 32] and error code at [esp + 36].

%macro EXC_NOERR 1
global exc_stub_%1
exc_stub_%1:
    push dword 0
    push dword %1
    jmp common_exception
%endmacro

%macro EXC_ERR 1
global exc_stub_%1
exc_stub_%1:
    push dword %1
    jmp common_exception
%endmacro

common_exception:
    pusha
    mov eax, [esp + 32]
    mov edx, [esp + 36]
    push esp
    push edx
    push eax
    call exception_handler
    add esp, 12
    jmp common_iret

common_iret:
    popa
    add esp, 8
    iret

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

%macro DUMMY_IRQ 1
global irq_stub_%1
irq_stub_%1:
    push dword 0
    push dword %1
    pusha
    push esp
    push dword 0
    push dword %1
    call exception_handler
    add esp, 12
    jmp common_iret
%endmacro

extern keyboard_handler
global irq_stub_33
irq_stub_33:
    pusha
    xor eax, eax
    in al, 0x64
    test al, 0x01
    jz .done
    test al, 0x20
    jnz .done
    xor eax, eax
    in al, 0x60
    push eax
    call keyboard_handler
    add esp, 4
.done:
    mov al, 0x20
    out 0x20, al
    popa
    iret

; Timer IRQ (IRQ0 → INT 32): send EOI, call timer_irq, iret.
; EOI is sent BEFORE the handler so the (possibly long-running) context
; switch inside timer_irq does not block further timer interrupts.
extern timer_irq
global irq_stub_32
irq_stub_32:
    pusha
    mov al, 0x20
    out 0x20, al
    call timer_irq
    popa
    iret

%assign i 34
%rep 3
  DUMMY_IRQ i
  %assign i i+1
%endrep

; Unused legacy IRQ5. Keep a harmless PIC acknowledgement gate installed.
global irq_stub_37
irq_stub_37:
    pusha
    mov al, 0x20
    out 0x20, al
    popa
    iret

%assign i 38
%rep 4
  DUMMY_IRQ i
  %assign i i+1
%endrep

; NE2000 IRQ10 (slave PIC IRQ2 -> INT 42). The driver drains the small NIC
; ring into a software queue before acknowledging the PIC.
extern ne2000_irq_handler
global irq_stub_42
irq_stub_42:
    pusha
    call ne2000_irq_handler
    mov al, 0x20
    out 0xA0, al
    out 0x20, al
    popa
    iret

; Intel ICH AC97 PCI IRQ11.
extern ac97_irq_handler
global irq_stub_43
irq_stub_43:
    pusha
    call ac97_irq_handler
    mov al, 0x20
    out 0xA0, al
    out 0x20, al
    popa
    iret

extern mouse_handler
global irq_stub_44
irq_stub_44:
    pusha
    xor eax, eax
    in al, 0x64
    test al, 0x01
    jz .done
    test al, 0x20
    jz .done
    xor eax, eax
    in al, 0x60
    push eax
    call mouse_handler
    add esp, 4
.done:
    mov al, 0x20
    out 0xA0, al
    out 0x20, al
    popa
    iret

%assign i 45
%rep 3
  DUMMY_IRQ i
  %assign i i+1
%endrep

; syscall_stub saves the general-purpose register set as syscall_frame
; and lets the C dispatcher update eax with the return value.
global syscall_stub
syscall_stub:
    pusha
    push esp
    call syscall_handler
    add esp, 4
    popa
    iret
