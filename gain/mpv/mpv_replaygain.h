/*
 * mpv audio gain: gain composition and replaygain tag model.
 *
 * Ported from mpv 0.41.0 (GPLv2+), provenance per function:
 *   - player/audio.c  db_gain(), compute_replaygain(), audio_get_gain()
 *   - demux/demux.c   decode_float(), decode_gain(), decode_peak(),
 *                     decode_rgain()
 * The C expression sequences, operand types and evaluation order are kept
 * identical to the upstream functions so the results are bit-comparable;
 * the MPContext/mp_tags plumbing is replaced by plain structs and strings.
 *
 * Pure C: no FFmpeg dependency, unit tested in gain/tests/test_mpv_gain.c.
 */

#ifndef GAIN_MPV_REPLAYGAIN_H
#define GAIN_MPV_REPLAYGAIN_H

/* demux/demux.c: struct replaygain_data. */
typedef struct MpvReplayGain {
    float track_gain;   /* dB */
    float track_peak;   /* 1.0 = full sample value */
    float album_gain;   /* dB */
    float album_peak;
} MpvReplayGain;

/*
 * The option subset player/audio.c reads from MPOpts, with mpv defaults
 * (options/options.c mp_default_opts).  Note there is no volume-max clamp
 * here: mpv clamps --volume in the property layer (player/command.c), not in
 * audio_get_gain().
 */
typedef struct MpvGainConfig {
    float volume;        /* mpv --volume, percent, default 100            */
    float volume_gain;   /* mpv --volume-gain, dB, default 0              */
    int   mute;          /* mpv --mute                                    */
    int   rgain_mode;    /* mpv --replaygain: 0 no, 1 track, 2 album      */
    float rgain_preamp;  /* mpv --replaygain-preamp, dB, default 0        */
    int   rgain_clip;    /* mpv --replaygain-clip (allow clipping), no=0  */
    float rgain_fallback;/* mpv --replaygain-fallback, dB, default 0 (off) */
} MpvGainConfig;

/* Raw tag strings, as found in container metadata (absent = NULL). */
typedef struct MpvRgTags {
    const char *track_gain;   /* REPLAYGAIN_TRACK_GAIN */
    const char *track_peak;   /* REPLAYGAIN_TRACK_PEAK */
    const char *album_gain;   /* REPLAYGAIN_ALBUM_GAIN */
    const char *album_peak;   /* REPLAYGAIN_ALBUM_PEAK */
    const char *rg_gain;      /* REPLAYGAIN_GAIN       */
    const char *rg_peak;      /* REPLAYGAIN_PEAK       */
    const char *r128_track;   /* R128_TRACK_GAIN (Opus, Q7.8 dB) */
    const char *r128_album;   /* R128_ALBUM_GAIN (Opus, Q7.8 dB) */
} MpvRgTags;

/* player/audio.c: db_gain(). */
double mpv_db_gain(double db);

/*
 * demux/demux.c: decode_rgain().  Returns 1 and fills *out when the tags
 * carry recognized replaygain data (classic tags, REPLAYGAIN_GAIN/PEAK, or
 * Opus R128 with the +5 dB reference level compensation), 0 otherwise.
 */
int mpv_decode_rgain(MpvReplayGain *out, const MpvRgTags *tags);

/*
 * player/audio.c: compute_replaygain().  rg may be NULL (no tag data).
 * The --replaygain-fallback quirk is kept: a non-zero fallback applies even
 * when rgain_mode is "no", and 0.0 disables it (C falsy check upstream).
 */
float mpv_compute_replaygain(const MpvGainConfig *cfg,
                             const MpvReplayGain *rg);

/*
 * player/audio.c: audio_get_gain().  The one total gain factor:
 * (volume/100)^3 * replaygain * db_gain(volume-gain), 0 when muted.
 */
float mpv_audio_get_gain(const MpvGainConfig *cfg, const MpvReplayGain *rg);

#endif /* GAIN_MPV_REPLAYGAIN_H */
