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
static uint8_t pixels[GUIAPP_MAX_W * GUIAPP_MAX_H];
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
static volatile uint32_t ui_anchor_pos;
static volatile uint32_t ui_anchor_ms;
static volatile int ui_hold_pos_valid;
static volatile uint32_t ui_hold_pos;
static int playback_tid = -1;

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
    win_w = clamp_int(w, MIN_W, GUIAPP_MAX_W);
    win_h = clamp_int(h, MIN_H, GUIAPP_MAX_H);
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

static void layout_buttons(struct appui_rect *restart, struct appui_rect *play,
                           struct appui_rect *stop) {
    int y = layout_button_y();
    int bh = layout_button_h();
    int m = layout_margin();
    int gap = clamp_int(layout_w / 48, 12, 24);
    int total = layout_w - 2 * m;
    int play_w = clamp_int(total / 3, 120, 180);
    int side_w = (total - play_w - 2 * gap) / 2;
    if (side_w < 96) {
        side_w = 96;
        play_w = total - 2 * side_w - 2 * gap;
        if (play_w < 100) play_w = 100;
    }
    *restart = (struct appui_rect){m, y, side_w, bh};
    *play = (struct appui_rect){m + side_w + gap, y, play_w, bh};
    *stop = (struct appui_rect){m + side_w + gap + play_w + gap, y, side_w, bh};
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
}

static void request_flush_rewind(void) {
    uint32_t hw = hardware_playhead();
    ui_hold_pos = hw;
    ui_hold_pos_valid = 1;
    ui_resync(hw);
    flush_absolute = 0;
    flush_audio = 1;
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
            sleep_ms(10);
            continue;
        }
        if (position >= sample_count) {
            if (queued_samples() == 0) {
                playing = 0;
                ui_resync(sample_count);
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
    }
}

static void restart(void) {
    if (!samples)
        return;
    playing = 1;
    request_flush_absolute(0);
}

static void stop_playback(void) {
    playing = 0;
    request_flush_absolute(0);
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
        }
    }
    closed = 1;
}

static void draw_vbar(int w, int h, int x, int y, int bw, int bh, int fill_h, int color) {
    if (fill_h < 1) fill_h = 1;
    if (fill_h > bh) fill_h = bh;
    appui_fill(pixels, w, h, (struct appui_rect){x, y + bh - fill_h, bw, fill_h}, color);
}

static void draw_album_art(int w, int h) {
    struct appui_rect art = art_rect();
    int accent = THEME_ACCENT;
    int soft = THEME_ACCENT_SOFT;
    int panel = appui_gray(1);
    int s = art.w;
    int cx = art.x + art.w / 2;
    int cy = art.y + art.h / 2;
    int max_r = s / 2 - 10;
    if (max_r < 16) max_r = 16;

    appui_fill_round(pixels, w, h,
                     (struct appui_rect){art.x + 4, art.y + 6, art.w, art.h}, appui_gray(0));
    appui_fill_round(pixels, w, h, art, soft);
    appui_fill_round(pixels, w, h,
                     (struct appui_rect){art.x + 3, art.y + 3, art.w - 6, art.h - 6}, panel);

    for (int r = max_r; r >= 6; r -= 2) {
        int c = (r > max_r * 2 / 3) ? accent :
                (r > max_r / 3 ? THEME_ACCENT_DIM : appui_gray(4));
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                int d2 = dx * dx + dy * dy;
                if (d2 <= r * r && d2 >= (r - 2) * (r - 2))
                    appui_pixel(pixels, w, h, cx + dx, cy + dy, c);
            }
        }
    }
    {
        int hub = clamp_int(s / 12, 5, 8);
        int stem_h = clamp_int(s / 4, 18, 32);
        int note_w = clamp_int(s / 10, 10, 16);
        appui_fill_round(pixels, w, h,
                         (struct appui_rect){cx - hub, cy - hub, hub * 2, hub * 2}, accent);
        appui_fill(pixels, w, h, (struct appui_rect){cx + 2, cy - stem_h + 4, 3, stem_h}, 15);
        appui_fill(pixels, w, h, (struct appui_rect){cx + 2, cy - stem_h + 4, note_w, 3}, 15);
        appui_fill(pixels, w, h,
                   (struct appui_rect){cx + note_w - 1, cy - stem_h + 4, 3, stem_h / 3}, 15);
        appui_fill_round(pixels, w, h,
                         (struct appui_rect){cx - note_w / 2, cy + 2, note_w + 2, note_w - 2}, 15);
        appui_fill_round(pixels, w, h,
                         (struct appui_rect){cx + note_w - 4, cy - stem_h / 2, note_w, note_w - 4}, 15);
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

    appui_fill_round(pixels, w, h,
                     (struct appui_rect){base_x - 6, base_y - 6, area_w + 12, area_h + 12},
                     appui_gray(1));

    play_bar = 0;
    if (sample_count)
        play_bar = (int)scale_u32(pos, (uint32_t)bars, sample_count);
    if (play_bar >= bars)
        play_bar = bars - 1;

    tick = monotonic_ms();
    for (int i = 0; i < bars; i++) {
        int src = (i * WAVE_BARS) / bars;
        int peak, bh, x, color;
        if (src >= WAVE_BARS) src = WAVE_BARS - 1;
        peak = wave_peak[src];
        bh = 6 + (peak * (area_h - 10)) / 128;
        if (playing && samples && i <= play_bar) {
            int pulse = (int)((tick / 40u + (uint32_t)i * 3u) % 7u);
            bh = clamp_int(bh + pulse - 3, 4, area_h);
        }
        x = base_x + i * (bar_w + bar_gap);
        if (!samples)
            color = appui_gray(4);
        else if (i < play_bar)
            color = THEME_ACCENT;
        else if (i == play_bar)
            color = 15;
        else
            color = appui_gray(5);
        draw_vbar(w, h, x, base_y, bar_w, area_h, bh, color);
    }
}

