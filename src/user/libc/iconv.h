#ifndef BUZZOS_ICONV_COMPAT_H
#define BUZZOS_ICONV_COMPAT_H

#include <stddef.h>

typedef void *iconv_t;

iconv_t iconv_open(const char *to_encoding, const char *from_encoding);
size_t iconv(iconv_t descriptor, char **input, size_t *input_left,
             char **output, size_t *output_left);
int iconv_close(iconv_t descriptor);

#endif
