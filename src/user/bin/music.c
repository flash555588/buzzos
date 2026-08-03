#include "appui.h"
#include "guiapp.h"
#include "libc.h"
#include "minimp3.h"

enum {
    MIN_W = 480,
    MIN_H = 320,
    DEFAULT_W = 720,
    DEFAULT_H = 410,
    AUDIO_BLOCK = 2048,
    PLAYBACK_LATENCY_MS = 80,
    DEFAULT_RATE = 11025,
    WAVE_BARS = 96,
    FRAME_MS = 25,
    /* Cap decoded PCM so a long track cannot exhaust the heap. */
    MAX_PCM_BYTES = 12 * 1024 * 1024,
    PATH_CAP = GUIAPP_PATH_MAX,
    TITLE_CAP = 48,
};

#define DEFAULT_WAV "/share/buzzos-demo.wav"
#define DEFAULT_MP3 "/share/buzzos-demo.mp3"

static struct guiapp_ctx gui;
static uint32_t *pixels;
static size_t pixels_cap;
static uint8_t *samples;
static uint32_t sample_count;
static unsigned sample_rate = DEFAULT_RATE;
static uint8_t wave_peak[WAVE_BARS];
static char track_title[TITLE_CAP];
static char track_artist[TITLE_CAP];
static char track_format[32];
static char track_path[PATH_CAP];
/* Write cursor: samples already submitted to the kernel FIFO (ahead of ears). */
static volatile uint32_t position;
static volatile int playing = 1;
static volatile int closed;
static volatile int win_w = DEFAULT_W;
static volatile int win_h = DEFAULT_H;
static int layout_w = DEFAULT_W;
static int layout_h = DEFAULT_H;
static volatile int mouse_x;
static volatile int mouse_y;
static volatile int prev_mouse_buttons;
static volatile int flush_audio;
static volatile int flush_absolute;
static volatile uint32_t flush_pos;
static volatile int playback_wake_sequence;
static volatile uint32_t ui_anchor_pos;
static volatile uint32_t ui_anchor_ms;
static volatile int ui_hold_pos_valid;
static volatile uint32_t ui_hold_pos;
static int playback_tid = -1;
static volatile int ui_dirty = 1;

static void invalidate_ui(void) {
    __sync_lock_test_and_set(&ui_dirty, 1);
    (void)futex_wake((int *)&ui_dirty, 1);
}

