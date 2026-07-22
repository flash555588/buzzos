#ifndef BUZZOS_AC97_H
#define BUZZOS_AC97_H

#include <stddef.h>
#include <stdint.h>

int ac97_init(void);
int ac97_write(const uint8_t *data, size_t size);
int ac97_set_rate(uint32_t rate);
void ac97_poll(void);
void ac97_irq_handler(void);

#endif
