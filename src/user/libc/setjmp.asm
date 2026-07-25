; User-space setjmp/longjmp for i386 (Lua error handling, etc).
; jmp_buf layout (6 uint32_t): [ebx, esi, edi, ebp, esp, eip]

bits 32
section .text

global setjmp
global longjmp

; int setjmp(jmp_buf env)
setjmp:
    mov eax, [esp + 4]
    mov [eax + 0], ebx
    mov [eax + 4], esi
    mov [eax + 8], edi
    mov [eax + 12], ebp
    lea ecx, [esp + 4]
    mov [eax + 16], ecx
    mov ecx, [esp]
    mov [eax + 20], ecx
    xor eax, eax
    ret

; void longjmp(jmp_buf env, int val)
longjmp:
    mov edx, [esp + 4]
    mov eax, [esp + 8]
    test eax, eax
    jnz .nonzero
    inc eax
.nonzero:
    mov ebx, [edx + 0]
    mov esi, [edx + 4]
    mov edi, [edx + 8]
    mov ebp, [edx + 12]
    mov esp, [edx + 16]
    jmp [edx + 20]
