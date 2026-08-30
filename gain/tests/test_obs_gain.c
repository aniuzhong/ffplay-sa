/*
 * Unit tests for the OBS gain port: the verbatim-port math (audio_math.h,
 * gain_core) and the audio-action envelope state machine (audio_actions.c,
 * the apply_audio_actions port). The expected values come from the OBS
 * source comments and the dB conventions (20*log10: +6.0206 dB = x2).
 *
 * Pure C: no FFmpeg, no SDL - compiled by the test_obs_gain target.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "obs/audio_actions.h"
#include "obs/audio_math.h"
#include "obs/gain_core.h"

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

#define CHECK_EQ(actual, expected) \
    do { \
        long long a_ = (long long)(actual), e_ = (long long)(expected); \
        if (a_ != e_) { \
            printf("FAIL %s: got %lld, expected %lld (line %d)\n", \
                   #actual, a_, e_, __LINE__); \
            failures++; \
        } \
    } while (0)

/* ---- audio_math.h: dB <-> linear conversion ------------------------------ */

static void test_db_mul_conversions(void)
{
    static const float muls[] = { 0.001f, 0.25f, 0.5f, 1.0f, 2.0f, 8.0f, 100.0f };
    static const float dbs[]  = { -96.f, -60.f, -30.f, -6.f, -0.1f, 0.f, 3.f, 30.f };
    size_t i;

    CHECK_NEAR(obs_db_to_mul(0.0f), 1.0f);
    CHECK_NEAR(obs_db_to_mul(6.0206f), 2.0f);
    CHECK_NEAR(obs_mul_to_db(2.0f), 6.0206f);
    CHECK_NEAR(obs_mul_to_db(0.5f), -6.0206f);

    /* OBS audio-math.h edge rules: silence is -inf dB, non-finite dB is 0. */
    CHECK_EQ(obs_mul_to_db(0.0f) == -INFINITY, 1);
    CHECK_NEAR(obs_db_to_mul(-INFINITY), 0.0f);
    CHECK_NEAR(obs_db_to_mul(+INFINITY), 0.0f); /* isfinite fails -> 0 */
    CHECK_NEAR(obs_db_to_mul((float)NAN), 0.0f);

    /* Round trips. */
    for (i = 0; i < sizeof(muls) / sizeof(muls[0]); i++)
        CHECK_NEAR(obs_db_to_mul(obs_mul_to_db(muls[i])), muls[i]);
    for (i = 0; i < sizeof(dbs) / sizeof(dbs[0]); i++)
        CHECK_NEAR(obs_mul_to_db(obs_db_to_mul(dbs[i])), dbs[i]);
}

/* ---- gain_core: fold_volume (get_source_volume epsilon folding) ---------- */

static void test_fold_volume(void)
{
    /* close_float(x, y, EPSILON=1e-4): |x-y| <= 1e-4 collapses to 1.0/0.0. */
    CHECK_NEAR(obs_gain_fold_volume(1.0f), 1.0f);
    CHECK_NEAR(obs_gain_fold_volume(1.0f + 5e-5f), 1.0f);
    CHECK_NEAR(obs_gain_fold_volume(1.0f - 5e-5f), 1.0f);
    CHECK_NEAR(obs_gain_fold_volume(0.0f), 0.0f);
    CHECK_NEAR(obs_gain_fold_volume(-1e-5f), 0.0f);

    /* Outside the epsilon the raw value survives. */
    CHECK_NEAR(obs_gain_fold_volume(0.999f), 0.999f);
    CHECK_NEAR(obs_gain_fold_volume(0.5f), 0.5f);
    CHECK_NEAR(obs_gain_fold_volume(2.0f), 2.0f);
}

/* ---- gain_core: device sink clamp (clamp_audio_output rules) ------------- */

static void test_clamp(void)
{
    static const float in[]  = { 0.5f, -0.5f, 1.5f, -1.5f, 1.0f, -1.0f };
    static const float exp[] = { 0.5f, -0.5f, 1.0f, -1.0f, 1.0f, -1.0f };
    float v[6];
    float nan_v = NAN, inf_v = INFINITY, ninf_v = -INFINITY;
    size_t i;

    memcpy(v, in, sizeof(v));
    obs_gain_clamp(v, 6);
    for (i = 0; i < 6; i++)
        CHECK_NEAR(v[i], exp[i]);

    obs_gain_clamp(&nan_v, 1);   /* NaN collapses to silence */
    obs_gain_clamp(&inf_v, 1);
    obs_gain_clamp(&ninf_v, 1);
    CHECK_EQ(nan_v == 0.0f, 1);
    CHECK_EQ(inf_v == 1.0f, 1);
    CHECK_EQ(ninf_v == -1.0f, 1);
}

