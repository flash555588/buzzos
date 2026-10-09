#ifndef BUZZOS_MOUSE_H
#define BUZZOS_MOUSE_H

#include <stdint.h>

struct mouse_state {
    int x;
    int y;
    int buttons;
    int dx;
    int dy;
    uint32_t seq;
    int wheel;
    uint32_t wheel_seq;
};

void mouse_init(void);
/* PS/2 stores the ninth (sign) bit in the packet header, not in data bit 7.
 * Keep valid movement on the other axis when only one axis overflows. */
static inline int mouse_ps2_axis(uint8_t data, uint8_t header,
                                 uint8_t sign_mask, uint8_t overflow_mask) {
    if (header & overflow_mask) return 0;
    return (int)data - ((header & sign_mask) ? 256 : 0);
}

void mouse_handler(uint8_t byte);
/* Select an absolute pointing device as the authoritative pointer source.
 * PS/2 remains initialized, but its relative packets are ignored while this
 * mode is active so the two devices cannot fight over one cursor. */
void mouse_set_absolute_mode(int enabled);
/* Publish one complete absolute-device report in screen-pixel coordinates.
 * wheel is a signed delta; buttons use bits 0=left, 1=right, 2=middle. */
void mouse_absolute_event(int x, int y, int buttons, int wheel);
void mouse_get_state(struct mouse_state *out);
void mouse_clamp_to_screen(void);

#endif /* BUZZOS_MOUSE_H */
