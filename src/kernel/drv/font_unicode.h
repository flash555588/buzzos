#ifndef BUZZOS_FONT_UNICODE_H
#define BUZZOS_FONT_UNICODE_H

#include <stdint.h>

enum {
    UFONT_HEIGHT = 22,
    UFONT_MAX_WIDTH = 24,
    UFONT_STRIDE = 3,
    UFONT_BYTES = UFONT_HEIGHT * UFONT_STRIDE
};

/* Returns the glyph width (12 or 24), or zero when the code point is absent. */
int font_unicode_lookup(uint32_t codepoint, uint8_t out[UFONT_BYTES]);

#endif
