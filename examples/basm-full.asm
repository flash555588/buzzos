bits 64
global _start

%define SYS_EXIT  1
%define SYS_WRITE 5

section .text
_start:
    push rbp
    mov rbp, rsp
    sub rsp, 16

    mov dword [rbp - 4], 6
    mov dword [rbp - 8], 7
    mov eax, [rbp - 4]
    imul eax, [rbp - 8]
    cmp eax, 42
    setne al
    movzx eax, al
    test eax, eax
    jne .fail

    lea rsi, [scratch]
    mov dword [rsi + 4], 42
    cmp dword [rsi + 4], 42
    jne .fail

    mov eax, SYS_WRITE
    mov edi, 1
    mov rsi, message
    mov edx, message_len
    int 0x80

    leave
    mov eax, SYS_EXIT
    mov edi, 7
    int 0x80

.fail:
    leave
    mov eax, SYS_EXIT
    mov edi, 99
    int 0x80

section .rodata
message:
    db 98, 97, 115, 109, 45, 102, 117, 108, 108, 45, 111, 107, 10
message_len equ $ - message
table:
    times 4 dd 0x12345678
    align 16

section .bss
scratch:
    resd 4
