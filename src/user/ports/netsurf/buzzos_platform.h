#ifndef BUZZOS_NETSURF_PLATFORM_H
#define BUZZOS_NETSURF_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

struct nsbuzz_surface {
    int width;
    int height;
    uint32_t *xrgb;
    uint8_t *indexed;
};

typedef void (*nsbuzz_callback)(void *context);

int nsbuzz_surface_resize(struct nsbuzz_surface *surface, int width, int height);
void nsbuzz_surface_destroy(struct nsbuzz_surface *surface);
uint8_t *nsbuzz_surface_present(struct nsbuzz_surface *surface);

int nsbuzz_schedule(int delay_ms, nsbuzz_callback callback, void *context);
int nsbuzz_schedule_run(void);
void nsbuzz_schedule_clear(void);

#endif
