#ifndef BUZZOS_I386_FPU_H
#define BUZZOS_I386_FPU_H

#include <stdint.h>

#define FPU_STATE_SIZE 512

int fpu_init(void);
int fpu_available(void);
void fpu_state_init(void *state);
void fpu_state_save(void *state);
void fpu_state_restore(const void *state);

#endif