/* ---- gain_core: S16 <-> float bridge -------------------------------------- */

static void test_s16_float_bridge(void)
{
    static const int16_t in[] = { -32768, -12345, -1, 0, 1, 12345, 32767 };
    int16_t back[7];
    float f[7];
    size_t i;

    obs_gain_s16_to_float(in, f, 7);
    for (i = 0; i < 7; i++)
        CHECK_NEAR(f[i], (float)in[i] / 32768.0f);
    CHECK_EQ(f[0] == -1.0f, 1); /* -32768 maps exactly onto float full scale */

    obs_gain_float_to_s16(f, back, 7);
    for (i = 0; i < 7; i++)
        CHECK_EQ(back[i], in[i]); /* powers-of-two mapping round-trips exactly */

    /* Saturation at the float pipeline's clamped bounds. */
    CHECK_EQ((int)obs_gain_float_to_s16_one(1.0f), 32767);
    CHECK_EQ((int)obs_gain_float_to_s16_one(-1.0f), -32768);
    CHECK_EQ((int)obs_gain_float_to_s16_one(0.5f), 16384);
    CHECK_EQ((int)obs_gain_float_to_s16_one(-0.5f), -16384);
}

/* ---- gain_core: multiply kernels ------------------------------------------ */

static void test_mul_kernels(void)
{
    float s[3] = { 1.0f, 2.0f, -4.0f };
    float env[4] = { 1.0f, 2.0f, 3.0f, 4.0f }; /* 2 frames, 2 channels */
    float vol_data[2] = { 0.5f, 2.0f };

    obs_gain_mul_scalar(s, 3, 0.5f);
    CHECK_NEAR(s[0], 0.5f);
    CHECK_NEAR(s[1], 1.0f);
    CHECK_NEAR(s[2], -2.0f);

    obs_gain_mul_envelope(env, 2, 2, vol_data);
    CHECK_NEAR(env[0], 0.5f);  /* frame 0, ch 0 */
    CHECK_NEAR(env[1], 1.0f);  /* frame 0, ch 1 */
    CHECK_NEAR(env[2], 6.0f);  /* frame 1, ch 0 */
    CHECK_NEAR(env[3], 8.0f);  /* frame 1, ch 1 */
}

/* ---- audio_actions: apply_audio_actions envelope expansion ---------------- */

static void fill_actions(ObsGainActions *a, const uint64_t *frames, const float *muls, int n)
{
    int i;

    obs_gain_actions_init(a);
    for (i = 0; i < n; i++)
        obs_gain_actions_push(a, frames[i], muls[i]);
}

static void test_expand_no_events(void)
{
    ObsGainActions a;
    float vol_data[OBS_GAIN_BLOCK_FRAMES];
    float out;
    size_t i;

    obs_gain_actions_init(&a);
    out = obs_gain_actions_expand(&a, 1000, OBS_GAIN_BLOCK_FRAMES, 0.5f, vol_data);
    CHECK_EQ(a.num, 0);
    for (i = 0; i < OBS_GAIN_BLOCK_FRAMES; i++)
        CHECK_EQ(vol_data[i] == 0.5f, 1);
    CHECK_NEAR(out, 0.5f);
    obs_gain_actions_free(&a);
}

/* Single event inside the block: old volume up to its frame, new from it. */
static void test_expand_single_step(void)
{
    ObsGainActions a;
    uint64_t frames[] = { 1000 + 512 };
    float muls[] = { 0.5f };
    float vol_data[OBS_GAIN_BLOCK_FRAMES];
    float out;
    size_t i;

    fill_actions(&a, frames, muls, 1);
    out = obs_gain_actions_expand(&a, 1000, OBS_GAIN_BLOCK_FRAMES, 1.0f, vol_data);
    CHECK_EQ(a.num, 0);                 /* consumed */
    CHECK_NEAR(out, 0.5f);              /* raw stored factor handed back */
    for (i = 0; i < 512; i++)
        CHECK_EQ(vol_data[i] == 1.0f, 1);
    for (i = 512; i < OBS_GAIN_BLOCK_FRAMES; i++)
        CHECK_EQ(vol_data[i] == 0.5f, 1);
    obs_gain_actions_free(&a);
}

