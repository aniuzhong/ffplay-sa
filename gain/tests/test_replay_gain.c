/*
 * Unit tests for the replay gain multiplier calculation, ported from VLC
 * src/input/replay_gain.c. The expected values are the ones documented in
 * the upstream peak protection comment (peak 0.5 -> max +6.02 dB, peak 1.0
 * -> unity gain, peak 2.0 -> -6.02 dB).
 *
 * Pure C: no FFmpeg dependency, compiled by the test_replay_gain target.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "vlc/replay_gain.h"

static int failures;

#define CHECK_NEAR(actual, expected) \
    do { \
        double a_ = (double)(actual), e_ = (double)(expected); \
        if (fabs(a_ - e_) > 1e-4) { \
            printf("FAIL %s: got %f, expected %f (line %d)\n", \
                   #actual, a_, e_, __LINE__); \
            failures++; \
        } \
    } while (0)

static ReplayGainConfig base_cfg(void)
{
    ReplayGainConfig cfg;

    cfg.mode            = RG_NONE;
    cfg.preamp          = 0.f;
    cfg.default_gain    = -7.f;
    cfg.peak_protection = 1;
    cfg.gain            = 1.f;
    return cfg;
}

static ReplayGain empty_rg(void)
{
    ReplayGain rg;

    memset(&rg, 0, sizeof(rg));
    return rg;
}

/* mode "none" just applies the configuration gain (VLC: return config_gain). */
static void test_mode_none(void)
{
    ReplayGainConfig cfg = base_cfg();
    ReplayGain rg = empty_rg();

    CHECK_NEAR(replay_gain_calc_multiplier(&cfg, &rg), 1.f);
    cfg.gain = 0.5f;
    CHECK_NEAR(replay_gain_calc_multiplier(&cfg, &rg), 0.5f);
}

/* +6.0206 dB doubles the signal; the missing peak defaults to 1.0. */
static void test_track_gain(void)
{
    ReplayGainConfig cfg = base_cfg();
    ReplayGain rg = empty_rg();

    cfg.mode = RG_TRACK;
    rg.have_gain[RG_TRACK] = 1;
    rg.gain[RG_TRACK] = 6.0206f;

    /* With protection on, a missing peak counts as 1.0 and clamps any
     * amplification to unity (VLC behavior). */
    CHECK_NEAR(replay_gain_calc_multiplier(&cfg, &rg), 1.f);

    cfg.peak_protection = 0;
    CHECK_NEAR(replay_gain_calc_multiplier(&cfg, &rg), 2.f);
}

/* The other mode is preferred when the selected one has no gain tag. */
static void test_album_fallback(void)
{
    ReplayGainConfig cfg = base_cfg();
    ReplayGain rg = empty_rg();

    cfg.mode = RG_TRACK;
    rg.have_gain[RG_ALBUM] = 1;
    rg.gain[RG_ALBUM] = -6.0206f;

    CHECK_NEAR(replay_gain_calc_multiplier(&cfg, &rg), 0.5f);
}

/* No tags at all: the default gain applies and skips peak protection. */
static void test_default_gain(void)
{
    ReplayGainConfig cfg = base_cfg();
    ReplayGain rg = empty_rg();

    cfg.mode = RG_TRACK;
    CHECK_NEAR(replay_gain_calc_multiplier(&cfg, &rg),
               powf(10.f, -7.f / 20.f));

    cfg.default_gain = 6.0206f;
    CHECK_NEAR(replay_gain_calc_multiplier(&cfg, &rg), 2.f);
}

static void test_preamp(void)
{
    ReplayGainConfig cfg = base_cfg();
    ReplayGain rg = empty_rg();

    cfg.mode = RG_TRACK;
    rg.have_gain[RG_TRACK] = 1;
    rg.gain[RG_TRACK] = 0.f;
    cfg.preamp = 3.f;
    cfg.peak_protection = 0;

    CHECK_NEAR(replay_gain_calc_multiplier(&cfg, &rg), powf(10.f, 0.15f));
}

/* Reference loudness is expected in LUFS; -18 LUFS is the neutral value. */
static void test_reference_loudness(void)
{
    ReplayGainConfig cfg = base_cfg();
    ReplayGain rg = empty_rg();

    cfg.mode = RG_TRACK;
    rg.have_gain[RG_TRACK] = 1;
    rg.gain[RG_TRACK] = 0.f;
    rg.have_reference_loudness = 1;
    rg.reference_loudness = -23.f; /* +5 dB correction */
    cfg.peak_protection = 0;

    CHECK_NEAR(replay_gain_calc_multiplier(&cfg, &rg), powf(10.f, 0.25f));
}

/*
 * Peak protection from the VLC comment: peak 0.5 -> max +6.02 dB,
 * peak 1.0 -> max 0 dB, peak 2.0 -> max -6.02 dB; a missing peak counts as
 * 1.0. Disabling protection restores the raw multiplier.
 */
static void test_peak_protection(void)
{
    ReplayGainConfig cfg = base_cfg();
    ReplayGain rg = empty_rg();

    cfg.mode = RG_TRACK;
    rg.have_gain[RG_TRACK] = 1;
    rg.gain[RG_TRACK] = 6.0206f;

    rg.have_peak[RG_TRACK] = 1;
    rg.peak[RG_TRACK] = 0.5f;
    CHECK_NEAR(replay_gain_calc_multiplier(&cfg, &rg), 2.f);

    rg.peak[RG_TRACK] = 1.f;
    CHECK_NEAR(replay_gain_calc_multiplier(&cfg, &rg), 1.f);

    rg.peak[RG_TRACK] = 2.f;
    CHECK_NEAR(replay_gain_calc_multiplier(&cfg, &rg), 0.5f);

    rg.have_peak[RG_TRACK] = 0; /* missing peak -> treated as 1.0 */
    CHECK_NEAR(replay_gain_calc_multiplier(&cfg, &rg), 1.f);

    cfg.peak_protection = 0;
    CHECK_NEAR(replay_gain_calc_multiplier(&cfg, &rg), 2.f);
}

/* merge only fills values that are absent in the destination. */
static void test_merge(void)
{
    ReplayGain dst = empty_rg(), src = empty_rg();

    dst.have_gain[RG_TRACK] = 1;
    dst.gain[RG_TRACK] = -3.f;
    src.have_gain[RG_TRACK] = 1;
    src.gain[RG_TRACK] = 5.f;
    src.have_gain[RG_ALBUM] = 1;
    src.gain[RG_ALBUM] = 6.f;
    src.have_peak[RG_ALBUM] = 1;
    src.peak[RG_ALBUM] = 0.9f;

    replay_gain_merge(&dst, &src);
    CHECK_NEAR(dst.gain[RG_TRACK], -3.f);      /* dst wins          */
    CHECK_NEAR(dst.gain[RG_ALBUM], 6.f);       /* filled from src   */
    CHECK_NEAR(dst.peak[RG_ALBUM], 0.9f);
}

int main(void)
{
    test_mode_none();
    test_track_gain();
    test_album_fallback();
    test_default_gain();
    test_preamp();
    test_reference_loudness();
    test_peak_protection();
    test_merge();

    if (failures) {
        printf("%d test(s) FAILED\n", failures);
        return 1;
    }
    printf("replay gain tests: all passed\n");
    return 0;
}
