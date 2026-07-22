#include "libc.h"
#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

#define MIX_CHANNELS 16
#define MIX_RATE 11025
#define MIX_SAMPLES 315

struct mix_channel {
    const byte *samples;
    uint32_t length;
    uint32_t position;
    uint32_t step;
    int volume;
    int active;
};

static struct mix_channel channels[MIX_CHANNELS];
static byte mix_buffer[MIX_SAMPLES];
static boolean use_prefix;
static snddevice_t devices[] = { SNDDEVICE_SB };

/* Configuration variables normally supplied by the SDL sound module. */
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

static int buzz_get_lump(sfxinfo_t *sfx) {
    char name[9];
    const char *src = sfx->link ? sfx->link->name : sfx->name;
    int out = 0;
    if (use_prefix) { name[out++] = 'd'; name[out++] = 's'; }
    while (*src && out < 8) name[out++] = *src++;
    name[out] = 0;
    return W_GetNumForName(name);
}

static boolean buzz_sound_init(boolean prefix) {
    use_prefix = prefix;
    memset(channels, 0, sizeof(channels));
    return audio_config(MIX_RATE) == 0;
}

static void buzz_sound_shutdown(void) {
    memset(channels, 0, sizeof(channels));
}

static void buzz_sound_update(void) {
    for (int i = 0; i < MIX_SAMPLES; i++) {
        int mixed = 0;
        for (int c = 0; c < MIX_CHANNELS; c++) {
            struct mix_channel *ch = &channels[c];
            if (!ch->active) continue;
            uint32_t sample_index = ch->position >> 16;
            if (sample_index >= ch->length) {
                ch->active = 0;
                continue;
            }
            /* The sub-percent gain difference is inaudible, while avoiding
             * thousands of integer divisions per game frame is substantial. */
            mixed += (((int)ch->samples[sample_index] - 128) * ch->volume) >> 7;
            ch->position += ch->step;
        }
        if (mixed < -128) mixed = -128;
        if (mixed > 127) mixed = 127;
        mix_buffer[i] = (byte)(mixed + 128);
    }
    (void)audio_write(mix_buffer, sizeof(mix_buffer));
}

static void buzz_sound_params(int channel, int volume, int separation) {
    (void)separation;
    if (channel >= 0 && channel < MIX_CHANNELS)
        channels[channel].volume = volume;
}

static int buzz_start_sound(sfxinfo_t *sfx, int channel, int volume, int separation) {
    (void)separation;
    if (channel < 0 || channel >= MIX_CHANNELS) return -1;
    if (sfx->lumpnum < 0) sfx->lumpnum = buzz_get_lump(sfx);
    int lump = sfx->lumpnum;
    int lump_size = W_LumpLength((unsigned)lump);
    byte *raw = W_CacheLumpNum(lump, PU_STATIC);
    if (!raw || lump_size < 48 || raw[0] != 3 || raw[1] != 0) return -1;
    uint32_t rate = (uint32_t)raw[2] | ((uint32_t)raw[3] << 8);
    uint32_t length = (uint32_t)raw[4] | ((uint32_t)raw[5] << 8) |
                      ((uint32_t)raw[6] << 16) | ((uint32_t)raw[7] << 24);
    if (!rate || length > (uint32_t)lump_size - 8u || length <= 32u) return -1;

    struct mix_channel *ch = &channels[channel];
    ch->samples = raw + 8 + 16;
    ch->length = length - 32;
    ch->position = 0;
    ch->step = (rate << 16) / MIX_RATE;
    if (!ch->step) ch->step = 1;
    ch->volume = volume;
    ch->active = 1;
    return channel;
}

static void buzz_stop_sound(int channel) {
    if (channel >= 0 && channel < MIX_CHANNELS) channels[channel].active = 0;
}

static boolean buzz_sound_playing(int channel) {
    return channel >= 0 && channel < MIX_CHANNELS && channels[channel].active;
}

static void buzz_precache(sfxinfo_t *sounds, int count) {
    (void)sounds; (void)count;
}

sound_module_t DG_sound_module = {
    devices, 1, buzz_sound_init, buzz_sound_shutdown, buzz_get_lump,
    buzz_sound_update, buzz_sound_params, buzz_start_sound, buzz_stop_sound,
    buzz_sound_playing, buzz_precache
};

/* PCM effects are implemented now. Music lumps are MUS/MIDI and require a
 * synthesizer, so expose a quiet music backend rather than misplaying them. */
static boolean music_init(void) { return true; }
static void music_void(void) {}
static void music_volume(int volume) { (void)volume; }
static void *music_register(void *data, int len) { (void)len; return data; }
static void music_unregister(void *handle) { (void)handle; }
static void music_play(void *handle, boolean looping) { (void)handle; (void)looping; }
static boolean music_playing(void) { return false; }

music_module_t DG_music_module = {
    devices, 1, music_init, music_void, music_volume, music_void, music_void,
    music_register, music_unregister, music_play, music_void, music_playing, 0
};
