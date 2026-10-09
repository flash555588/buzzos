#ifndef BUZZOS_ICONV_COMPAT_H
#define BUZZOS_ICONV_COMPAT_H

#include <stddef.h>
#include <stdint.h>

typedef void *iconv_t;

iconv_t iconv_open(const char *to_encoding, const char *from_encoding);
size_t iconv(iconv_t descriptor, char **input, size_t *input_left,
             char **output, size_t *output_left);
int iconv_close(iconv_t descriptor);
uint32_t buzzos_gbk_decode(unsigned char lead, unsigned char trail);
unsigned int buzzos_gbk_encode(uint32_t codepoint);

#endif
