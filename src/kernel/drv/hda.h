#ifndef BUZZOS_HDA_H
#define BUZZOS_HDA_H

#include <stddef.h>
#include <stdint.h>

int hda_init(void);
void hda_poll(void);
int hda_write(const uint8_t *data, size_t size);
int hda_set_rate(uint32_t rate, uint32_t latency_ms);
int hda_flush(void);
int hda_queued_samples(void);

#endif
