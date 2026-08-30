/*
 * ffplay gain experiment, mpv backend: integration layer.
 *
 * - The total gain factor is mpv's audio_get_gain() over a MpvGainConfig
 *   sourced from FFPLAY_MPV_* environment variables (mpv replaces its
 *   MPOpts/property plumbing with env vars here; the math is untouched).
 * - Replaygain data is harvested with mpv's demuxer semantics:
 *   demux/demux_lavf.c export_replaygain() (AV_PKT_DATA_REPLAYGAIN side
 *   data, microbels / 100000) when present, else demux/demux.c
 *   demux_update_replaygain() ordering (stream tags, then file tags, first
 *   win — no per-tag merging, that was the VLC backend's behavior).
 * - The SDL callback trampoline applies mpv's process_plane() S16 kernel
 *   after the original callback, i.e. at the last stage before the device,
 *   which is where mpv applies its AO gain (audio/out/buffer.c).
 *
 * mpv stores the factor in a C11 _Atomic float (struct ao, relaxed loads);
 * this port uses the same lock-free float pattern as the VLC backend
 * (gain/vlc/volume.h) because the Windows build is MSVC.  ffplay forces the
 * SDL device to AUDIO_S16SYS, so the S16 kernel is the only one wired up.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/dict.h>
#include <libavutil/log.h>
#include <libavutil/replaygain.h>

#include "ffplay_gain_mpv.h"
#include "mpv/mpv_kernels.h"
#include "mpv/mpv_replaygain.h"

/* --- lock-free float, same pattern as gain/vlc/volume.h (MSVC fallback) --- */

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>

typedef struct MpvAtomicFloat {
    volatile long value;
} MpvAtomicFloat;

static void mpv_atomic_store(MpvAtomicFloat *a, float v)
{
    union { long l; float f; } u;
    u.f = v;
    _InterlockedExchange(&a->value, u.l);
}

static float mpv_atomic_load(MpvAtomicFloat *a)
{
    union { long l; float f; } u;
    u.l = a->value;
    return u.f;
}
#else
typedef struct MpvAtomicFloat {
    _Atomic float value;
} MpvAtomicFloat;

static void mpv_atomic_store(MpvAtomicFloat *a, float v)
{
    a->value = v;
}

static float mpv_atomic_load(MpvAtomicFloat *a)
{
    return a->value;
}
#endif

/* --- configuration (mpv MPOpts subset; defaults = mpv defaults) ---------- */

static MpvGainConfig mpv_cfg = {
    .volume        = 100.f,   /* --volume, 100 = unity                    */
    .volume_gain   = 0.f,     /* --volume-gain, dB                        */
    .mute          = 0,       /* --mute                                   */
    .rgain_mode    = 0,       /* --replaygain: 0 no / 1 track / 2 album   */
    .rgain_preamp  = 0.f,     /* --replaygain-preamp, dB                  */
    .rgain_clip    = 0,       /* --replaygain-clip, default no: protect   */
    .rgain_fallback= 0.f,     /* --replaygain-fallback; 0 disables (the
                                   C falsy check is mpv's own behavior)    */
};

static MpvAtomicFloat mpv_gain;      /* struct ao -> gain                     */
static MpvReplayGain mpv_rg;         /* decoded tag data, NULL-equivalent flag */
static int mpv_have_rg;
static int mpv_cfg_loaded;           /* read thread only: no race             */
static FILE *mpv_capture;            /* FFPLAY_MPV_CAPTURE: post-gain dump    */

static float mpv_env_float(const char *name, float def)
{
    const char *s = getenv(name);
    char *end;
    double v;

    if (!s || !*s)
        return def;
    v = strtod(s, &end);
    return end == s ? def : (float)v;
}

static void mpv_load_config(void)
{
    const char *mode = getenv("FFPLAY_MPV_REPLAYGAIN");
    const char *mute = getenv("FFPLAY_MPV_MUTE");
    const char *clip = getenv("FFPLAY_MPV_RG_CLIP");
    const char *cap  = getenv("FFPLAY_MPV_CAPTURE");

    if (mode) {
        if (!strcmp(mode, "track"))
            mpv_cfg.rgain_mode = 1;
        else if (!strcmp(mode, "album"))
            mpv_cfg.rgain_mode = 2;
        else
            mpv_cfg.rgain_mode = 0;
    }
    mpv_cfg.volume         = mpv_env_float("FFPLAY_MPV_VOLUME", 100.f);
    mpv_cfg.volume_gain    = mpv_env_float("FFPLAY_MPV_VOLUME_GAIN", 0.f);
    if (mute)
        mpv_cfg.mute = (*mute && strcmp(mute, "0") != 0);
    mpv_cfg.rgain_preamp   = mpv_env_float("FFPLAY_MPV_RG_PREAMP", 0.f);
    if (clip)
        mpv_cfg.rgain_clip = (*clip && strcmp(clip, "0") != 0);
    mpv_cfg.rgain_fallback = mpv_env_float("FFPLAY_MPV_RG_FALLBACK", 0.f);
    if (cap && *cap)
        mpv_capture = fopen(cap, "wb");

    mpv_atomic_store(&mpv_gain, 1.0f);
    mpv_cfg_loaded = 1;

    av_log(NULL, AV_LOG_INFO,
           "[mpv-gain] volume=%.1f%% volume_gain=%.2f dB mute=%d "
           "replaygain=%s preamp=%.2f dB clip=%s fallback=%.2f dB capture=%s\n",
           mpv_cfg.volume, mpv_cfg.volume_gain, mpv_cfg.mute,
           mpv_cfg.rgain_mode == 1 ? "track" :
           mpv_cfg.rgain_mode == 2 ? "album" : "no",
           mpv_cfg.rgain_preamp, mpv_cfg.rgain_clip ? "allowed" : "prevented",
           mpv_cfg.rgain_fallback, mpv_capture ? "on" : "off");
}

