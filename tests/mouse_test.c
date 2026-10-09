#include <assert.h>
#include <stdio.h>
#include "mouse.h"

int main(void) {
    for (int delta = -256; delta <= 255; delta++) {
        uint8_t data = (uint8_t)delta;
        for (int buttons = 0; buttons < 8; buttons++) {
            uint8_t header = 0x08 | buttons;
            uint8_t x = header | (delta < 0 ? 0x10 : 0);
            uint8_t y = header | (delta < 0 ? 0x20 : 0);
            assert(mouse_ps2_axis(data, x, 0x10, 0x40) == delta);
            assert(mouse_ps2_axis(data, y, 0x20, 0x80) == delta);
            assert(mouse_ps2_axis(data, x | 0x40, 0x10, 0x40) == 0);
            assert(mouse_ps2_axis(data, y | 0x80, 0x20, 0x80) == 0);
            /* Overflow of the other axis must retain this axis's movement. */
            assert(mouse_ps2_axis(data, x | 0x80, 0x10, 0x40) == delta);
            assert(mouse_ps2_axis(data, y | 0x40, 0x20, 0x80) == delta);
        }
    }
    assert(mouse_ps2_axis(0xC8, 0x08, 0x20, 0x80) == 200);
    assert(mouse_ps2_axis(0x38, 0x28, 0x20, 0x80) == -200);
    puts("PS/2 signed 9-bit movement, buttons and independent overflow passed");
    return 0;
}
