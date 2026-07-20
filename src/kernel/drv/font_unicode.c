#include "font_unicode.h"
#include "font_unicode_data.h"

_Static_assert(KFONT_UNICODE_STRIDE == UFONT_STRIDE, "Unicode font stride mismatch");
_Static_assert(KFONT_UNICODE_BYTES == UFONT_BYTES, "Unicode font size mismatch");

int font_unicode_lookup(uint32_t codepoint, uint8_t out[UFONT_BYTES]) {
    int lo = 0;
    int hi = KFONT_UNICODE_COUNT - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint32_t found = kfont_unicode_codepoints[mid];
        if (found < codepoint) {
            lo = mid + 1;
        } else if (found > codepoint) {
            hi = mid - 1;
        } else {
            if (out) {
                for (int i = 0; i < UFONT_BYTES; i++)
                    out[i] = kfont_unicode_bits[mid][i];
            }
            return kfont_unicode_widths[mid];
        }
    }
    return 0;
}
