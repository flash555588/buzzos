#ifndef BUZZOS_ASSERT_COMPAT_H
#define BUZZOS_ASSERT_COMPAT_H

#include "libc.h"

#ifdef NDEBUG
#define assert(condition) ((void)0)
#else
#define assert(condition) ((condition) ? (void)0 : abort())
#endif

#endif