static void wake_playback(void) {
    __sync_add_and_fetch(&playback_wake_sequence, 1);
    (void)futex_wake((int *)&playback_wake_sequence, 1);
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void copy_text(char *dst, const char *src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static int ends_with_ci(const char *path, const char *ext) {
    size_t n, m, i;
    if (!path || !ext) return 0;
    n = strlen(path);
    m = strlen(ext);
    if (n < m) return 0;
    for (i = 0; i < m; i++) {
        char a = path[n - m + i];
        char b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

static void set_title_from_path(const char *path) {
    const char *base = path;
    const char *p;
    char name[TITLE_CAP];
    int i = 0;
    for (p = path; p && *p; p++)
        if (*p == '/')
            base = p + 1;
    while (base[i] && base[i] != '.' && i + 1 < (int)sizeof(name)) {
        name[i] = base[i];
        i++;
    }
    name[i] = 0;
    if (!name[0])
        copy_text(name, "Track", sizeof(name));
    copy_text(track_title, name, sizeof(track_title));
}

static void set_window_size(int w, int h) {
    int next_w = clamp_int(w, MIN_W, GUIAPP_MAX_W);
    int next_h = clamp_int(h, MIN_H, GUIAPP_MAX_H);
    if (win_w != next_w || win_h != next_h)
        invalidate_ui();
    win_w = next_w;
    win_h = next_h;
}

static void layout_sync_from_window(void) {
    layout_w = clamp_int(win_w, MIN_W, GUIAPP_MAX_W);
    layout_h = clamp_int(win_h, MIN_H, GUIAPP_MAX_H);
}

/* ---- responsive layout ---- */

static int layout_margin(void) { return clamp_int(layout_w / 36, 16, 40); }

static int layout_art_size(void) {
    int m = layout_margin();
    int s = layout_h / 3;
    s = clamp_int(s, 72, 140);
    if (s > layout_w / 3) s = layout_w / 3;
    if (s > layout_h - 2 * m - 180)
        s = clamp_int(layout_h - 2 * m - 180, 64, 140);
    return s;
}

static int layout_help_y(void) { return layout_h - 34; }
static int layout_button_h(void) { return clamp_int(layout_h / 10, 40, 52); }
static int layout_button_y(void) { return layout_help_y() - layout_button_h() - 14; }
static int layout_progress_y(void) { return layout_button_y() - 52; }
static int layout_header_h(void) {
    return layout_margin() + layout_art_size() + layout_margin() / 2;
}
static int layout_wave_y(void) { return layout_header_h() + 8; }
static int layout_wave_h(void) {
    int h = layout_progress_y() - 36 - layout_wave_y();
    return clamp_int(h, 40, 160);
}

static struct appui_rect art_rect(void) {
    int m = layout_margin();
    int s = layout_art_size();
    return (struct appui_rect){m, m, s, s};
}

static struct appui_rect progress_track(void) {
    int m = layout_margin();
    return (struct appui_rect){m, layout_progress_y(), layout_w - 2 * m, 12};
}

/* Transport: two square icon buttons flanking a wider primary pill,
 * centred as a group.  Both the painter and handle_click() read the rects from
 * here, so the glyphs and their hit boxes cannot drift apart. */
static void layout_buttons(struct appui_rect *restart, struct appui_rect *play,
                           struct appui_rect *stop) {
    int y = layout_button_y();
    int bh = layout_button_h();
    int m = layout_margin();
    int gap = clamp_int(layout_w / 48, 12, 24);
    int side = bh;
    int play_w = clamp_int(layout_w / 4, 110, 170);
    int total = 2 * side + play_w + 2 * gap;
    int x = (layout_w - total) / 2;
    if (x < m) x = m;
    *restart = (struct appui_rect){x, y, side, bh};
    *play = (struct appui_rect){x + side + gap, y, play_w, bh};
    *stop = (struct appui_rect){x + side + gap + play_w + gap, y, side, bh};
}

static struct appui_rect play_button(void) {
    struct appui_rect a, b, c;
    layout_buttons(&a, &b, &c);
    return b;
}
static struct appui_rect restart_button(void) {
    struct appui_rect a, b, c;
    layout_buttons(&a, &b, &c);
    return a;
}
static struct appui_rect stop_button(void) {
    struct appui_rect a, b, c;
    layout_buttons(&a, &b, &c);
    return c;
}

static void format_time(char *buf, int cap, uint32_t sample_pos) {
    uint32_t rate = sample_rate ? sample_rate : DEFAULT_RATE;
    uint32_t total_sec = sample_count ? sample_pos / rate : 0;
    uint32_t m = total_sec / 60u;
    uint32_t s = total_sec % 60u;
    if (m > 99u) m = 99u;
    snprintf(buf, (size_t)cap, "%u:%02u", (unsigned)m, (unsigned)s);
}

/* Progress/seek math: pos * width can exceed 2^32 around ~3 minutes at
 * 44.1 kHz (e.g. 8e6 * 600). Always multiply in 64-bit. */
static uint32_t scale_u32(uint32_t value, uint32_t numer, uint32_t denom) {
    if (!denom)
        return 0;
    return (uint32_t)(((uint64_t)value * (uint64_t)numer) / (uint64_t)denom);
}

static uint32_t queued_samples(void) {
    int q = audio_queued();
    return q > 0 ? (uint32_t)q : 0;
}

/* Heard sample index ≈ written - still-queued. Queue can briefly exceed the
 * write cursor around seeks (DMA estimate + old FIFO); never wrap that into
 * a false jump back to zero. */
static uint32_t hardware_playhead(void) {
    uint32_t written = position;
    if (written > sample_count)
        written = sample_count;

    /* While a flush/seek is in flight, trust the held target, not the queue. */
    if (flush_audio) {
        uint32_t held = ui_hold_pos_valid ? ui_hold_pos :
                        (flush_absolute ? flush_pos : written);
        if (held > sample_count)
            held = sample_count;
        return held;
    }

    uint32_t q = queued_samples();
    if (q > written)
        q = written;
    return written - q;
}

static void ui_resync(uint32_t pos) {
    if (pos > sample_count)
        pos = sample_count;
    ui_anchor_pos = pos;
    ui_anchor_ms = monotonic_ms();
    ui_hold_pos = pos;
}

static uint32_t display_playhead(void) {
    unsigned rate = sample_rate ? sample_rate : DEFAULT_RATE;
    if (ui_hold_pos_valid)
        return ui_hold_pos > sample_count ? sample_count : ui_hold_pos;

    uint32_t hw = hardware_playhead();
    uint32_t written = position;
    if (written > sample_count)
        written = sample_count;

    if (!playing) {
        ui_resync(hw);
        return hw;
    }

    uint32_t now = monotonic_ms();
    uint32_t elapsed_ms = now - ui_anchor_ms;
    uint32_t est = ui_anchor_pos +
                   (uint32_t)(((uint64_t)elapsed_ms * (uint64_t)rate) / 1000u);

    /* Catch up if we fell behind the device estimate. */
    if (est < hw) {
        ui_resync(hw);
        est = hw;
    }
    /* If the wall clock runs ahead of what we have submitted, cap to written.
     * Do NOT snap to a glitchy low hardware estimate — that caused the bar to
     * jump back to the start when queued > position around seeks. */
    if (est > written)
        est = written;
    if (est > sample_count)
        est = sample_count;

    /*
     * Never pull a running progress indicator backwards to chase a
     * period-granular hardware estimate. Seeks and pause explicitly resync
     * the anchor; normal playback may only hold or advance.
     */
    return est;
}

static void request_flush_absolute(uint32_t pos) {
    if (pos > sample_count)
        pos = sample_count;
    /* Hold UI at the seek target; write cursor is applied on the audio thread
     * so we never observe "new position + old FIFO" in the playhead math. */
    ui_hold_pos = pos;
    ui_hold_pos_valid = 1;
    ui_resync(pos);
    flush_pos = pos;
    flush_absolute = 1;
    flush_audio = 1;
    __sync_synchronize();
    wake_playback();
}

static void request_flush_rewind(void) {
    uint32_t hw = hardware_playhead();
    ui_hold_pos = hw;
    ui_hold_pos_valid = 1;
    ui_resync(hw);
    flush_absolute = 0;
    flush_audio = 1;
    __sync_synchronize();
    wake_playback();
}

static void apply_flush(void) {
    if (flush_absolute) {
        position = flush_pos;
        if (position > sample_count)
            position = sample_count;
    } else {
        uint32_t q = queued_samples();
        if (q > position)
            q = position;
        position -= q;
    }
    (void)audio_flush();
    ui_resync(position);
    ui_hold_pos = position;
    ui_hold_pos_valid = 0;
    flush_audio = 0;
}

static void build_waveform(void) {
    memset(wave_peak, 0, sizeof(wave_peak));
    if (!samples || !sample_count)
        return;
    uint32_t bucket = sample_count / WAVE_BARS;
    if (!bucket)
        bucket = 1;
    for (int i = 0; i < WAVE_BARS; i++) {
        uint32_t start = (uint32_t)i * bucket;
        uint32_t end = start + bucket;
        if (end > sample_count)
            end = sample_count;
        int peak = 0;
        for (uint32_t j = start; j < end; j++) {
            int d = samples[j] >= 128 ? (int)samples[j] - 128 : 128 - (int)samples[j];
            if (d > peak)
                peak = d;
        }
        wave_peak[i] = (uint8_t)peak;
    }
}

static unsigned pick_ac97_rate(unsigned hz) {
    if (hz == 11025u || hz == 22050u || hz == 44100u)
        return hz;
    if (hz <= 16000u)
        return 11025u;
    if (hz <= 32000u)
        return 22050u;
    return 44100u;
}

/* Nearest-neighbour resample of mono u8 into an AC97-supported rate. */
static int resample_mono_u8(uint8_t **io, uint32_t *count, unsigned src_hz, unsigned dst_hz) {
    uint8_t *in;
    uint8_t *out;
    uint64_t out_n;
    uint32_t i;
    if (!io || !*io || !count || !src_hz || !dst_hz)
        return -1;
    if (src_hz == dst_hz)
        return 0;
    in = *io;
    out_n = ((uint64_t)*count * (uint64_t)dst_hz + (uint64_t)src_hz - 1u) / (uint64_t)src_hz;
    if (out_n == 0 || out_n > MAX_PCM_BYTES)
        return -1;
    out = malloc((size_t)out_n);
    if (!out)
        return -1;
    for (i = 0; i < (uint32_t)out_n; i++) {
        uint32_t src = (uint32_t)(((uint64_t)i * (uint64_t)src_hz) / (uint64_t)dst_hz);
        if (src >= *count)
            src = *count - 1;
        out[i] = in[src];
    }
    free(in);
    *io = out;
    *count = (uint32_t)out_n;
    return 0;
}

static int append_u8(uint8_t **buf, uint32_t *len, uint32_t *cap, uint8_t sample) {
    if (*len >= *cap) {
        uint32_t ncap = *cap ? *cap * 2u : 65536u;
        uint8_t *nbuf;
        if (ncap > MAX_PCM_BYTES)
            ncap = MAX_PCM_BYTES;
        if (*len >= ncap)
            return -1;
        nbuf = realloc(*buf, ncap);
        if (!nbuf)
            return -1;
        *buf = nbuf;
        *cap = ncap;
    }
    (*buf)[(*len)++] = sample;
    return 0;
}

static int load_file_bytes(const char *path, uint8_t **out, uint32_t *out_size) {
    struct stat st;
    uint8_t *file;
    uint32_t got = 0;
    int fd;
    if (stat(path, &st) < 0 || st.st_size < 4 || st.st_size > 32 * 1024 * 1024)
        return -1;
    file = malloc(st.st_size);
    if (!file)
        return -1;
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        free(file);
        return -1;
    }
    while (got < st.st_size) {
        int n = read(fd, file + got, st.st_size - got);
        if (n <= 0)
            break;
        got += (uint32_t)n;
    }
    close(fd);
    if (got != st.st_size) {
        free(file);
        return -1;
    }
    *out = file;
    *out_size = got;
    return 0;
}

static int find_wav_chunk(const uint8_t *file, uint32_t size, const char *tag,
                          uint32_t *off, uint32_t *len) {
    uint32_t p = 12;
    while (p + 8 <= size) {
        uint32_t chunk_len = le32(file + p + 4);
        if (memcmp(file + p, tag, 4) == 0) {
            *off = p + 8;
            *len = chunk_len;
            if (*off + *len > size)
                *len = size - *off;
            return 0;
        }
        p += 8u + chunk_len + (chunk_len & 1u);
    }
    return -1;
}

static int decode_wav(const uint8_t *file, uint32_t size) {
    uint32_t fmt_off = 0, fmt_len = 0, data_off = 0, data_len = 0;
    uint16_t audio_fmt, channels, bits;
    uint32_t rate;
    uint8_t *pcm;
    uint32_t count = 0, cap = 0, i;

    if (size < 44 || memcmp(file, "RIFF", 4) != 0 || memcmp(file + 8, "WAVE", 4) != 0)
        return -1;
    if (find_wav_chunk(file, size, "fmt ", &fmt_off, &fmt_len) < 0 || fmt_len < 16)
        return -1;
    if (find_wav_chunk(file, size, "data", &data_off, &data_len) < 0 || !data_len)
        return -1;

    audio_fmt = le16(file + fmt_off);
    channels = le16(file + fmt_off + 2);
    rate = le32(file + fmt_off + 4);
    bits = le16(file + fmt_off + 14);
    if (audio_fmt != 1 || channels < 1 || channels > 2 || (bits != 8 && bits != 16))
        return -1;

    pcm = NULL;
    if (bits == 8) {
        uint32_t frames = data_len / channels;
        cap = frames;
        if (cap > MAX_PCM_BYTES)
            cap = MAX_PCM_BYTES;
        pcm = malloc(cap);
        if (!pcm)
            return -1;
        for (i = 0; i < frames && count < cap; i++) {
            if (channels == 1) {
                pcm[count++] = file[data_off + i];
            } else {
                unsigned a = file[data_off + i * 2];
                unsigned b = file[data_off + i * 2 + 1];
                pcm[count++] = (uint8_t)((a + b) / 2u);
            }
        }
    } else {
        uint32_t frames = data_len / (channels * 2u);
        cap = frames;
        if (cap > MAX_PCM_BYTES)
            cap = MAX_PCM_BYTES;
        pcm = malloc(cap);
        if (!pcm)
            return -1;
        for (i = 0; i < frames && count < cap; i++) {
            int32_t s;
            if (channels == 1) {
                s = (int16_t)le16(file + data_off + i * 2);
            } else {
                int16_t l = (int16_t)le16(file + data_off + i * 4);
                int16_t r = (int16_t)le16(file + data_off + i * 4 + 2);
                s = ((int32_t)l + (int32_t)r) / 2;
            }
            pcm[count++] = (uint8_t)((s + 32768) >> 8);
        }
    }

    {
        unsigned dst = pick_ac97_rate(rate);
        if (resample_mono_u8(&pcm, &count, rate, dst) < 0) {
            free(pcm);
            return -1;
        }
        sample_rate = dst;
    }
    samples = pcm;
    sample_count = count;
    snprintf(track_format, sizeof(track_format), "WAV  |  %u Hz  |  mono  |  8-bit",
             sample_rate);
    copy_text(track_artist, "PCM Wave", sizeof(track_artist));
    return 0;
}

static int decode_mp3(const uint8_t *file, uint32_t size) {
    mp3dec_t dec;
    mp3dec_frame_info_t info;
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    uint8_t *out = NULL;
    uint32_t out_len = 0, out_cap = 0;
    size_t off = 0;
    int src_hz = 0;
    int samples_i;

    /* Skip ID3v2 tag if present. */
    if (size >= 10 && file[0] == 'I' && file[1] == 'D' && file[2] == '3') {
        uint32_t tag = ((uint32_t)(file[6] & 0x7f) << 21) |
                       ((uint32_t)(file[7] & 0x7f) << 14) |
                       ((uint32_t)(file[8] & 0x7f) << 7) |
                       (uint32_t)(file[9] & 0x7f);
        off = 10u + tag;
        if (off > size)
            off = 0;
    }

    mp3dec_init(&dec);
    while (off < size) {
        samples_i = mp3dec_decode_frame(&dec, file + off, (int)(size - off), pcm, &info);
        if (info.frame_bytes <= 0) {
            off++;
            continue;
        }
        off += (size_t)info.frame_bytes;
        if (samples_i <= 0)
            continue;
        if (!src_hz) {
            src_hz = info.hz;
            if (src_hz <= 0)
                src_hz = 44100;
        }
        {
            int ch = info.channels > 0 ? info.channels : 1;
            int i;
            for (i = 0; i < samples_i; i++) {
                int32_t s;
                uint8_t u;
                if (ch >= 2)
                    s = ((int32_t)pcm[i * 2] + (int32_t)pcm[i * 2 + 1]) / 2;
                else
                    s = pcm[i];
                u = (uint8_t)((s + 32768) >> 8);
                if (append_u8(&out, &out_len, &out_cap, u) < 0) {
                    free(out);
                    return -1;
                }
            }
        }
    }

    if (!out || !out_len) {
        free(out);
        return -1;
    }

    {
        unsigned dst = pick_ac97_rate((unsigned)src_hz);
        if (resample_mono_u8(&out, &out_len, (unsigned)src_hz, dst) < 0) {
            free(out);
            return -1;
        }
        sample_rate = dst;
    }
    samples = out;
    sample_count = out_len;
    snprintf(track_format, sizeof(track_format), "MP3  |  %u Hz  |  mono  |  decoded",
             sample_rate);
    copy_text(track_artist, "MPEG Audio", sizeof(track_artist));
    return 0;
}

static int load_track(const char *path) {
    uint8_t *file = NULL;
    uint32_t size = 0;
    int rc = -1;

    free(samples);
    samples = NULL;
    sample_count = 0;
    position = 0;
    sample_rate = DEFAULT_RATE;
    copy_text(track_path, path ? path : "", sizeof(track_path));
    set_title_from_path(path ? path : "Track");
    copy_text(track_artist, "Unknown", sizeof(track_artist));
    copy_text(track_format, "No audio loaded", sizeof(track_format));

    if (!path || !path[0])
        return -1;
    if (load_file_bytes(path, &file, &size) < 0)
        return -1;

    if (ends_with_ci(path, ".mp3") ||
        (size >= 2 && file[0] == 0xFFu && (file[1] & 0xE0u) == 0xE0u) ||
        (size >= 3 && file[0] == 'I' && file[1] == 'D' && file[2] == '3'))
        rc = decode_mp3(file, size);
    if (rc < 0)
        rc = decode_wav(file, size);
    if (rc < 0 && !ends_with_ci(path, ".wav"))
        rc = decode_mp3(file, size);

    free(file);
    if (rc == 0)
        build_waveform();
    return rc;
}

static int choose_track(const char *arg) {
    struct stat st;
    if (arg && arg[0] && stat(arg, &st) == 0 && st.st_type == DT_REG)
        return load_track(arg);
    if (stat(DEFAULT_MP3, &st) == 0 && st.st_type == DT_REG)
        return load_track(DEFAULT_MP3);
    if (stat(DEFAULT_WAV, &st) == 0 && st.st_type == DT_REG)
        return load_track(DEFAULT_WAV);
    return -1;
}

static void playback_thread(void) {
    while (!closed) {
        if (flush_audio) {
            apply_flush();
            yield();
            continue;
        }
        if (!playing) {
            int expected = playback_wake_sequence;
            __sync_synchronize();
            if (!playing && !flush_audio && !closed)
                (void)futex_wait((int *)&playback_wake_sequence, expected);
            continue;
        }
        if (position >= sample_count) {
            if (queued_samples() == 0) {
                playing = 0;
                ui_resync(sample_count);
                invalidate_ui();
            }
            sleep_ms(10);
            continue;
        }

        uint32_t left = sample_count - position;
        uint32_t want = left < AUDIO_BLOCK ? left : AUDIO_BLOCK;
        int n = audio_write(samples + position, want);
        if (n > 0) {
            position += (uint32_t)n;
            yield();
        } else {
            yield();
            sleep_ms(1);
        }
    }
}

static void toggle_play(void) {
    if (!samples)
        return;
    if (playing) {
        playing = 0;
        request_flush_rewind();
    } else {
        if (hardware_playhead() >= sample_count || position >= sample_count) {
            position = 0;
            ui_resync(0);
        } else {
            ui_resync(hardware_playhead());
        }
        playing = 1;
        wake_playback();
    }
    invalidate_ui();
}

static void restart(void) {
    if (!samples)
        return;
    playing = 1;
    wake_playback();
    request_flush_absolute(0);
    invalidate_ui();
}

static void stop_playback(void) {
    playing = 0;
    request_flush_absolute(0);
    invalidate_ui();
}

static void seek_from_x(int x) {
    layout_sync_from_window();
    struct appui_rect bar = progress_track();
    if (!samples || sample_count == 0 || bar.w <= 0)
        return;
    int rel = clamp_int(x - bar.x, 0, bar.w);
    uint32_t pos = scale_u32((uint32_t)rel, sample_count, (uint32_t)bar.w);
    if (pos >= sample_count && sample_count > 0)
        pos = sample_count - 1;
    /* Do not poke `position` here: until the audio thread flushes the FIFO,
     * pairing a new write cursor with the old queue made the playhead wrap
     * to zero and the track appear to restart. */
    request_flush_absolute(pos);
    invalidate_ui();
}

static void handle_click(int x, int y) {
    layout_sync_from_window();
    if (appui_inside(x, y, play_button()))
        toggle_play();
    else if (appui_inside(x, y, restart_button()))
        restart();
    else if (appui_inside(x, y, stop_button()))
        stop_playback();
    else {
        struct appui_rect bar = progress_track();
        struct appui_rect hit = {bar.x - 6, bar.y - 12, bar.w + 12, bar.h + 24};
        if (appui_inside(x, y, hit))
            seek_from_x(x);
    }
}

static void gui_event_reader(void) {
    struct guiapp_event ev;
    while (!closed) {
        if (guiapp_read_event(&gui, &ev) < 0) {
            closed = 1;
            break;
        }
        if (ev.type == GUIAPP_EVT_CLOSE) {
            closed = 1;
            break;
        }
        if (ev.type == GUIAPP_EVT_INIT || ev.type == GUIAPP_EVT_RESIZE) {
            if (ev.width > 0 && ev.height > 0)
                set_window_size(ev.width, ev.height);
        } else if (ev.type == GUIAPP_EVT_KEY && ev.buttons) {
            if (ev.key == ' ')
                toggle_play();
            else if (ev.key == 'r' || ev.key == 'R')
                restart();
            else if (ev.key == 's' || ev.key == 'S')
                stop_playback();
        } else if (ev.type == GUIAPP_EVT_MOUSE) {
            if (mouse_x != ev.x || mouse_y != ev.y ||
                prev_mouse_buttons != ev.buttons)
                invalidate_ui();
            mouse_x = ev.x;
            mouse_y = ev.y;
            int clicked = (ev.buttons & 1) && !(prev_mouse_buttons & 1);
            prev_mouse_buttons = ev.buttons;
            if (clicked)
                handle_click(ev.x, ev.y);
            else if ((ev.buttons & 1) && samples) {
                layout_sync_from_window();
                struct appui_rect bar = progress_track();
                struct appui_rect hit = {bar.x - 6, bar.y - 12, bar.w + 12, bar.h + 24};
                if (appui_inside(ev.x, ev.y, hit))
                    seek_from_x(ev.x);
            }
        } else if (ev.type == GUIAPP_EVT_CAPABILITIES) {
            invalidate_ui();
        }
    }
    closed = 1;
    invalidate_ui();
    wake_playback();
}

static void draw_vbar(int w, int h, int x, int y, int bw, int bh, int fill_h,
                      uint32_t color) {
    struct ui_surface s = appui_surface(pixels, w, h);
    if (fill_h < 1) fill_h = 1;
    if (fill_h > bh) fill_h = bh;
    ui_fill_round(&s, ui_rect_make(x, y + bh - fill_h, bw, fill_h),
                  bw / 2, color);
}

static void draw_album_art(int w, int h) {
    struct appui_rect art = art_rect();
    struct ui_surface s = appui_surface(pixels, w, h);
    struct ui_rect box = appui_to_ui(art);
    int s_px = art.w;
    int cx = art.x + art.w / 2;
    int cy = art.y + art.h / 2;
    int max_r = s_px / 2 - 10;
    if (max_r < 16) max_r = 16;

    /* Elevated art card: a real soft shadow rather than an offset dark rect. */
    ui_shadow(&s, box, UI_RADIUS_OVERLAY, UI_ELEV_CARD_R, UI_ELEV_CARD_A, 2);
    ui_fill_round(&s, box, UI_RADIUS_OVERLAY, UI_ACCENT_DARK1);
    ui_fill_round(&s, ui_rect_inset(box, 3), UI_RADIUS_OVERLAY,
                  UI_BG_MICA_ALT);

    /* Vinyl grooves, accent at the rim easing to a neutral core. */
    for (int r = max_r; r >= 6; r -= 2) {
        uint32_t c = (r > max_r * 2 / 3) ? UI_ACCENT_FILL :
                     (r > max_r / 3 ? UI_ACCENT_BASE : UI_STROKE_SURFACE);
        ui_ring(&s, cx, cy, r, 1, c, 255);
    }
    {
        int hub = clamp_int(s_px / 12, 5, 8);
        int glyph = clamp_int(s_px / 3, 20, 48);
        ui_circle(&s, cx, cy, hub, UI_ACCENT_FILL, 255);
        ui_icon_in(&s, UI_ICON_MUSIC, box, glyph, UI_TEXT_PRIMARY, 255);
    }
}

static void draw_waveform(int w, int h, uint32_t pos) {
    int m = layout_margin();
    int base_x = m;
    int base_y = layout_wave_y();
    int area_w = w - 2 * m;
    int area_h = layout_wave_h();
    int bar_gap, bars, bar_w, play_bar;
    uint32_t tick;
    struct ui_surface surf = appui_surface(pixels, w, h);
    if (area_w < 40 || area_h < 24)
        return;

    bar_gap = area_w > 700 ? 2 : 1;
    bars = WAVE_BARS;
    bar_w = (area_w - bar_gap * (bars - 1)) / bars;
    if (bar_w < 2) {
        bar_w = 2;
        bars = (area_w + bar_gap) / (bar_w + bar_gap);
        if (bars < 8) bars = 8;
        if (bars > WAVE_BARS) bars = WAVE_BARS;
    }

    ui_fill_round(&surf,
                  ui_rect_make(base_x - 6, base_y - 6, area_w + 12,
                               area_h + 12),
                  UI_RADIUS_OVERLAY, UI_BG_LAYER);

    play_bar = 0;
    if (sample_count)
        play_bar = (int)scale_u32(pos, (uint32_t)bars, sample_count);
    if (play_bar >= bars)
        play_bar = bars - 1;

    tick = monotonic_ms();
    for (int i = 0; i < bars; i++) {
        int src = (i * WAVE_BARS) / bars;
        int peak, bh, x;
        uint32_t color;
        if (src >= WAVE_BARS) src = WAVE_BARS - 1;
        peak = wave_peak[src];
        bh = 6 + (peak * (area_h - 10)) / 128;
        if (playing && samples && i <= play_bar) {
            int pulse = (int)((tick / 40u + (uint32_t)i * 3u) % 7u);
            bh = clamp_int(bh + pulse - 3, 4, area_h);
        }
        x = base_x + i * (bar_w + bar_gap);
        if (!samples)
            color = UI_CTRL_DISABLED;
        else if (i < play_bar)
            color = UI_ACCENT_FILL;
        else if (i == play_bar)
            color = UI_TEXT_PRIMARY;
        else
            color = UI_STROKE_SURFACE;
        draw_vbar(w, h, x, base_y, bar_w, area_h, bh, color);
    }
}

/* Chrome label.  Callers still pass the old KFONT text origin, so the box is
 * derived from it and the glyphs are centred inside -- none of this is
 * character-cell layout, so the scaled UI sizes are safe here. */
static void draw_label_sz(int w, int h, int x, int y, const char *text,
                          uint32_t color, int max_w, int size) {
    if (max_w < 24) max_w = 24;
    if (x + max_w > w - 4)
        max_w = w - 4 - x;
    if (max_w < 8)
        return;
    appui_label(pixels, w, h,
                (struct appui_rect){x, y - 4, max_w, KFONT_HEIGHT},
                text, size, color, UI_ALIGN_LEFT);
}

static void draw_label(int w, int h, int x, int y, const char *text,
                       uint32_t color, int max_w) {
    draw_label_sz(w, h, x, y, text, color, max_w, UI_FONT_BODY);
}

static void draw_progress(int w, int h, uint32_t pos) {
    struct appui_rect track = progress_track();
    struct ui_surface s = appui_surface(pixels, w, h);
    char cur[16], tot[16];
    int fill_w = 0;
    int thumb_x;
    format_time(cur, sizeof(cur), pos);
    format_time(tot, sizeof(tot), sample_count);
    appui_label(pixels, w, h,
                (struct appui_rect){track.x, track.y - 32, track.w,
                                    KFONT_HEIGHT},
                cur, UI_FONT_CAPTION, UI_TEXT_SECONDARY, UI_ALIGN_LEFT);
    appui_label(pixels, w, h,
                (struct appui_rect){track.x, track.y - 32, track.w,
                                    KFONT_HEIGHT},
                tot, UI_FONT_CAPTION, UI_TEXT_SECONDARY, UI_ALIGN_RIGHT);

    /* Track and elapsed fill.  Same geometry the seek math inverts. */
    appui_progress(pixels, w, h, track,
                   sample_count ? (int)scale_u32(pos, (uint32_t)track.w,
                                                 sample_count)
                                : 0,
                   track.w);
    if (sample_count)
        fill_w = (int)scale_u32(pos, (uint32_t)track.w, sample_count);
    if (fill_w > track.w) fill_w = track.w;

    /* Slider thumb: an accent disc with a ring of the page colour so it
     * reads as floating above the track. */
    thumb_x = track.x + fill_w;
    if (thumb_x < track.x) thumb_x = track.x;
    if (thumb_x > track.x + track.w) thumb_x = track.x + track.w;
    ui_circle(&s, thumb_x, track.y + track.h / 2, 10, UI_BG_SOLID, 255);
    ui_circle(&s, thumb_x, track.y + track.h / 2, 8, UI_ACCENT_FILL, 255);
    ui_circle(&s, thumb_x, track.y + track.h / 2, 4, UI_BG_SOLID, 255);
}

/* Restart and stop carry glyphs; play/pause keeps a word because the icon set
 * has no play or pause glyph and a wrong one reads worse than the label. */
static void draw_transport(int w, int h, int is_playing) {
    struct appui_rect rb, pb, sb;
    int state;
    int enabled = samples != 0;

    layout_buttons(&rb, &pb, &sb);

    state = appui_pointer_state(rb, mouse_x, mouse_y, prev_mouse_buttons);
    if (!enabled) state |= APPUI_STATE_DISABLED;
    appui_icon_button(pixels, w, h, rb, UI_ICON_CHEVRON_LEFT, state);

    state = appui_pointer_state(pb, mouse_x, mouse_y, prev_mouse_buttons);
    if (!enabled) state |= APPUI_STATE_DISABLED;
    appui_button_ex(pixels, w, h, pb, is_playing ? "Pause" : "Play",
                    APPUI_BTN_PRIMARY, state);

    state = appui_pointer_state(sb, mouse_x, mouse_y, prev_mouse_buttons);
    if (!enabled) state |= APPUI_STATE_DISABLED;
    appui_icon_button(pixels, w, h, sb, UI_ICON_CLOSE, state);
}

static void render_frame(int *out_w, int *out_h) {
    layout_sync_from_window();
    int w = layout_w;
    int h = layout_h;
    if (appui_pixels_ensure(&pixels, &pixels_cap, w, h,
                            GUIAPP_MAX_W, GUIAPP_MAX_H) < 0) {
        if (out_w) *out_w = 0;
        if (out_h) *out_h = 0;
        return;
    }
    uint32_t pos = display_playhead();
    int is_playing = samples && playing && pos < sample_count;
    int m = layout_margin();
    int art = layout_art_size();
    int header_h = layout_header_h();
    int text_x, text_w;

    appui_fill(pixels, w, h, (struct appui_rect){0, 0, w, h}, UI_BG_SOLID);
    appui_fill(pixels, w, h, (struct appui_rect){0, 0, w, header_h},
               UI_BG_MICA);
    appui_separator(pixels, w, h, 0, header_h - 1, w, 0);

    draw_album_art(w, h);

    text_x = m + art + m;
    text_w = w - text_x - m;
    if (text_w < 80) {
        text_x = m;
        text_w = w - 2 * m;
        draw_label_sz(w, h, text_x, m + art + 8, track_title, UI_TEXT_PRIMARY,
                      text_w, UI_FONT_BODY_LG);
        draw_label(w, h, text_x, m + art + 36, track_artist,
                   UI_TEXT_SECONDARY, text_w);
    } else {
        int ty = m + 8;
        int line = clamp_int(art / 5, 28, 34);
        const char *status;
        uint32_t status_color;
        draw_label_sz(w, h, text_x, ty, track_title, UI_TEXT_PRIMARY, text_w,
                      UI_FONT_BODY_LG);
        draw_label(w, h, text_x, ty + line, track_artist, UI_TEXT_SECONDARY,
                   text_w);
        if (!samples) {
            status = "No audio loaded";
            status_color = UI_SYS_CRITICAL;
        } else if (pos >= sample_count) {
            status = "Finished";
            status_color = UI_TEXT_TERTIARY;
        } else if (is_playing) {
            status = "Now Playing";
            status_color = UI_SYS_SUCCESS;
        } else {
            status = "Paused";
            status_color = UI_SYS_CAUTION;
        }
        draw_label(w, h, text_x, ty + line * 2, status, status_color, text_w);
        if (art >= 100)
            draw_label_sz(w, h, text_x, ty + line * 3, track_format,
                          UI_TEXT_TERTIARY, text_w, UI_FONT_CAPTION);
    }

    if (text_w < 80) {
        const char *status = !samples ? "No audio" :
                             pos >= sample_count ? "Finished" :
                             is_playing ? "Playing" : "Paused";
        draw_label(w, h, m, layout_wave_y() - 26, status, UI_TEXT_SECONDARY,
                   w - 2 * m);
    }

    draw_waveform(w, h, pos);
    draw_progress(w, h, pos);
    draw_transport(w, h, is_playing);

    if (w >= 680)
        draw_label_sz(w, h, m, layout_help_y(),
                      "Space: play/pause   R: restart   S: stop   Click: seek",
                      UI_TEXT_TERTIARY, w - 2 * m, UI_FONT_CAPTION);
    else if (w >= 420)
        draw_label_sz(w, h, m, layout_help_y(),
                      "Space play/pause  R restart  S stop",
                      UI_TEXT_TERTIARY, w - 2 * m, UI_FONT_CAPTION);
    else
        draw_label_sz(w, h, m, layout_help_y(), "Space play/pause",
                      UI_TEXT_TERTIARY, w - 2 * m, UI_FONT_CAPTION);

    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

/* appui scales a 28-row glyph cell by ui_font_scale_pct and crops to the ink;
 * the GPU atlas scales the whole cell to the requested size.  A CPU font token
 * therefore maps to 28 * pct / 100 pixels, which is what keeps GPU text the
 * same size as the CPU surface it can be swapped for at any frame. */
enum {
    GPU_FONT_CAPTION = 16,  /* UI_FONT_CAPTION,  58% */
    GPU_FONT_BODY = 19,     /* UI_FONT_BODY,     68% */
    GPU_FONT_BODY_LG = 22,  /* UI_FONT_BODY_LG,  79% */
};

static void canvas_box(struct guiapp_canvas *canvas, struct appui_rect r,
                       int radius, uint32_t color) {
    (void)guiapp_canvas_rect(canvas, r.x, r.y, r.w, r.h, radius, color);
}

/* appui centres the glyph ink in its box; the GPU centres the full cell, whose
 * descender space sits below the ink.  Growing a tight box to the cell height
 * keeps the same optical centre and stops the GPU scissor from shearing the
 * bottom off large text. */
static void canvas_label(struct guiapp_canvas *canvas, struct appui_rect r,
                         const char *text, int size, uint32_t color,
                         uint16_t flags) {
    if (r.h < size) {
        r.y -= (size - r.h) / 2;
        r.h = size;
    }
    (void)guiapp_canvas_text(canvas, r.x, r.y, r.w, r.h, text, size,
                             color, flags);
}

static uint32_t canvas_button_color(struct appui_rect r, int primary,
                                    int enabled) {
    int state = appui_pointer_state(r, mouse_x, mouse_y, prev_mouse_buttons);
    if (!enabled)
        return UI_CTRL_DISABLED;
    if (state & APPUI_STATE_PRESSED)
        return primary ? UI_ACCENT_DARK1 : UI_SUBTLE_PRESSED;
    if (state & APPUI_STATE_HOVERED)
        return primary ? UI_ACCENT_LIGHT1 : UI_SUBTLE_HOVER;
    return primary ? UI_ACCENT_FILL : UI_BG_LAYER;
}

/* GPU-native Music surface.  This emits ~85 compact primitives; it never
 * allocates or walks a width*height CPU framebuffer. */
static int render_canvas_frame(void) {
    enum { GPU_WAVE_BARS = 56 };
    struct guiapp_canvas canvas;
    struct appui_rect art, track, restart_btn, play_btn, stop_btn;
    char cur[16], tot[16];
    const char *status;
    uint32_t status_color;
    int w, h, m, art_size, header_h, text_x, text_w;
    uint32_t pos;
    int is_playing;

    layout_sync_from_window();
    w = layout_w;
    h = layout_h;
    pos = display_playhead();
    is_playing = samples && playing && pos < sample_count;
    m = layout_margin();
    art_size = layout_art_size();
    header_h = layout_header_h();
    if (guiapp_canvas_begin(&gui, &canvas, w, h) < 0)
        return -1;

    canvas_box(&canvas, (struct appui_rect){0, 0, w, h}, 0, UI_BG_SOLID);
    canvas_box(&canvas, (struct appui_rect){0, 0, w, header_h}, 0,
               UI_BG_MICA);
    canvas_box(&canvas, (struct appui_rect){0, header_h - 1, w, 1}, 0,
               UI_STROKE_SURFACE);

    art = art_rect();
    canvas_box(&canvas, art, UI_RADIUS_OVERLAY, UI_ACCENT_DARK1);
    canvas_box(&canvas,
               (struct appui_rect){art.x + 3, art.y + 3,
                                   art.w - 6, art.h - 6},
               UI_RADIUS_OVERLAY - 2, UI_BG_MICA_ALT);
    {
        int disc = art.w - 20;
        int dx = art.x + (art.w - disc) / 2;
        int dy = art.y + (art.h - disc) / 2;
        canvas_box(&canvas, (struct appui_rect){dx, dy, disc, disc},
                   disc / 2, UI_ACCENT_BASE);
        canvas_box(&canvas,
                   (struct appui_rect){dx + disc / 7, dy + disc / 7,
                                       disc - 2 * (disc / 7),
                                       disc - 2 * (disc / 7)},
                   disc / 2, UI_BG_MICA_ALT);
        canvas_box(&canvas,
                   (struct appui_rect){dx + disc / 3, dy + disc / 3,
                                       disc / 3, disc / 3},
                   disc / 6, UI_ACCENT_FILL);
        canvas_label(&canvas, (struct appui_rect){dx, dy, disc, disc},
                     "Music", clamp_int(disc / 7, 10, 18),
                     UI_TEXT_PRIMARY, GUIAPP_CANVAS_ALIGN_CENTER |
                                      GUIAPP_CANVAS_TEXT_BOLD);
    }

    text_x = m + art_size + m;
    text_w = w - text_x - m;
    if (text_w < 80) {
        text_x = m;
        text_w = w - 2 * m;
    }
    canvas_label(&canvas,
                 (struct appui_rect){text_x, m + 4, text_w, 30},
                 track_title, GPU_FONT_BODY_LG, UI_TEXT_PRIMARY,
                 GUIAPP_CANVAS_ALIGN_LEFT | GUIAPP_CANVAS_TEXT_BOLD);
    canvas_label(&canvas,
                 (struct appui_rect){text_x, m + 34, text_w, 24},
                 track_artist, GPU_FONT_BODY, UI_TEXT_SECONDARY,
                 GUIAPP_CANVAS_ALIGN_LEFT);
    if (!samples) {
        status = "No audio loaded";
        status_color = UI_SYS_CRITICAL;
    } else if (pos >= sample_count) {
        status = "Finished";
        status_color = UI_TEXT_TERTIARY;
    } else if (is_playing) {
        status = "Now Playing";
        status_color = UI_SYS_SUCCESS;
    } else {
        status = "Paused";
        status_color = UI_SYS_CAUTION;
    }
    canvas_label(&canvas,
                 (struct appui_rect){text_x, m + 62, text_w, 24},
                 status, GPU_FONT_BODY, status_color,
                 GUIAPP_CANVAS_ALIGN_LEFT);
    if (text_w >= 80 && art_size >= 100)
        canvas_label(&canvas,
                     (struct appui_rect){text_x, m + 88, text_w, 22},
                     track_format, GPU_FONT_CAPTION, UI_TEXT_TERTIARY,
                     GUIAPP_CANVAS_ALIGN_LEFT);

    {
        int base_x = m;
        int base_y = layout_wave_y();
        int area_w = w - 2 * m;
        int area_h = layout_wave_h();
        int gap = 2;
        int bar_w = (area_w - gap * (GPU_WAVE_BARS - 1)) /
                    GPU_WAVE_BARS;
        int play_bar = sample_count
            ? (int)scale_u32(pos, GPU_WAVE_BARS, sample_count) : 0;
        uint32_t now = monotonic_ms();
        if (bar_w < 2) { bar_w = 2; gap = 1; }
        if (play_bar >= GPU_WAVE_BARS) play_bar = GPU_WAVE_BARS - 1;
        canvas_box(&canvas,
                   (struct appui_rect){base_x - 6, base_y - 6,
                                       area_w + 12, area_h + 12},
                   UI_RADIUS_OVERLAY, UI_BG_LAYER);
        for (int i = 0; i < GPU_WAVE_BARS; i++) {
            int src = i * WAVE_BARS / GPU_WAVE_BARS;
            int bh = 6 + wave_peak[src] * (area_h - 10) / 128;
            uint32_t color = !samples ? UI_CTRL_DISABLED :
                i < play_bar ? UI_ACCENT_FILL :
                i == play_bar ? UI_TEXT_PRIMARY : UI_STROKE_SURFACE;
            if (is_playing && i <= play_bar)
                bh = clamp_int(bh + (int)((now / 40u + i * 3u) % 7u) - 3,
                               4, area_h);
            canvas_box(&canvas,
                       (struct appui_rect){base_x + i * (bar_w + gap),
                                           base_y + area_h - bh,
                                           bar_w, bh},
                       bar_w / 2, color);
        }
    }

    track = progress_track();
    format_time(cur, sizeof(cur), pos);
    format_time(tot, sizeof(tot), sample_count);
    canvas_label(&canvas,
                 (struct appui_rect){track.x, track.y - 30,
                                     track.w / 2, 22},
                 cur, GPU_FONT_CAPTION, UI_TEXT_SECONDARY,
                 GUIAPP_CANVAS_ALIGN_LEFT);
    canvas_label(&canvas,
                 (struct appui_rect){track.x + track.w / 2, track.y - 30,
                                     track.w - track.w / 2, 22},
                 tot, GPU_FONT_CAPTION, UI_TEXT_SECONDARY,
                 GUIAPP_CANVAS_ALIGN_RIGHT);
    canvas_box(&canvas, track, track.h / 2, UI_STROKE_CONTROL);
    {
        int fill_w = sample_count
            ? (int)scale_u32(pos, (uint32_t)track.w, sample_count) : 0;
        int thumb_x;
        if (fill_w > track.w) fill_w = track.w;
        if (fill_w > 0)
            canvas_box(&canvas,
                       (struct appui_rect){track.x, track.y, fill_w, track.h},
                       track.h / 2, UI_ACCENT_FILL);
        thumb_x = track.x + fill_w;
        canvas_box(&canvas,
                   (struct appui_rect){thumb_x - 9,
                                       track.y + track.h / 2 - 9, 18, 18},
                   9, UI_BG_SOLID);
        canvas_box(&canvas,
                   (struct appui_rect){thumb_x - 7,
                                       track.y + track.h / 2 - 7, 14, 14},
                   7, UI_ACCENT_FILL);
    }

    layout_buttons(&restart_btn, &play_btn, &stop_btn);
    canvas_box(&canvas, restart_btn, UI_RADIUS_CONTROL,
               canvas_button_color(restart_btn, 0, samples != 0));
    canvas_label(&canvas, restart_btn, "<<", GPU_FONT_BODY, UI_TEXT_PRIMARY,
                 GUIAPP_CANVAS_ALIGN_CENTER | GUIAPP_CANVAS_TEXT_BOLD);
    canvas_box(&canvas, play_btn, play_btn.h / 2,
               canvas_button_color(play_btn, 1, samples != 0));
    canvas_label(&canvas, play_btn, is_playing ? "Pause" : "Play",
                 GPU_FONT_BODY, UI_TEXT_ON_ACCENT,
                 GUIAPP_CANVAS_ALIGN_CENTER | GUIAPP_CANVAS_TEXT_BOLD);
    canvas_box(&canvas, stop_btn, UI_RADIUS_CONTROL,
               canvas_button_color(stop_btn, 0, samples != 0));
    canvas_label(&canvas, stop_btn, "X", GPU_FONT_BODY, UI_TEXT_PRIMARY,
                 GUIAPP_CANVAS_ALIGN_CENTER | GUIAPP_CANVAS_TEXT_BOLD);

    canvas_label(&canvas,
                 (struct appui_rect){m, layout_help_y(), w - 2 * m, 22},
                 w >= 680
                    ? "Space: play/pause   R: restart   S: stop   Click: seek"
                    : "Space play/pause   R restart   S stop",
                 GPU_FONT_CAPTION, UI_TEXT_TERTIARY,
                 GUIAPP_CANVAS_ALIGN_LEFT);
    return guiapp_canvas_present(&canvas, "Music");
}

int main(int argc, char **argv) {
    const char *path_arg = (argc > 4 && argv[4] && argv[4][0]) ? argv[4] : 0;
    int exit_status = 0;
    int gpu_mode;

    if (guiapp_parse_args(argc, argv, &gui) < 0)
        return 1;

    if (choose_track(path_arg) < 0)
        playing = 0;

    if (audio_config_latency(sample_rate, PLAYBACK_LATENCY_MS) < 0)
        return 1;

    ui_resync(0);

    if (samples) {
        playback_tid = spawn(playback_thread);
        if (playback_tid < 0)
            playing = 0;
    }

    if (spawn(gui_event_reader) < 0) {
        closed = 1;
        exit_status = 1;
    }

    gpu_mode = guiapp_has_capability(&gui, GUIAPP_CAP_GPU_CANVAS);
    {
        int w = 0, h = 0;
        if (gpu_mode) {
            if (render_canvas_frame() < 0)
                closed = 1;
        } else {
            render_frame(&w, &h);
            if (guiapp_send_frame(&gui, "Music", w, h, pixels) < 0)
                closed = 1;
        }
        __sync_lock_test_and_set(&ui_dirty, 0);
    }

    while (!closed) {
        int next_gpu_mode =
            guiapp_has_capability(&gui, GUIAPP_CAP_GPU_CANVAS);
        if (next_gpu_mode != gpu_mode) {
            gpu_mode = next_gpu_mode;
            invalidate_ui();
        }
        if (!playing && !ui_dirty) {
            (void)futex_wait((int *)&ui_dirty, 0);
            continue;
        }
        int w = 0, h = 0;
        __sync_lock_test_and_set(&ui_dirty, 0);
        int sent = gpu_mode
            ? render_canvas_frame()
            : (render_frame(&w, &h),
               guiapp_send_frame(&gui, "Music", w, h, pixels));
        if (sent < 0) {
            closed = 1;
            break;
        }
        sleep_ms(playing ? FRAME_MS : 10);
    }

    closed = 1;
    wake_playback();
    if (playback_tid >= 0)
        (void)join(playback_tid);
    (void)audio_flush();
    free(samples);
    free(pixels);
    return exit_status;
}
