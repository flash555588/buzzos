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
void mouse_handler(uint8_t byte);
void mouse_get_state(struct mouse_state *out);
void mouse_clamp_to_screen(void);

#endif /* BUZZOS_MOUSE_H */
