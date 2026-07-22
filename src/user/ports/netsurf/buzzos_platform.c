#include "buzzos_platform.h"
#include "libc.h"

struct scheduled_callback {
    struct scheduled_callback *next;
    uint32_t deadline;
    nsbuzz_callback callback;
    void *context;
};

static struct scheduled_callback *scheduled;

static int deadline_reached(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}

static uint8_t rgb6(unsigned red, unsigned green, unsigned blue) {
    unsigned r = (red * 5u + 127u) / 255u;
    unsigned g = (green * 5u + 127u) / 255u;
    unsigned b = (blue * 5u + 127u) / 255u;
    return (uint8_t)(16u + r * 36u + g * 6u + b);
}

int nsbuzz_surface_resize(struct nsbuzz_surface *surface, int width, int height) {
    if (!surface || width <= 0 || height <= 0)
        return -1;
    size_t count = (size_t)width * (size_t)height;
    if (count / (size_t)width != (size_t)height ||
        count > (size_t)-1 / sizeof(uint32_t))
        return -1;
    uint32_t *xrgb = malloc(count * sizeof(uint32_t));
    uint8_t *indexed = malloc(count);
    if (!xrgb || !indexed) {
        free(xrgb);
        free(indexed);
        return -1;
    }
    memset(xrgb, 0, count * sizeof(uint32_t));
    memset(indexed, 0, count);
    free(surface->xrgb);
    free(surface->indexed);
    surface->xrgb = xrgb;
    surface->indexed = indexed;
    surface->width = width;
    surface->height = height;
    return 0;
}

void nsbuzz_surface_destroy(struct nsbuzz_surface *surface) {
    if (!surface)
        return;
    free(surface->xrgb);
    free(surface->indexed);
    memset(surface, 0, sizeof(*surface));
}

uint8_t *nsbuzz_surface_present(struct nsbuzz_surface *surface) {
    if (!surface || !surface->xrgb || !surface->indexed)
        return 0;
    size_t count = (size_t)surface->width * (size_t)surface->height;
    for (size_t i = 0; i < count; i++) {
        uint32_t pixel = surface->xrgb[i];
        surface->indexed[i] = rgb6((pixel >> 16) & 255u,
                                   (pixel >> 8) & 255u,
                                   pixel & 255u);
    }
    return surface->indexed;
}

int nsbuzz_schedule(int delay_ms, nsbuzz_callback callback, void *context) {
    struct scheduled_callback **link = &scheduled;
    while (*link) {
        struct scheduled_callback *entry = *link;
        if (entry->callback == callback && entry->context == context) {
            *link = entry->next;
            free(entry);
        } else {
            link = &entry->next;
        }
    }
    if (delay_ms < 0)
        return 0;
    if (!callback)
        return -1;
    struct scheduled_callback *entry = malloc(sizeof(*entry));
    if (!entry)
        return -1;
    entry->deadline = monotonic_ms() + (uint32_t)delay_ms;
    entry->callback = callback;
    entry->context = context;
    entry->next = scheduled;
    scheduled = entry;
    return 0;
}

int nsbuzz_schedule_run(void) {
    uint32_t now = monotonic_ms();
    for (;;) {
        int ran = 0;
        struct scheduled_callback **link = &scheduled;
        while (*link) {
            struct scheduled_callback *entry = *link;
            if (deadline_reached(now, entry->deadline)) {
                nsbuzz_callback callback = entry->callback;
                void *context = entry->context;
                *link = entry->next;
                free(entry);
                callback(context);
                ran = 1;
                break;
            }
            link = &entry->next;
        }
        if (!ran)
            break;
        now = monotonic_ms();
    }
    if (!scheduled)
        return -1;
    uint32_t delay = scheduled->deadline - now;
    for (struct scheduled_callback *entry = scheduled->next; entry; entry = entry->next) {
        uint32_t candidate = entry->deadline - now;
        if (candidate < delay)
            delay = candidate;
    }
    return delay > 0x7fffffffu ? 0 : (int)delay;
}

void nsbuzz_schedule_clear(void) {
    while (scheduled) {
        struct scheduled_callback *next = scheduled->next;
        free(scheduled);
        scheduled = next;
    }
}
