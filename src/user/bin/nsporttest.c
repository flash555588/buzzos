#include "libc.h"
#include "buzzos_platform.h"

static void increment(void *context) {
    (*(int *)context)++;
}

int main(void) {
    struct nsbuzz_surface surface = {0};
    if (nsbuzz_surface_resize(&surface, 2, 1) < 0) {
        puts("nsporttest: surface allocation failed");
        return 1;
    }
    surface.xrgb[0] = 0x00ff0000u;
    surface.xrgb[1] = 0x0000ff00u;
    uint8_t *pixels = nsbuzz_surface_present(&surface);
    if (!pixels || pixels[0] != 196u || pixels[1] != 46u) {
        puts("nsporttest: surface conversion failed");
        nsbuzz_surface_destroy(&surface);
        return 1;
    }
    nsbuzz_surface_destroy(&surface);

    int callbacks = 0;
    uint32_t before = monotonic_ms();
    if (nsbuzz_schedule(10, increment, &callbacks) < 0) {
        puts("nsporttest: schedule allocation failed");
        return 1;
    }
    sleep_ms(20);
    int next = nsbuzz_schedule_run();
    uint32_t elapsed = monotonic_ms() - before;
    nsbuzz_schedule_clear();
    if (callbacks != 1 || next != -1 || elapsed < 10u) {
        printf("nsporttest: scheduler failed callbacks=%d next=%d elapsed=%u\n",
               callbacks, next, elapsed);
        return 1;
    }
    puts("nsporttest: ok surface scheduler");
    return 0;
}