static void draw_label(int w, int h, int x, int y, const char *text, int color, int max_w) {
    if (max_w < 24) max_w = 24;
    if (x + max_w > w - 4)
        max_w = w - 4 - x;
    if (max_w < 8)
        return;
    appui_text(pixels, w, h, x, y, text, color, -1,
               (struct appui_rect){x - 2, y - 8, max_w + 4, 32});
}

static void draw_progress(int w, int h, uint32_t pos) {
    struct appui_rect track = progress_track();
    char cur[16], tot[16];
    int fill_w = 0;
    int thumb_x;
    format_time(cur, sizeof(cur), pos);
    format_time(tot, sizeof(tot), sample_count);
    draw_label(w, h, track.x, track.y - 28, cur, THEME_TEXT_DIM, 96);
    {
        int tw = appui_text_width(tot);
        draw_label(w, h, track.x + track.w - tw, track.y - 28, tot, THEME_TEXT_DIM, tw + 8);
    }
    appui_fill_round(pixels, w, h, track, appui_gray(4));
    if (sample_count)
        fill_w = (int)scale_u32(pos, (uint32_t)track.w, sample_count);
    if (fill_w > track.w) fill_w = track.w;
    if (fill_w > 0) {
        appui_fill_round(pixels, w, h,
                         (struct appui_rect){track.x, track.y, fill_w, track.h}, THEME_ACCENT);
        if (fill_w > 4)
            appui_fill(pixels, w, h,
                       (struct appui_rect){track.x + 2, track.y + 1, fill_w - 4, 3},
                       THEME_ACCENT_DIM);
    }
    thumb_x = track.x + fill_w - 7;
    if (thumb_x < track.x) thumb_x = track.x;
    if (thumb_x > track.x + track.w - 14) thumb_x = track.x + track.w - 14;
    appui_fill_round(pixels, w, h,
                     (struct appui_rect){thumb_x, track.y - 5, 14, track.h + 10}, 15);
    appui_fill_round(pixels, w, h,
                     (struct appui_rect){thumb_x + 2, track.y - 3, 10, track.h + 6},
                     THEME_ACCENT);
}

static void draw_icon_button(int w, int h, struct appui_rect r, const char *label, int primary) {
    int hover = appui_inside(mouse_x, mouse_y, r);
    int edge = primary ? THEME_ACCENT : (hover ? appui_gray(7) : appui_gray(0));
    int bg = primary ? (hover ? THEME_ACCENT : THEME_ACCENT_DIM)
                     : (hover ? appui_gray(4) : THEME_WIN_CONTROL);
    int tw = appui_text_width(label);
    int tx = r.x + (r.w - tw) / 2;
    int ty = r.y + (r.h - 16) / 2;
    appui_fill_round(pixels, w, h, r, edge);
    appui_fill_round(pixels, w, h,
                     (struct appui_rect){r.x + 1, r.y + 1, r.w - 2, r.h - 2}, bg);
    appui_text(pixels, w, h, tx, ty, label, THEME_TEXT, -1,
               (struct appui_rect){r.x + 2, r.y + 2, r.w - 4, r.h - 4});
}

