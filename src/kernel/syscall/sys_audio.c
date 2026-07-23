#include "audio.h"
#include "syscall_internal.h"

int sys_audio_write(uint32_t data_arg, uint32_t size,
                    uint32_t c, uint32_t d, uint32_t e) {
    (void)c; (void)d; (void)e;
    if (size > 16384u || !user_range_ok(data_arg, size))
        return -1;
    return audio_write((const uint8_t *)(uintptr_t)data_arg, size);
}

int sys_audio_config(uint32_t rate, uint32_t b,
                     uint32_t c, uint32_t d, uint32_t e) {
    (void)c; (void)d; (void)e;
    return audio_set_rate(rate, b);
}

int sys_audio_queued(uint32_t a, uint32_t b,
                     uint32_t c, uint32_t d, uint32_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return audio_queued_samples();
}

int sys_audio_flush(uint32_t a, uint32_t b,
                    uint32_t c, uint32_t d, uint32_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return audio_flush();
}
