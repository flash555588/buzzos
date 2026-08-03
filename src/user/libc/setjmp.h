#ifndef BUZZOS_SETJMP_H
#define BUZZOS_SETJMP_H

/* SysV x86_64: rbx, rbp, r12-r15, rsp, rip. */
typedef unsigned long jmp_buf[8];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int value) __attribute__((noreturn));

#endif
