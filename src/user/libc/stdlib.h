#ifndef BUZZOS_STDLIB_COMPAT_H
#define BUZZOS_STDLIB_COMPAT_H

#include "libc.h"

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 2147483647

int rand(void);
void srand(unsigned int seed);
int system(const char *command);

#endif
