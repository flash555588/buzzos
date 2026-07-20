#ifndef BUZZOS_USER_H
#define BUZZOS_USER_H

#include <stdint.h>
#include "user_bounds.h"

/* Install the fixed, per-address-space ring-3 entry trampoline. */
int user_install_trampoline(void);

/* Enter ring 3 through the fixed trampoline, passing the final entry in EDX. */
void user_enter(uint32_t entry, uint32_t stack_top) __attribute__((noreturn));

#endif /* BUZZOS_USER_H */
