#ifndef BUZZOS_SETJMP_H
#define BUZZOS_SETJMP_H

/* i386 jmp_buf: ebx, esi, edi, ebp, esp, eip */
typedef unsigned long jmp_buf[6];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int value) __attribute__((noreturn));

#endif
