#ifndef BUZZOS_AUDIO_H
#define BUZZOS_AUDIO_H

#include <stddef.h>
#include <stdint.h>

int audio_init(void);
int audio_start_worker(void);
int audio_write(const uint8_t *data, size_t size);
int audio_set_rate(uint32_t rate, uint32_t latency_ms);
int audio_flush(void);
int audio_queued_samples(void);
const char *audio_driver_name(void);

#endif
