#include "appui.h"
#include "guiapp.h"
#include "libc.h"

enum { W = 560, H = 300, AUDIO_BLOCK = 4096 };
#define SAMPLE_PATH "/share/buzzos-demo.wav"

static uint8_t pixels[W * H];
static uint8_t *samples;
static uint32_t sample_count;
static volatile uint32_t position;
static volatile int playing = 1;
static volatile int closed;
static int playback_tid = -1;
static int previous_buttons;

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int load_sample(void) {
    struct stat st;
    if (stat(SAMPLE_PATH, &st) < 0 || st.st_size < 44)
        return -1;
    uint8_t *file = malloc(st.st_size);
    if (!file) return -1;
    int fd = open(SAMPLE_PATH, O_RDONLY);
    if (fd < 0) { free(file); return -1; }
    uint32_t got = 0;
    while (got < st.st_size) {
        int n = read(fd, file + got, st.st_size - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    close(fd);
    if (got != st.st_size || memcmp(file, "RIFF", 4) != 0 ||
        memcmp(file + 8, "WAVEfmt ", 8) != 0 || le16(file + 20) != 1 ||
        le16(file + 22) != 1 || le32(file + 24) != 11025 ||
        le16(file + 34) != 8 || memcmp(file + 36, "data", 4) != 0) {
        free(file);
        return -1;
    }
    uint32_t length = le32(file + 40);
    if (length > got - 44u) length = got - 44u;
    memmove(file, file + 44, length);
    samples = file;
    sample_count = length;
    return 0;
}

static void playback_thread(void) {
    int reported_done = 0;
    while (!closed) {
        if (!playing || position >= sample_count) {
            if (position >= sample_count) {
                playing = 0;
                if (!reported_done) {
                    puts("music: playback complete");
                    reported_done = 1;
                }
            } else {
                reported_done = 0;
            }
            sleep_ms(20);
            continue;
        }
        uint32_t left = sample_count - position;
        uint32_t want = left < AUDIO_BLOCK ? left : AUDIO_BLOCK;
        int n = audio_write(samples + position, want);
        if (n > 0) {
            position += (uint32_t)n;
            /* Keep the kernel FIFO prebuffered. AC97 DMA is the playback
             * clock; pacing here with BuzzOS's 10 ms sleep granularity
             * periodically starved an 11025 Hz stream. */
            yield();
        } else {
            sleep_ms(5);
        }
    }
}

static struct appui_rect play_button(void) {
    return (struct appui_rect){56, 196, 190, 44};
}

static struct appui_rect restart_button(void) {
    return (struct appui_rect){270, 196, 190, 44};
}

static void render(void) {
    appui_fill(pixels, W, H, (struct appui_rect){0, 0, W, H}, appui_gray(2));
    appui_text(pixels, W, H, 32, 28, "BuzzOS Music", 15, -1,
               (struct appui_rect){28, 20, W - 56, 38});
    appui_text(pixels, W, H, 20, 78, "Bundled AC97 playback sample", appui_gray(12), -1,
               (struct appui_rect){30, 72, W - 60, 28});
    appui_fill(pixels, W, H, (struct appui_rect){32, 126, W - 64, 22}, appui_gray(5));
    int progress = sample_count ? (int)((position * (W - 68u)) / sample_count) : 0;
    appui_fill(pixels, W, H, (struct appui_rect){34, 128, progress, 18}, appui_rgb6(1, 4, 5));
    const char *status = !samples ? "Invalid WAV" :
                         position >= sample_count ? "Finished - press Restart" :
                         playing ? "Playing" : "Paused";
    appui_text(pixels, W, H, 20, 158, status, 15, -1,
               (struct appui_rect){32, 154, W - 64, 28});
    appui_button(pixels, W, H, play_button(), playing ? "Pause" : "Play", 1);
    appui_button(pixels, W, H, restart_button(), "Restart", 0);
    appui_text(pixels, W, H, 16, 258, "Space: play/pause    R: restart", appui_gray(11), -1,
               (struct appui_rect){32, 252, W - 64, 24});
}

static void restart(void) {
    position = 0;
    playing = samples != 0;
}

int main(int argc, char **argv) {
    struct guiapp_ctx ctx;
    struct guiapp_event ev;
    if (guiapp_parse_args(argc, argv, &ctx) < 0) return 1;
    if (audio_config(11025) < 0) return 1;
    (void)load_sample();
    if (samples) {
        puts("music: playing /share/buzzos-demo.wav");
        playback_tid = spawn(playback_thread);
        if (playback_tid < 0) playing = 0;
    }
    for (;;) {
        if (guiapp_read_event(&ctx, &ev) < 0 || ev.type == GUIAPP_EVT_CLOSE)
            break;
        if (ev.type == GUIAPP_EVT_KEY && ev.buttons) {
            if (ev.key == ' ') playing = !playing && position < sample_count;
            else if (ev.key == 'r' || ev.key == 'R') restart();
        } else if (ev.type == GUIAPP_EVT_MOUSE) {
            int clicked = (ev.buttons & 1) && !(previous_buttons & 1);
            previous_buttons = ev.buttons;
            if (clicked && appui_inside(ev.x, ev.y, play_button()))
                playing = !playing && position < sample_count;
            else if (clicked && appui_inside(ev.x, ev.y, restart_button()))
                restart();
        }
        render();
        if (guiapp_send_frame(&ctx, "Music", W, H, pixels) < 0) break;
    }
    closed = 1;
    if (playback_tid >= 0) (void)join(playback_tid);
    free(samples);
    return 0;
}
