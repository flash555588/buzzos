#ifndef BUZZOS_CHARSET_IMPL_H
#define BUZZOS_CHARSET_IMPL_H

#include <stdint.h>
#include <errno.h>
#include <charset_gbk_tables.h>

enum charset_encoding {
    CHARSET_INVALID, CHARSET_UTF8, CHARSET_GBK, CHARSET_ASCII,
    CHARSET_LATIN1, CHARSET_CP1252, CHARSET_UTF16LE, CHARSET_UTF16BE,
    CHARSET_UTF32LE, CHARSET_UTF32BE
};

struct charset_descriptor {
    enum charset_encoding from;
    enum charset_encoding to;
};

static const uint16_t charset_cp1252[] = {
    0x20ac,0x0081,0x201a,0x0192,0x201e,0x2026,0x2020,0x2021,
    0x02c6,0x2030,0x0160,0x2039,0x0152,0x008d,0x017d,0x008f,
    0x0090,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,
    0x02dc,0x2122,0x0161,0x203a,0x0153,0x009d,0x017e,0x0178
};

uint32_t buzzos_gbk_decode(unsigned char lead, unsigned char trail) {
    if (lead < 0x81 || lead > 0xfe || trail < 0x40 || trail == 0x7f || trail > 0xfe)
        return 0;
    size_t index = (size_t)(lead - 0x81) * 190 + trail - 0x40 - (trail > 0x7f);
    return charset_gbk_decode_table[index];
}

unsigned int buzzos_gbk_encode(uint32_t codepoint) {
    size_t lower = 0;
    size_t upper = sizeof(charset_gbk_encode_table) / sizeof(charset_gbk_encode_table[0]);
    while (lower < upper) {
        size_t middle = lower + (upper - lower) / 2;
        uint32_t entry = charset_gbk_encode_table[middle];
        if ((entry >> 16) < codepoint) lower = middle + 1;
        else if ((entry >> 16) > codepoint) upper = middle;
        else return entry & 0xffff;
    }
    return 0;
}

static enum charset_encoding charset_name(const char *name) {
    char normalized[32];
    size_t length = 0;
    if (!name) return CHARSET_INVALID;
    while (*name) {
        unsigned char character = (unsigned char)*name++;
        if (character == '-' || character == '_') continue;
        if (length + 1 >= sizeof(normalized)) return CHARSET_INVALID;
        if (character >= 'a' && character <= 'z') character -= 'a' - 'A';
        normalized[length++] = (char)character;
    }
    normalized[length] = 0;
    if (!strcmp(normalized, "UTF8")) return CHARSET_UTF8;
    if (!strcmp(normalized, "GBK") || !strcmp(normalized, "GB2312") ||
        !strcmp(normalized, "GB231280") || !strcmp(normalized, "CP936") ||
        !strcmp(normalized, "MS936") || !strcmp(normalized, "WINDOWS936") ||
        !strcmp(normalized, "CHINESE") || !strcmp(normalized, "CSGB2312")) return CHARSET_GBK;
    if (!strcmp(normalized, "ASCII") || !strcmp(normalized, "USASCII")) return CHARSET_ASCII;
    if (!strcmp(normalized, "ISO88591") || !strcmp(normalized, "LATIN1")) return CHARSET_LATIN1;
    if (!strcmp(normalized, "WINDOWS1252") || !strcmp(normalized, "CP1252")) return CHARSET_CP1252;
    if (!strcmp(normalized, "UTF16LE")) return CHARSET_UTF16LE;
    if (!strcmp(normalized, "UTF16BE")) return CHARSET_UTF16BE;
    if (!strcmp(normalized, "UTF32LE") || !strcmp(normalized, "UCS4LE")) return CHARSET_UTF32LE;
    if (!strcmp(normalized, "UTF32BE") || !strcmp(normalized, "UCS4BE")) return CHARSET_UTF32BE;
    return CHARSET_INVALID;
}

