bits 64
section .text
global kernel_setjmp
global kernel_longjmp

; jmp_buf: rbx, rbp, r12, r13, r14, r15, rsp, rip (8 uint64_t)
kernel_setjmp:
    mov [rdi + 0], rbx
    mov [rdi + 8], rbp
    mov [rdi + 16], r12
    mov [rdi + 24], r13
    mov [rdi + 32], r14
    mov [rdi + 40], r15
    lea rax, [rsp + 8]
    mov [rdi + 48], rax
    mov rax, [rsp]
    mov [rdi + 56], rax
    xor eax, eax
    ret

kernel_longjmp:
    mov rdx, rdi
    mov eax, esi
    test eax, eax
    jnz .nonzero
    inc eax
.nonzero:
    mov rbx, [rdx + 0]
    mov rbp, [rdx + 8]
    mov r12, [rdx + 16]
    mov r13, [rdx + 24]
    mov r14, [rdx + 32]
    mov r15, [rdx + 40]
    mov rsp, [rdx + 48]
    jmp [rdx + 56]