/* Recompute the total factor (mpv: audio_update_volume -> ao_set_gain). */
static void mpv_recompute(void)
{
    float gain = mpv_audio_get_gain(&mpv_cfg, mpv_have_rg ? &mpv_rg : NULL);
    mpv_atomic_store(&mpv_gain, gain);
    av_log(NULL, AV_LOG_DEBUG, "[mpv-gain] factor %.6f (%.2f dB)\n",
           gain, 20.0 * log10(gain));
}

/* --- replaygain harvest, mpv demuxer semantics --------------------------- */

/*
 * demux/demux_lavf.c export_replaygain(): AVReplayGain side data is in
 * microbels (divide by 100000 for dB); peak 100000 = full scale.  The
 * INT32_MIN / 0 sentinels mark absent data, and album falls back to track.
 */
static int mpv_rg_from_side_data(MpvReplayGain *out, const AVStream *st)
{
    const AVPacketSideData *sd = av_packet_side_data_get(
        st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data,
        AV_PKT_DATA_REPLAYGAIN);
    const AVReplayGain *av_rgain = sd ? (const AVReplayGain *)sd->data : NULL;

    if (!av_rgain)
        return 0;

    int track_ok = av_rgain->track_gain != INT32_MIN && av_rgain->track_peak != 0;
    int album_ok = av_rgain->album_gain != INT32_MIN && av_rgain->album_peak != 0;
    if (!track_ok && !album_ok)
        return 0;

    out->track_gain = out->album_gain = 0;
    out->track_peak = out->album_peak = 1;
    if (track_ok) {
        out->track_gain = out->album_gain =
            av_rgain->track_gain / 100000.0f;
        out->track_peak = out->album_peak =
            av_rgain->track_peak / 100000.0f;
    }
    if (album_ok) {
        out->album_gain = av_rgain->album_gain / 100000.0f;
        out->album_peak = av_rgain->album_peak / 100000.0f;
    }
    return 1;
}

/* demux/demux.c decode_rgain() over an AVDictionary. */
static int mpv_rg_from_dict(MpvReplayGain *out, const AVDictionary *meta)
{
    MpvRgTags tags = { 0 };
    static const char *const keys[] = {
        "REPLAYGAIN_TRACK_GAIN", "REPLAYGAIN_TRACK_PEAK",
        "REPLAYGAIN_ALBUM_GAIN", "REPLAYGAIN_ALBUM_PEAK",
        "REPLAYGAIN_GAIN",       "REPLAYGAIN_PEAK",
        "R128_TRACK_GAIN",       "R128_ALBUM_GAIN",
    };
    const char **slots[] = {
        &tags.track_gain, &tags.track_peak,
        &tags.album_gain, &tags.album_peak,
        &tags.rg_gain,    &tags.rg_peak,
        &tags.r128_track, &tags.r128_album,
    };
    int i;

    for (i = 0; i < (int)FF_ARRAY_ELEMS(keys); i++) {
        const AVDictionaryEntry *e = av_dict_get(meta, keys[i], NULL, 0);
        if (e)
            *slots[i] = e->value;
    }
    return mpv_decode_rgain(out, &tags);
}

void ffplay_gain_mpv_on_stream(AVFormatContext *ic, AVStream *st)
{
    if (!mpv_cfg_loaded)
        mpv_load_config();
    if (!st || st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
        return;

    /* mpv: side data first (export_replaygain); else stream tags, then file
     * tags (demux_update_replaygain) — first source that yields data wins. */
    mpv_have_rg = mpv_rg_from_side_data(&mpv_rg, st);
    if (!mpv_have_rg && st->metadata)
        mpv_have_rg = mpv_rg_from_dict(&mpv_rg, st->metadata);
    if (!mpv_have_rg && ic && ic->metadata)
        mpv_have_rg = mpv_rg_from_dict(&mpv_rg, ic->metadata);
    if (!mpv_have_rg)
        memset(&mpv_rg, 0, sizeof(mpv_rg));

    if (mpv_have_rg)
        av_log(NULL, AV_LOG_INFO,
               "[mpv-gain] Replaygain: Track=%f/%f Album=%f/%f\n",
               mpv_rg.track_gain, mpv_rg.track_peak,
               mpv_rg.album_gain, mpv_rg.album_peak);

    mpv_recompute();
}

/* --- SDL callback trampoline --------------------------------------------- */

static SDL_AudioCallback mpv_orig_cb;

static void SDLCALL mpv_gain_tramp(void *opaque, Uint8 *stream, int len)
{
    mpv_orig_cb(opaque, stream, len);

    /* mpv applies the gain to the final device samples (S16 interleaved:
     * len bytes = len / 2 scalar samples, process_plane semantics). */
    float gain = mpv_atomic_load(&mpv_gain);
    mpv_process_plane(MPV_AFMT_S16, stream, len / 2, gain);

    if (mpv_capture && len > 0 && fwrite(stream, 1, len, mpv_capture) != (size_t)len) {
        av_log(NULL, AV_LOG_ERROR, "[mpv-gain] capture write failed\n");
        fclose(mpv_capture);
        mpv_capture = NULL;
    }
}

SDL_AudioCallback ffplay_gain_mpv_wrap_callback(SDL_AudioCallback orig)
{
    if (!mpv_cfg_loaded)
        mpv_load_config();
    mpv_orig_cb = orig;
    return mpv_gain_tramp;
}
