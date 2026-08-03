#include "audio.h"
#include "syscall_internal.h"

intptr_t sys_audio_write(uintptr_t data_arg, uintptr_t size,
                    uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)c; (void)d; (void)e;
    if (size > 16384u || !user_range_ok(data_arg, size))
        return -1;
    return audio_write((const uint8_t *)(uintptr_t)data_arg, size);
}

intptr_t sys_audio_config(uintptr_t rate, uintptr_t b,
                     uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)c; (void)d; (void)e;
    return audio_set_rate(rate, b);
}

intptr_t sys_audio_queued(uintptr_t a, uintptr_t b,
                     uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return audio_queued_samples();
}

intptr_t sys_audio_flush(uintptr_t a, uintptr_t b,
                    uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    return audio_flush();
}
