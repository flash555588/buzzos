#include "audio.h"
#include "ac97.h"
#include "hda.h"
#include "serial.h"
#include "task.h"
#include "timer.h"

struct audio_ops {
    const char *name;
    int (*init)(void);
    int (*write)(const uint8_t *data, size_t size);
    int (*set_rate)(uint32_t rate, uint32_t latency_ms);
    int (*flush)(void);
    int (*queued_samples)(void);
    void (*poll)(void);
};

static const struct audio_ops drivers[] = {
    {
        .name = "intel-hda",
        .init = hda_init,
        .write = hda_write,
        .set_rate = hda_set_rate,
        .flush = hda_flush,
        .queued_samples = hda_queued_samples,
        .poll = hda_poll,
    },
    {
        .name = "intel-ac97",
        .init = ac97_init,
        .write = ac97_write,
        .set_rate = ac97_set_rate,
        .flush = ac97_flush,
        .queued_samples = ac97_queued_samples,
    },
};

static const struct audio_ops *active;
static int worker_task = -1;

static void audio_worker(void) {
    for (;;) {
        if (active && active->poll)
            active->poll();
        task_sleep_until(timer_ticks() + 1u);
    }
}

int audio_init(void) {
    active = 0;
    for (uint32_t i = 0; i < sizeof(drivers) / sizeof(drivers[0]); i++) {
        if (drivers[i].init() == 0) {
            active = &drivers[i];
            serial_puts("[audio] selected driver: ");
            serial_puts(active->name);
            serial_puts("\n");
            return 0;
        }
    }
    serial_puts("[audio] no supported device\n");
    return -1;
}

int audio_start_worker(void) {
    if (!active || !active->poll)
        return 0;
    if (worker_task >= 0)
        return 0;
    worker_task = task_create_ex(audio_worker, "audio-worker", 1);
    if (worker_task < 0) {
        serial_puts("[audio] failed to create worker\n");
        return -1;
    }
    task_make_ready(worker_task);
    serial_puts("[audio] deferred worker started\n");
    return 0;
}

int audio_write(const uint8_t *data, size_t size) {
    return active ? active->write(data, size) : -1;
}

int audio_set_rate(uint32_t rate, uint32_t latency_ms) {
    return active ? active->set_rate(rate, latency_ms) : -1;
}

int audio_flush(void) {
    return active && active->flush ? active->flush() : -1;
}

int audio_queued_samples(void) {
    return active ? active->queued_samples() : -1;
}

const char *audio_driver_name(void) {
    return active ? active->name : "none";
}