static uint16_t charset_read16(const unsigned char *input, int little) {
    return little ? input[0] | ((uint16_t)input[1] << 8) : ((uint16_t)input[0] << 8) | input[1];
}

static int charset_read(enum charset_encoding encoding, const unsigned char *input,
                        size_t available, uint32_t *codepoint, size_t *used) {
    uint32_t value = input[0];
    size_t count = 1;
    switch (encoding) {
    case CHARSET_UTF8:
        if (value >= 0x80) {
            if (value >= 0xc2 && value <= 0xdf) { count = 2; value &= 0x1f; }
            else if (value >= 0xe0 && value <= 0xef) { count = 3; value &= 0x0f; }
            else if (value >= 0xf0 && value <= 0xf4) { count = 4; value &= 7; }
            else return EILSEQ;
            for (size_t index = 1; index < count; index++) {
                if (index >= available) return EINVAL;
                if ((input[index] & 0xc0) != 0x80) return EILSEQ;
                if (index == 1 && ((input[0] == 0xe0 && input[1] < 0xa0) ||
                    (input[0] == 0xed && input[1] >= 0xa0) ||
                    (input[0] == 0xf0 && input[1] < 0x90) ||
                    (input[0] == 0xf4 && input[1] >= 0x90))) return EILSEQ;
                value = (value << 6) | (input[index] & 0x3f);
            }
        }
        break;
    case CHARSET_GBK:
        if (value == 0x80) value = 0x20ac;
        else if (value > 0x80 && value < 0xff) {
            if (available < 2) return EINVAL;
            value = buzzos_gbk_decode(input[0], input[1]);
            if (!value) return EILSEQ;
            count = 2;
        } else if (value == 0xff) return EILSEQ;
        break;
    case CHARSET_ASCII:
        if (value > 0x7f) return EILSEQ;
        break;
    case CHARSET_LATIN1:
        break;
    case CHARSET_CP1252:
        if (value >= 0x80 && value <= 0x9f) value = charset_cp1252[value - 0x80];
        break;
    case CHARSET_UTF16LE:
    case CHARSET_UTF16BE: {
        if (available < 2) return EINVAL;
        int little = encoding == CHARSET_UTF16LE;
        value = charset_read16(input, little);
        count = 2;
        if (value >= 0xd800 && value <= 0xdbff) {
            if (available < 4) return EINVAL;
            uint32_t trail = charset_read16(input + 2, little);
            if (trail < 0xdc00 || trail > 0xdfff) return EILSEQ;
            value = 0x10000 + ((value - 0xd800) << 10) + trail - 0xdc00;
            count = 4;
        } else if (value >= 0xdc00 && value <= 0xdfff) return EILSEQ;
        break;
    }
    case CHARSET_UTF32LE:
    case CHARSET_UTF32BE:
        if (available < 4) return EINVAL;
        if (encoding == CHARSET_UTF32LE)
            value = input[0] | ((uint32_t)input[1] << 8) | ((uint32_t)input[2] << 16) | ((uint32_t)input[3] << 24);
        else
            value = ((uint32_t)input[0] << 24) | ((uint32_t)input[1] << 16) | ((uint32_t)input[2] << 8) | input[3];
        count = 4;
        break;
    default:
        return EINVAL;
    }
    if (value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return EILSEQ;
    *codepoint = value;
    *used = count;
    return 0;
}

static void charset_write16(unsigned char *output, uint16_t value, int little) {
    output[little ? 0 : 1] = (unsigned char)value;
    output[little ? 1 : 0] = (unsigned char)(value >> 8);
}

static int charset_write(enum charset_encoding encoding, uint32_t value,
                         unsigned char *output, size_t *used) {
    size_t count = 1;
    switch (encoding) {
    case CHARSET_UTF8:
        if (value < 0x80) output[0] = (unsigned char)value;
        else {
            count = value < 0x800 ? 2 : value < 0x10000 ? 3 : 4;
            for (size_t index = count - 1; index > 0; index--) {
                output[index] = 0x80 | (value & 0x3f);
                value >>= 6;
            }
            output[0] = (unsigned char)((count == 2 ? 0xc0 : count == 3 ? 0xe0 : 0xf0) | value);
        }
        break;
    case CHARSET_GBK:
        if (value < 0x80) output[0] = (unsigned char)value;
        else if (value == 0x20ac) output[0] = 0x80;
        else {
            unsigned int mapped = buzzos_gbk_encode(value);
            if (!mapped) return EILSEQ;
            output[0] = (unsigned char)(mapped >> 8);
            output[1] = (unsigned char)mapped;
            count = 2;
        }
        break;
    case CHARSET_ASCII:
    case CHARSET_LATIN1:
        if (value > (encoding == CHARSET_ASCII ? 0x7f : 0xff)) return EILSEQ;
        output[0] = (unsigned char)value;
        break;
    case CHARSET_CP1252:
        if (value < 0x80 || (value >= 0xa0 && value <= 0xff)) output[0] = (unsigned char)value;
        else {
            size_t index = 0;
            while (index < 32 && charset_cp1252[index] != value) index++;
            if (index == 32) return EILSEQ;
            output[0] = (unsigned char)(0x80 + index);
        }
        break;
    case CHARSET_UTF16LE:
    case CHARSET_UTF16BE: {
        int little = encoding == CHARSET_UTF16LE;
        count = 2;
        if (value > 0xffff) {
            value -= 0x10000;
            charset_write16(output, (uint16_t)(0xd800 | (value >> 10)), little);
            charset_write16(output + 2, (uint16_t)(0xdc00 | (value & 0x3ff)), little);
            count = 4;
        } else charset_write16(output, (uint16_t)value, little);
        break;
    }
    case CHARSET_UTF32LE:
    case CHARSET_UTF32BE:
        count = 4;
        for (size_t index = 0; index < 4; index++)
            output[encoding == CHARSET_UTF32LE ? index : 3 - index] = (unsigned char)(value >> (index * 8));
        break;
    default:
        return EINVAL;
    }
    *used = count;
    return 0;
}

iconv_t iconv_open(const char *to_encoding, const char *from_encoding) {
    enum charset_encoding from = charset_name(from_encoding);
    enum charset_encoding to = charset_name(to_encoding);
    if (from == CHARSET_INVALID || to == CHARSET_INVALID) {
        errno = EINVAL;
        return (iconv_t)-1;
    }
    struct charset_descriptor *descriptor = malloc(sizeof(*descriptor));
    if (!descriptor) { errno = ENOMEM; return (iconv_t)-1; }
    descriptor->from = from;
    descriptor->to = to;
    return descriptor;
}

size_t iconv(iconv_t handle, char **input, size_t *input_left,
             char **output, size_t *output_left) {
    if (!handle || handle == (iconv_t)-1) { errno = EBADF; return (size_t)-1; }
    if (!input || !*input) return 0;
    if (!input_left || !output || !*output || !output_left) { errno = EINVAL; return (size_t)-1; }
    struct charset_descriptor *descriptor = handle;
    while (*input_left) {
        uint32_t codepoint;
        size_t consumed, produced;
        unsigned char encoded[4];
        int error = charset_read(descriptor->from, (const unsigned char *)*input,
                                  *input_left, &codepoint, &consumed);
        if (!error) error = charset_write(descriptor->to, codepoint, encoded, &produced);
        if (!error && produced > *output_left) error = E2BIG;
        if (error) { errno = error; return (size_t)-1; }
        memcpy(*output, encoded, produced);
        *input += consumed;
        *input_left -= consumed;
        *output += produced;
        *output_left -= produced;
    }
    return 0;
}

int iconv_close(iconv_t descriptor) {
    if (!descriptor || descriptor == (iconv_t)-1) { errno = EBADF; return -1; }
    free(descriptor);
    return 0;
}

#endif
