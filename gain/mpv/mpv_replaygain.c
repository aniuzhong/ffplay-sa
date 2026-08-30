/*
 * mpv audio gain: gain composition and replaygain tag parsing.
 *
 * Ported from mpv 0.41.0 (GPLv2+).  Function bodies follow the upstream
 * sources statement by statement (see mpv_replaygain.h for provenance);
 * upstream names appear in the comments for diffing.  Do not "clean up" the
 * float/double mixing or the evaluation order: the bit-comparable contract
 * of this port depends on them.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "mpv_replaygain.h"

/* common/common.h */
#define MPMIN(a, b) ((a) > (b) ? (b) : (a))
#define MPMAX(a, b) ((a) > (b) ? (a) : (b))

double mpv_db_gain(double db)
{
    return pow(10.0, db / 20.0);
}

/* demux/demux.c: decode_float(). */
static int decode_float(const char *str, float *out)
{
    char *rest;
    float dec_val;

    dec_val = strtod(str, &rest);
    if (rest == str || !isfinite(dec_val))
        return -1;

    *out = dec_val;
    return 0;
}

/* demux/demux.c: decode_gain(), with the tag lookup hoisted to the caller. */
static int decode_gain(const char *tag_val, float *out)
{
    float dec_val;

    if (!tag_val)
        return -1;

    if (decode_float(tag_val, &dec_val) < 0)
        return -1;

    *out = dec_val;
    return 0;
}

/* demux/demux.c: decode_peak(); an absent peak is fine and defaults to 1.0. */
static int decode_peak(const char *tag_val, float *out)
{
    float dec_val;

    *out = 1.0;

    if (!tag_val)
        return 0;

    if (decode_float(tag_val, &dec_val) < 0 || dec_val <= 0.0)
        return -1;

    *out = dec_val;
    return 0;
}

int mpv_decode_rgain(MpvReplayGain *out, const MpvRgTags *tags)
{
    MpvReplayGain rg = { 0 };

    if (!out || !tags)
        return 0;

    /*
     * demux/demux.c decode_rgain(), same tag generations in the same order.
     * Album gain falls back to track gain when undefined; a present but
     * invalid peak tag disqualifies the whole tag set (decode_peak < 0).
     */
    if (decode_gain(tags->track_gain, &rg.track_gain) >= 0 &&
        decode_peak(tags->track_peak, &rg.track_peak) >= 0)
    {
        if (decode_gain(tags->album_gain, &rg.album_gain) < 0 ||
            decode_peak(tags->album_peak, &rg.album_peak) < 0)
        {
            /* Album gain is undefined; fall back to track gain. */
            rg.album_gain = rg.track_gain;
            rg.album_peak = rg.track_peak;
        }
        *out = rg;
        return 1;
    }

    if (decode_gain(tags->rg_gain, &rg.track_gain) >= 0 &&
        decode_peak(tags->rg_peak, &rg.track_peak) >= 0)
    {
        rg.album_gain = rg.track_gain;
        rg.album_peak = rg.track_peak;
        *out = rg;
        return 1;
    }

    /* RFC 7845 Opus tags: Q7.8 fixed point dB, EBU R128 reference, no peaks. */
    if (decode_gain(tags->r128_track, &rg.track_gain) >= 0) {
        if (decode_gain(tags->r128_album, &rg.album_gain) < 0) {
            /* Album gain is undefined; fall back to track gain. */
            rg.album_gain = rg.track_gain;
        }
        rg.track_gain /= 256.;
        rg.album_gain /= 256.;

        /*
         * Add 5dB to compensate for the different reference levels between
         * our reference of ReplayGain 2 (-18 LUFS) and EBU R128 (-23 LUFS).
         */
        rg.track_gain += 5.;
        rg.album_gain += 5.;
        *out = rg;
        return 1;
    }

    return 0;
}

/*
 * player/audio.c: compute_replaygain(), with the replaygain_data lookup
 * replaced by the caller-supplied rg pointer (NULL = no tag data, as when
 * decode_rgain found nothing).
 */
float mpv_compute_replaygain(const MpvGainConfig *cfg, const MpvReplayGain *rg)
{
    float rgain = 1.0;

    if (cfg->rgain_mode && rg) {
        float gain, peak;
        if (cfg->rgain_mode == 1) {
            gain = rg->track_gain;
            peak = rg->track_peak;
        } else {
            gain = rg->album_gain;
            peak = rg->album_peak;
        }

        gain += cfg->rgain_preamp;
        rgain = (float)mpv_db_gain(gain);

        if (!cfg->rgain_clip) { /* clipping prevention */
            rgain = (float)MPMIN(rgain, 1.0 / peak);
        }
    } else if (cfg->rgain_fallback) {
        rgain = (float)mpv_db_gain(cfg->rgain_fallback);
    }

    return rgain;
}

/* player/audio.c: audio_get_gain(). */
float mpv_audio_get_gain(const MpvGainConfig *cfg, const MpvReplayGain *rg)
{
    float gain = (float)MPMAX(cfg->volume / 100.0, 0);
    gain = (float)pow(gain, 3);
    gain *= mpv_compute_replaygain(cfg, rg);
    gain *= (float)mpv_db_gain(cfg->volume_gain);
    if (cfg->mute)
        gain = 0.0;
    return gain;
}
