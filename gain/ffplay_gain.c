/*
 * ffplay gain experiment: integration layer.
 *
 * - REPLAYGAIN_* tag extraction adapted from VLC
 *   vlc_replay_gain_CopyFromMeta (AVDictionary replaces vlc_meta_t; a single
 *   lookup key is enough because av_dict_get matches case-insensitively by
 *   default, while VLC had to try both spellings).
 * - Configuration replaces VLC's audio-replay-gain-* variables with
 *   environment variables, read once.
 * - The SDL callback trampoline applies the gain after the original callback,
 *   mirroring VLC's "software volume" stage at the output sink
 *   (aout_volume_Amplify in src/audio_output/dec.c).
 *
 * ffplay forces the SDL device to AUDIO_S16SYS (audio_open fails on any other
 * format), so the S16 kernel is the only one wired up; factor updates are
 * atomic and safe from the stream thread while the SDL audio thread applies.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/avstring.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/eval.h>
#include <libavutil/log.h>

#include "ffplay_gain.h"
#include "vlc/replay_gain.h"
#include "vlc/volume.h"

/* Configuration (VLC: "gain", "audio-replay-gain-*" in src/libvlc-module.c). */
static ReplayGainConfig gain_cfg = {
    .mode            = RG_NONE,
    .preamp          = 0.f,
    .default_gain    = -7.f,
    .peak_protection = 1,
    .gain            = 1.f,
};

static GainVolume gain_volume;
static int gain_cfg_loaded; /* read thread only: config loading never races */

static const struct {
    int         type;     /* RG_TRACK / RG_ALBUM */
    int         is_gain;  /* 1 = gain tag in dB, 0 = peak tag */
    const char *key;
} gain_tags[] = {
    { RG_TRACK, 1, "REPLAYGAIN_TRACK_GAIN" },
    { RG_TRACK, 0, "REPLAYGAIN_TRACK_PEAK" },
    { RG_ALBUM, 1, "REPLAYGAIN_ALBUM_GAIN" },
    { RG_ALBUM, 0, "REPLAYGAIN_ALBUM_PEAK" },
};

static float gain_env_float(const char *name, float def)
{
    const char *s = getenv(name);
    char *end;
    double v;

    if (!s || !*s)
        return def;
    v = av_strtod(s, &end);
    return end == s ? def : (float)v;
}

static void gain_load_config(void)
{
    const char *mode = getenv("FFPLAY_GAIN_MODE");
    const char *peak;

    if (mode) {
        if (!strcmp(mode, "track"))
            gain_cfg.mode = RG_TRACK;
        else if (!strcmp(mode, "album"))
            gain_cfg.mode = RG_ALBUM;
        else
            gain_cfg.mode = RG_NONE;
    }
    gain_cfg.preamp          = gain_env_float("FFPLAY_GAIN_PREAMP", 0.f);
    gain_cfg.default_gain    = gain_env_float("FFPLAY_GAIN_DEFAULT", -7.f);
    peak                     = getenv("FFPLAY_GAIN_PEAK_PROTECTION");
    if (peak)
        gain_cfg.peak_protection = strcmp(peak, "0") != 0;
    gain_cfg.gain            = gain_env_float("FFPLAY_GAIN", 1.f);

    gain_volume_init(&gain_volume);
    /* ffplay forces the SDL device to AUDIO_S16SYS (see audio_open). */
    gain_volume_set_amplifier(&gain_volume, amplify_s16);
    gain_cfg_loaded = 1;

    av_log(NULL, AV_LOG_INFO,
           "[gain] mode=%s preamp=%.2f dB default=%.2f dB peak_protection=%s gain=%.4f\n",
           gain_cfg.mode == RG_TRACK ? "track" :
           gain_cfg.mode == RG_ALBUM ? "album" : "none",
           gain_cfg.preamp, gain_cfg.default_gain,
           gain_cfg.peak_protection ? "on" : "off",
           gain_cfg.gain);
}

/*
 * Extracts REPLAYGAIN_* tags, adapted from VLC vlc_replay_gain_CopyFromMeta.
 * The reference loudness tag is only consulted when a gain tag was found.
 */
static void replay_gain_from_avdict(ReplayGain *rg, const AVDictionary *meta)
{
    const AVDictionaryEntry *e;
    int i;

    for (i = 0; i < (int)FF_ARRAY_ELEMS(gain_tags); i++) {
        float v;

        e = av_dict_get(meta, gain_tags[i].key, NULL, 0);
        if (!e)
            continue;
        v = (float)av_strtod(e->value, NULL);
        if (gain_tags[i].is_gain) {
            rg->have_gain[gain_tags[i].type] = 1;
            rg->gain[gain_tags[i].type]      = v;
        } else {
            rg->have_peak[gain_tags[i].type] = 1;
            rg->peak[gain_tags[i].type]      = v;
        }
    }

    if (rg->have_gain[RG_TRACK] || rg->have_gain[RG_ALBUM]) {
        e = av_dict_get(meta, "REPLAYGAIN_REFERENCE_LOUDNESS", NULL, 0);
        if (e) {
            rg->have_reference_loudness = 1;
            rg->reference_loudness      = (float)av_strtod(e->value, NULL);
        }
    }
}

void ffplay_gain_on_stream(AVFormatContext *ic, AVStream *st)
{
    ReplayGain rg = { 0 }, file = { 0 };
    float mult;

    if (!gain_cfg_loaded)
        gain_load_config();
    if (!st || st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
        return;

    /* Stream level tags win; file level tags fill the gaps (VLC
     * replay_gain_Merge on es format vs input item metadata). */
    replay_gain_from_avdict(&rg, st->metadata);
    if (ic) {
        replay_gain_from_avdict(&file, ic->metadata);
        replay_gain_merge(&rg, &file);
    }

    mult = replay_gain_calc_multiplier(&gain_cfg, &rg);
    gain_volume_set_factor(&gain_volume, mult);
    av_log(NULL, AV_LOG_DEBUG, "[gain] applying %.6f (%.2f dB)\n",
           mult, 20.f * log10f(mult));
}

static SDL_AudioCallback gain_orig_cb;

static void SDLCALL gain_tramp(void *opaque, Uint8 *stream, int len)
{
    gain_orig_cb(opaque, stream, len);
    gain_volume_apply(&gain_volume, stream, len);
}

SDL_AudioCallback ffplay_gain_wrap_callback(SDL_AudioCallback orig)
{
    if (!gain_cfg_loaded)
        gain_load_config();
    gain_orig_cb = orig;
    return gain_tramp;
}
