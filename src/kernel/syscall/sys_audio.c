#include "audio.h"
#include "syscall_internal.h"

intptr_t sys_audio_write(uintptr_t data_arg, uintptr_t size,
                    uintptr_t c, uintptr_t d, uintptr_t e) {
    (void)c; (void)d; (void)e;
    uint8_t bounce[4096];
    if (size > 16384u)
        return -1;
    size_t done = 0;
    while (done < size) {
        size_t chunk = size - done;
        if (chunk > sizeof(bounce)) chunk = sizeof(bounce);
        if (copy_from_user(bounce, data_arg + done, chunk) < 0)
            return done ? (intptr_t)done : -1;
        int written = audio_write(bounce, chunk);
        if (written <= 0)
            return done ? (intptr_t)done : written;
        done += (size_t)written;
        if ((size_t)written < chunk)
            break;
    }
    return (intptr_t)done;
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
