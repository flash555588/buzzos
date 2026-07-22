#include "ac97.h"
#include "syscall_internal.h"

int sys_audio_write(uint32_t data_arg, uint32_t size,
                    uint32_t c, uint32_t d, uint32_t e) {
    (void)c; (void)d; (void)e;
    if (size > 16384u || !user_range_ok(data_arg, size))
        return -1;
    return ac97_write((const uint8_t *)(uintptr_t)data_arg, size);
}
