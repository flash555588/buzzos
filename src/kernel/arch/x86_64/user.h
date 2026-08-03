#ifndef BUZZOS_X86_64_USER_H
#define BUZZOS_X86_64_USER_H

#include <stdint.h>
#include "user_bounds.h"

int user_install_trampoline(void);
int user_install_trampoline_in_space(uintptr_t cr3);
void user_enter(uintptr_t entry, uintptr_t stack_top) __attribute__((noreturn));

#endif