/* OBS: timestamp behind audio_ts clamps to the block start. */
static void test_expand_late_event_clamps_to_block_start(void)
{
    ObsGainActions a;
    uint64_t frames[] = { 500 }; /* block starts at 1000 */
    float muls[] = { 0.25f };
    float vol_data[OBS_GAIN_BLOCK_FRAMES];
    float out;
    size_t i;

    fill_actions(&a, frames, muls, 1);
    out = obs_gain_actions_expand(&a, 1000, OBS_GAIN_BLOCK_FRAMES, 1.0f, vol_data);
    CHECK_EQ(a.num, 0);
    CHECK_NEAR(out, 0.25f);
    for (i = 0; i < OBS_GAIN_BLOCK_FRAMES; i++)
        CHECK_EQ(vol_data[i] == 0.25f, 1);
    obs_gain_actions_free(&a);
}

/* Two events in one block: three runs of constant volume. */
static void test_expand_multiple_steps(void)
{
    ObsGainActions a;
    uint64_t frames[] = { 1000 + 100, 1000 + 800 };
    float muls[] = { 0.5f, 2.0f };
    float vol_data[OBS_GAIN_BLOCK_FRAMES];
    float out;
    size_t i;

    fill_actions(&a, frames, muls, 2);
    out = obs_gain_actions_expand(&a, 1000, OBS_GAIN_BLOCK_FRAMES, 1.0f, vol_data);
    CHECK_EQ(a.num, 0);
    CHECK_NEAR(out, 2.0f);
    for (i = 0; i < 100; i++)
        CHECK_EQ(vol_data[i] == 1.0f, 1);
    for (i = 100; i < 800; i++)
        CHECK_EQ(vol_data[i] == 0.5f, 1);
    for (i = 800; i < OBS_GAIN_BLOCK_FRAMES; i++)
        CHECK_EQ(vol_data[i] == 2.0f, 1);
    obs_gain_actions_free(&a);
}

/* Two events on the same frame: the later one wins (OBS applies in queue
 * order; the second apply overwrites the first before any fill uses it). */
static void test_expand_same_frame_later_wins(void)
{
    ObsGainActions a;
    uint64_t frames[] = { 1000 + 500, 1000 + 500 };
    float muls[] = { 0.5f, 0.25f };
    float vol_data[OBS_GAIN_BLOCK_FRAMES];
    float out;
    size_t i;

    fill_actions(&a, frames, muls, 2);
    out = obs_gain_actions_expand(&a, 1000, OBS_GAIN_BLOCK_FRAMES, 1.0f, vol_data);
    CHECK_EQ(a.num, 0);
    CHECK_NEAR(out, 0.25f);
    for (i = 0; i < 500; i++)
        CHECK_EQ(vol_data[i] == 1.0f, 1);
    for (i = 500; i < OBS_GAIN_BLOCK_FRAMES; i++)
        CHECK_EQ(vol_data[i] == 0.25f, 1);
    obs_gain_actions_free(&a);
}

/* Event beyond the block end stays queued (OBS: the loop breaks) and is
 * applied by a later block. */
static void test_expand_event_stays_queued(void)
{
    ObsGainActions a;
    uint64_t frames[] = { 1000 + 1500 };
    float muls[] = { 0.5f };
    float vol_data[OBS_GAIN_BLOCK_FRAMES];
    float out;
    size_t i;

    fill_actions(&a, frames, muls, 1);
    out = obs_gain_actions_expand(&a, 1000, OBS_GAIN_BLOCK_FRAMES, 1.0f, vol_data);
    CHECK_EQ(a.num, 1);                 /* NOT consumed */
    CHECK_NEAR(out, 1.0f);              /* storage unchanged */
    for (i = 0; i < OBS_GAIN_BLOCK_FRAMES; i++)
        CHECK_EQ(vol_data[i] == 1.0f, 1);

    /* Next block (2024..3048) covers event frame 2500; the step lands at
     * 2500 - 2024 = 476 within it. */
    out = obs_gain_actions_expand(&a, 1000 + OBS_GAIN_BLOCK_FRAMES, OBS_GAIN_BLOCK_FRAMES, 1.0f,
                                  vol_data);
    CHECK_EQ(a.num, 0);
    CHECK_NEAR(out, 0.5f);
    for (i = 0; i < 476; i++)
        CHECK_EQ(vol_data[i] == 1.0f, 1);
    for (i = 476; i < OBS_GAIN_BLOCK_FRAMES; i++)
        CHECK_EQ(vol_data[i] == 0.5f, 1);
    obs_gain_actions_free(&a);
}

