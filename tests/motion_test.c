#include <assert.h>
#include <stdio.h>
#include "ui_motion.h"

int main(void) {
    struct ui_motion m;
    ui_motion_reset(&m, 0);
    ui_motion_to(&m, 255, 200, 100);
    int previous = 0;
    for (uint32_t now = 100; now < 300; now++) {
        ui_motion_step(&m, now);
        assert(m.value >= previous && m.value <= 255);
        previous = m.value;
    }
    ui_motion_step(&m, 300);
    assert(m.value == 255 && !m.active);
    assert(!ui_motion_step(&m, 100000));

    /* Reversal can land exactly on the internally sampled value: callers
     * must publish that sample even though the channel is now inactive. */
    ui_motion_reset(&m, 0);
    ui_motion_to(&m, 88, 180, 0);
    ui_motion_step(&m, 32);
    assert(m.value == 39);
    ui_motion_to(&m, 44, 180, 38);
    assert(m.value == 44 && !m.active);
    assert(!ui_motion_step(&m, 54));
    ui_motion_reset(&m, 0);
    ui_motion_scroll(&m, 220, 1000, 180, 0);
    ui_motion_scroll(&m, 44, 1000, 180, 16);
    assert(m.to == 264); /* Same direction retains queued wheel distance. */
    ui_motion_step(&m, 32);
    int before_reverse = m.value;
    ui_motion_scroll(&m, -44, 1000, 180, 32);
    assert(m.value == before_reverse && m.to < before_reverse);
    ui_motion_step(&m, 48);
    assert(m.value < before_reverse);
    ui_motion_scroll(&m, -1000, 1000, 180, 48);
    assert(m.to == 0);
    ui_motion_step(&m, 228);
    assert(!m.active && m.value == 0);
    ui_motion_scroll(&m, 2000, 1000, 180, 300);
    assert(m.to == 1000);
    ui_motion_reset(&m, 255);

    ui_motion_to(&m, 0, 160, 1000);
    ui_motion_step(&m, 1060);
    int interrupted = m.value;
    ui_motion_to(&m, 255, 160, 1060);
    assert(m.value == interrupted && m.from == interrupted);
    uint32_t started = m.start_ms;
    ui_motion_to(&m, 255, 160, 1080);
    assert(m.start_ms == started); /* Repeated hover must not restart. */
    ui_motion_step(&m, 1220);
    assert(m.value == 255 && !m.active);

    ui_motion_reset(&m, 24);
    ui_motion_to(&m, 0, 32, UINT32_MAX - 15);
    ui_motion_step(&m, 0);
    assert(m.value > 0 && m.value < 24 && m.active);
    ui_motion_step(&m, 16);
    assert(m.value == 0 && !m.active);
    assert(ui_motion_reveal(20, 10, 20, 100) == 0);
    assert(ui_motion_reveal(30, 10, 20, 100) == 0);
    assert(ui_motion_reveal(130, 10, 20, 100) == 255);
    ui_motion_to(&m, 255, 0, 2000);
    assert(m.value == 255 && !m.active);
    puts("motion timing, reversal, deadlines and clock wrap passed");
    return 0;
}
