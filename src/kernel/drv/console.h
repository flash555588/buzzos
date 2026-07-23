#ifndef BUZZOS_CONSOLE_H
#define BUZZOS_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

int  console_init(void);
void console_set_color(uint8_t foreground, uint8_t background);
void console_clear(void);
void console_putc(char character);
void console_puts(const char *text);
void console_write(const char *text, size_t count);
void console_backspace(void);

/* Called when the physical display returns to the boot console. */
void console_activate(int present_now);

#endif /* BUZZOS_CONSOLE_H */