static void render_frame(int *out_w, int *out_h) {
    layout_sync_from_window();
    int w = layout_w;
    int h = layout_h;
    uint32_t pos = display_playhead();
    int is_playing = samples && playing && pos < sample_count;
    int m = layout_margin();
    int art = layout_art_size();
    int header_h = layout_header_h();
    int text_x, text_w;

    appui_fill(pixels, w, h, (struct appui_rect){0, 0, w, h}, appui_gray(0));
    appui_fill(pixels, w, h, (struct appui_rect){0, 0, w, header_h}, appui_gray(1));
    appui_fill(pixels, w, h, (struct appui_rect){0, header_h - 2, w, 2}, appui_gray(3));

    draw_album_art(w, h);

    text_x = m + art + m;
    text_w = w - text_x - m;
    if (text_w < 80) {
        text_x = m;
        text_w = w - 2 * m;
        draw_label(w, h, text_x, m + art + 8, track_title, 15, text_w);
        draw_label(w, h, text_x, m + art + 36, track_artist, THEME_TEXT_DIM, text_w);
    } else {
        int ty = m + 8;
        int line = clamp_int(art / 5, 28, 34);
        const char *status;
        int status_color;
        draw_label(w, h, text_x, ty, track_title, 15, text_w);
        draw_label(w, h, text_x, ty + line, track_artist, THEME_TEXT_DIM, text_w);
        if (!samples) {
            status = "No audio loaded";
            status_color = THEME_CLOSE_RED;
        } else if (pos >= sample_count) {
            status = "Finished";
            status_color = THEME_TEXT_FAINT;
        } else if (is_playing) {
            status = "Now Playing";
            status_color = THEME_MAX_GREEN;
        } else {
            status = "Paused";
            status_color = THEME_MIN_YELLOW;
        }
        draw_label(w, h, text_x, ty + line * 2, status, status_color, text_w);
        if (art >= 100)
            draw_label(w, h, text_x, ty + line * 3, track_format, THEME_TEXT_FAINT, text_w);
    }

    if (text_w < 80) {
        const char *status = !samples ? "No audio" :
                             pos >= sample_count ? "Finished" :
                             is_playing ? "Playing" : "Paused";
        draw_label(w, h, m, layout_wave_y() - 26, status, THEME_TEXT_DIM, w - 2 * m);
    }

    draw_waveform(w, h, pos);
    draw_progress(w, h, pos);

    {
        struct appui_rect rb, pb, sb;
        layout_buttons(&rb, &pb, &sb);
        draw_icon_button(w, h, rb, "Restart", 0);
        draw_icon_button(w, h, pb, is_playing ? "Pause" : "Play", 1);
        draw_icon_button(w, h, sb, "Stop", 0);
    }

    if (w >= 560)
        draw_label(w, h, m, layout_help_y(),
                   "Space: play/pause   R: restart   S: stop   Click: seek",
                   THEME_TEXT_FAINT, w - 2 * m);
    else
        draw_label(w, h, m, layout_help_y(),
                   "Space play/pause  R restart  S stop",
                   THEME_TEXT_FAINT, w - 2 * m);

    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

int main(int argc, char **argv) {
    const char *path_arg = (argc > 4 && argv[4] && argv[4][0]) ? argv[4] : 0;
    int exit_status = 0;

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

    {
        int w = 0, h = 0;
        render_frame(&w, &h);
        if (guiapp_send_frame(&gui, "Music", w, h, pixels) < 0)
            closed = 1;
    }

    while (!closed) {
        int w = 0, h = 0;
        render_frame(&w, &h);
        if (guiapp_send_frame(&gui, "Music", w, h, pixels) < 0) {
            closed = 1;
            break;
        }
        sleep_ms(FRAME_MS);
    }

    closed = 1;
    if (playback_tid >= 0)
        (void)join(playback_tid);
    (void)audio_flush();
    free(samples);
    return exit_status;
}
