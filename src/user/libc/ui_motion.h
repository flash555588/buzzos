#ifndef BUZZOS_UI_MOTION_H
#define BUZZOS_UI_MOTION_H

#include <stdint.h>

/* Finite, integer-only motion. Unsigned elapsed time also handles clock wrap.
 * Retarget from the current sample so an interrupted transition never jumps. */
struct ui_motion {
    uint32_t start_ms, duration_ms;
    int from, to, value, active;
};

static inline int ui_motion_ease(uint32_t elapsed, uint32_t duration) {
    if (!duration || elapsed >= duration) return 1024;
    int remaining = 1024 - (int)((uint64_t)elapsed * 1024 / duration);
    return 1024 - (int)((int64_t)remaining * remaining * remaining / (1024 * 1024));
}

static inline int ui_motion_step(struct ui_motion *m, uint32_t now) {
    int old = m->value;
    if (!m->active) return 0;
    uint32_t elapsed = now - m->start_ms;
    if (elapsed >= m->duration_ms) {
        m->value = m->to;
        m->active = 0;
    } else {
        int ease = ui_motion_ease(elapsed, m->duration_ms);
        m->value = m->from + (int)((int64_t)(m->to - m->from) * ease / 1024);
    }
    return old != m->value;
}

static inline void ui_motion_reset(struct ui_motion *m, int value) {
    *m = (struct ui_motion){0, 0, value, value, value, 0};
}

static inline void ui_motion_to(struct ui_motion *m, int target,
                               uint32_t duration, uint32_t now) {
    if (m->active && m->to == target) return;
    ui_motion_step(m, now);
    if (!duration || m->value == target) {
        ui_motion_reset(m, target);
        return;
    }
    *m = (struct ui_motion){now, duration, m->value, target, m->value, 1};
}

static inline int ui_motion_reveal(uint32_t now, uint32_t start,
                                   uint32_t delay, uint32_t duration) {
    uint32_t elapsed = now - start;
    if (elapsed < delay) return 0;
    return ui_motion_ease(elapsed - delay, duration) * 255 / 1024;
}

/* Accumulate repeated wheel steps, but discard queued travel when the user
 * reverses direction. Otherwise an upward wheel event can still move down
 * towards the old, distant target. Callers publish value after retargeting. */
static inline void ui_motion_scroll(struct ui_motion *m, int delta, int limit,
                                    uint32_t duration, uint32_t now) {
    if (!delta) return;
    ui_motion_step(m, now);
    int remaining = m->to - m->value;
    int reversing = m->active && ((delta < 0 && remaining > 0) ||
                                 (delta > 0 && remaining < 0));
    int64_t target = (int64_t)(reversing ? m->value : m->to) + delta;
    if (target < 0) target = 0;
    if (target > limit) target = limit;
    ui_motion_to(m, (int)target, duration, now);
}

#endif
