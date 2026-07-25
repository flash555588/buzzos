#ifndef BUZZOS_FONT_UNICODE_H
#define BUZZOS_FONT_UNICODE_H

#include <stdint.h>

enum {
    UFONT_HEIGHT = 28,
    UFONT_MAX_WIDTH = 30,
    UFONT_STRIDE = 4,
    UFONT_BYTES = UFONT_HEIGHT * UFONT_STRIDE
};

/* Returns the glyph width (15 or 30), or zero when the code point is absent. */
int font_unicode_lookup(uint32_t codepoint, uint8_t out[UFONT_BYTES]);

#endif
