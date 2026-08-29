/*
 * Replay gain multiplier calculation, ported from VLC src/input/replay_gain.c
 * (replay_gain_CalcMultiplier), LGPL-2.1+, Copyright (C) 2011-2012
 * Rémi Denis-Courmont. The decision flow and the peak protection constants
 * are kept identical to the upstream implementation.
 */

#include <math.h>

#include "replay_gain.h"

void replay_gain_merge(ReplayGain *dst, const ReplayGain *src)
{
    int i;

    if (!dst || !src)
        return;

    if (!dst->have_reference_loudness && src->have_reference_loudness) {
        dst->have_reference_loudness = src->have_reference_loudness;
        dst->reference_loudness      = src->reference_loudness;
    }

    for (i = 0; i < 2; i++) {
        if (!dst->have_gain[i] && src->have_gain[i]) {
            dst->have_gain[i] = src->have_gain[i];
            dst->gain[i]      = src->gain[i];
        }
        if (!dst->have_peak[i] && src->have_peak[i]) {
            dst->have_peak[i] = src->have_peak[i];
            dst->peak[i]      = src->peak[i];
        }
    }
}

float replay_gain_calc_multiplier(const ReplayGainConfig *cfg,
                                  const ReplayGain *rg)
{
    int mode = cfg->mode;
    float gain, multiplier;

    if (mode != RG_TRACK && mode != RG_ALBUM)
        return cfg->gain;

    /* If the selected mode is not available, prefer the other one. */
    if (!rg->have_gain[mode] && rg->have_gain[!mode])
        mode = !mode;

    if (rg->have_gain[mode]) {
        /* Replay gain uses -18 LUFS as the reference level; a different
         * reference loudness in the tags shifts the whole curve. */
        const float rg_ref_lufs = -18.f;
        float ref_lufs_delta = 0.f;

        if (rg->have_reference_loudness)
            ref_lufs_delta = rg_ref_lufs - rg->reference_loudness;

        gain = rg->gain[mode] + cfg->preamp + ref_lufs_delta;
    } else {
        gain = cfg->default_gain;
    }

    multiplier = powf(10.f, gain / 20.f);

    /* Skip peak protection for the default gain case, as the default peak
     * value of 1.0 would limit gains greater than 0 dB. */
    if (rg->have_gain[mode] && cfg->peak_protection) {
        /* Use a peak of 1.0 if the peak value is missing or invalid. */
        float peak = rg->have_peak[mode] && rg->peak[mode] > 0.f ?
                     rg->peak[mode] : 1.f;
        /* To avoid clipping, the max gain multiplier must be <= 1.0 / peak. */
        float peak_limit = 1.f / peak;

        if (multiplier > peak_limit)
            multiplier = peak_limit;
    }

    /* Apply the configuration gain. */
    return multiplier * cfg->gain;
}
