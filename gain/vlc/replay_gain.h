/*
 * Replay gain data structures and multiplier calculation.
 *
 * Ported from VLC (LGPL-2.1+):
 *   - include/vlc_replay_gain.h   (audio_replay_gain_t, replay_gain_Merge;
 *                                  replay_gain_Compare/Reset omitted: unused)
 *   - src/input/replay_gain.c     (replay_gain_CalcMultiplier)
 * Upstream function/variable names are noted where it helps diffing against
 * VLC; the algorithm is kept faithful to the original.
 *
 * The calculation is a pure function so it can be unit tested without any
 * FFmpeg dependency (see gain/tests/test_replay_gain.c).
 */

#ifndef GAIN_VLC_REPLAY_GAIN_H
#define GAIN_VLC_REPLAY_GAIN_H

/* Values for ReplayGainConfig.mode (VLC AUDIO_REPLAY_GAIN_TRACK/ALBUM/MAX). */
#define RG_TRACK 0
#define RG_ALBUM 1
#define RG_NONE  2

typedef struct ReplayGain {
    int   have_gain[2];    /* valid flag per RG_TRACK/RG_ALBUM          */
    float gain[2];         /* gain value in dB                          */
    int   have_peak[2];
    float peak[2];         /* peak value, 1.0 means full sample value   */
    int   have_reference_loudness;
    float reference_loudness; /* LUFS, from REPLAYGAIN_REFERENCE_LOUDNESS */
} ReplayGain;

typedef struct ReplayGainConfig {
    int   mode;            /* RG_TRACK, RG_ALBUM or RG_NONE             */
    float preamp;          /* user pre-amplification in dB              */
    float default_gain;    /* dB applied when the file has no gain tags */
    int   peak_protection; /* clamp multiplier to 1/peak to avoid clipping */
    float gain;            /* extra user multiplier, linear scale, 1.0 = unity */
} ReplayGainConfig;

/*
 * Copies gain/peak/reference values from src into dst when they are set in
 * src and absent in dst (VLC replay_gain_Merge).
 */
void replay_gain_merge(ReplayGain *dst, const ReplayGain *src);

/*
 * Linear gain multiplier for the configured mode, following the Replay Gain
 * 2.0 specification (VLC replay_gain_CalcMultiplier).
 */
float replay_gain_calc_multiplier(const ReplayGainConfig *cfg,
                                  const ReplayGain *rg);

#endif /* GAIN_VLC_REPLAY_GAIN_H */
