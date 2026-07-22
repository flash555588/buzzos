#include "libc.h"

enum { SAMPLE_RATE = 11025, BLOCK_SAMPLES = 315, BLOCKS = 105 };

int main(void) {
    uint8_t samples[BLOCK_SAMPLES];
    uint32_t phase = 0;
    int total = 0;

    if (audio_config(SAMPLE_RATE) < 0) return 1;
    puts("audiotest: starting 3 second PCM stream");
    for (int block = 0; block < BLOCKS; block++) {
        for (int i = 0; i < BLOCK_SAMPLES; i++) {
            samples[i] = phase < (SAMPLE_RATE / 880) ? 176 : 80;
            phase++;
            if (phase >= (SAMPLE_RATE / 440)) phase = 0;
        }
        int written = audio_write(samples, sizeof(samples));
        if (written < 0) {
            puts("audiotest: audio_write failed");
            return 1;
        }
        total += written;
        sleep_ms(28);
    }
    printf("audiotest: ok %d bytes\n", total);
    return 0;
}
