#ifndef BUZZOS_KEYBOARD_H
#define BUZZOS_KEYBOARD_H

#include <stdint.h>

void keyboard_init(void);
void keyboard_handler(uint8_t scancode);
int  keyboard_getchar(void);   /* blocking — returns ASCII or 0 if empty */
/* Non-blocking logical key event. Bit 15 is set for key-down; low 15 bits
 * contain ASCII or the desktop arrow constants 256..259. */
int  keyboard_getevent(void);

#endif
