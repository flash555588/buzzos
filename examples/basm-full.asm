bits 32
global _start

%define SYS_EXIT  1
%define SYS_WRITE 5

section .text
_start:
    push ebp
    mov ebp, esp
    sub esp, 16

    mov dword [ebp - 4], 6
    mov dword [ebp - 8], 7
    mov eax, [ebp - 4]
    imul eax, [ebp - 8]
    cmp eax, 42
    setne al
    movzx eax, al
    test eax, eax
    jne .fail

    lea esi, [scratch]
    mov dword [esi + 4], 42
    cmp dword [esi + 4], 42
    jne .fail

    mov eax, SYS_WRITE
    mov ebx, 1
    mov ecx, message
    mov edx, message_len
    int 0x80

    leave
    mov eax, SYS_EXIT
    mov ebx, 7
    int 0x80

.fail:
    leave
    mov eax, SYS_EXIT
    mov ebx, 99
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
