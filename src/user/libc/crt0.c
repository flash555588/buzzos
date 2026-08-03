#include "libc.h"

/* User program provides main(). */
extern int main(int argc, char **argv);

__asm__(
    ".section .text.entry,\"ax\",@progbits\n"
    ".globl _start\n"
    "_start:\n"
    "    xorq %rbp, %rbp\n"
    "    movq (%rsp), %rdi\n"
    "    leaq 8(%rsp), %rsi\n"
    "    andq $-16, %rsp\n"
    "    callq main\n"
    "    movl %eax, %edi\n"
    "    callq exit\n"
    "1:\n"
    "    jmp 1b\n"
);