/* OBS queue-order semantics: an out-of-block action stops the loop even if
 * a later queued action would fall inside the block (index-order break). */
static void test_expand_queue_order_break(void)
{
    ObsGainActions a;
    uint64_t frames[] = { 1000 + 1500, 1000 + 200 }; /* first is out of block */
    float muls[] = { 0.5f, 0.25f };
    float vol_data[OBS_GAIN_BLOCK_FRAMES];
    float out;
    size_t i;

    fill_actions(&a, frames, muls, 2);
    out = obs_gain_actions_expand(&a, 1000, OBS_GAIN_BLOCK_FRAMES, 1.0f, vol_data);
    CHECK_EQ(a.num, 2);                 /* nothing consumed */
    CHECK_NEAR(out, 1.0f);
    for (i = 0; i < OBS_GAIN_BLOCK_FRAMES; i++)
        CHECK_EQ(vol_data[i] == 1.0f, 1);
    obs_gain_actions_free(&a);
}

/* Partial block (the callback-boundary edge): the step beyond it stays
 * queued, a step inside it lands relative to the block start. */
static void test_expand_partial_block(void)
{
    ObsGainActions a;
    uint64_t frames[] = { 1000 + 150, 1000 + 500 };
    float muls[] = { 0.5f, 0.25f };
    float vol_data[OBS_GAIN_BLOCK_FRAMES];
    float out;
    size_t i;

    fill_actions(&a, frames, muls, 2);
    out = obs_gain_actions_expand(&a, 1000, 300, 1.0f, vol_data);
    CHECK_EQ(a.num, 1);                 /* the @500 event survives */
    CHECK_NEAR(out, 0.5f);
    for (i = 0; i < 150; i++)
        CHECK_EQ(vol_data[i] == 1.0f, 1);
    for (i = 150; i < 300; i++)
        CHECK_EQ(vol_data[i] == 0.5f, 1);

    /* Surviving event is relative to the absolute frame, not re-offset. */
    out = obs_gain_actions_expand(&a, 1300, OBS_GAIN_BLOCK_FRAMES, 0.5f, vol_data);
    CHECK_EQ(a.num, 0);
    CHECK_NEAR(out, 0.25f);
    for (i = 0; i < 200; i++)           /* 1500 - 1300 = 200 */
        CHECK_EQ(vol_data[i] == 0.5f, 1);
    for (i = 200; i < OBS_GAIN_BLOCK_FRAMES; i++)
        CHECK_EQ(vol_data[i] == 0.25f, 1);
    obs_gain_actions_free(&a);
}

/* cur_mul is folded on read: a near-unity stored factor seeds the envelope
 * at exactly 1.0, and the returned storage is the raw event factor. */
static void test_expand_folds_current_volume(void)
{
    ObsGainActions a;
    uint64_t frames[] = { 1000 + 512 };
    float muls[] = { 0.5f };
    float vol_data[OBS_GAIN_BLOCK_FRAMES];
    float out;

    fill_actions(&a, frames, muls, 1);
    out = obs_gain_actions_expand(&a, 1000, OBS_GAIN_BLOCK_FRAMES, 1.0f - 5e-5f, vol_data);
    CHECK_EQ(vol_data[0] == 1.0f, 1);   /* 1 - 5e-5 folds to 1.0 */
    CHECK_NEAR(out, 0.5f);              /* raw storage handed back */
    obs_gain_actions_free(&a);
}

int main(void)
{
    test_db_mul_conversions();
    test_fold_volume();
    test_clamp();
    test_s16_float_bridge();
    test_mul_kernels();
    test_expand_no_events();
    test_expand_single_step();
    test_expand_late_event_clamps_to_block_start();
    test_expand_multiple_steps();
    test_expand_same_frame_later_wins();
    test_expand_event_stays_queued();
    test_expand_queue_order_break();
    test_expand_partial_block();
    test_expand_folds_current_volume();

    if (failures) {
        printf("%d test(s) FAILED\n", failures);
        return 1;
    }
    printf("obs gain tests: all passed\n");
    return 0;
}
