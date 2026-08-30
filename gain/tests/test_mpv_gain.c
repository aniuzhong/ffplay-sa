/*
 * Unit tests for the mpv gain port (gain/mpv/), plus an informational
 * multiplier cross-comparison against the VLC port (gain/vlc/).
 *
 * The expected values come from the ported mpv 0.41.0 semantics:
 *   - player/audio.c audio_get_gain(): (volume/100)^3 * replaygain * 10^(dB/20)
 *   - demux/demux.c decode_rgain(): three tag generations, R128 Q7.8 /256
 *     plus the +5 dB ReplayGain-2 (=-18 LUFS) vs EBU R128 (=-23 LUFS)
 *     compensation
 *   - audio/out/ao.c MUL_GAIN_i: Q8.8 fixed point, +128 rounds half toward
 *     +infinity, negative samples shift arithmetically (toward -infinity),
 *     MPCLAMP guards the integer range; unity gain skips entirely
 *
 * Pure C: no FFmpeg dependency, compiled by the test_mpv_gain target.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "mpv/mpv_kernels.h"
#include "mpv/mpv_replaygain.h"
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

#define CHECK_EQ(actual, expected) \
    do { \
        long long a_ = (long long)(actual), e_ = (long long)(expected); \
        if (a_ != e_) { \
            printf("FAIL %s: got %lld, expected %lld (line %d)\n", \
                   #actual, a_, e_, __LINE__); \
            failures++; \
        } \
    } while (0)

#define CHECK_TRUE(cond) \
    do { \
        if (!(cond)) { \
            printf("FAIL %s (line %d)\n", #cond, __LINE__); \
            failures++; \
        } \
    } while (0)

/* mpv default config: --volume=100 --volume-gain=0, no mute, no replaygain. */
static MpvGainConfig base_cfg(void)
{
    MpvGainConfig cfg;

    cfg.volume         = 100.f;
    cfg.volume_gain    = 0.f;
    cfg.mute           = 0;
    cfg.rgain_mode     = 0;
    cfg.rgain_preamp   = 0.f;
    cfg.rgain_clip     = 0;
    cfg.rgain_fallback = 0.f;
    return cfg;
}

static void test_db_gain(void)
{
    CHECK_NEAR(mpv_db_gain(0.0), 1.0);
    CHECK_NEAR(mpv_db_gain(6.0206), 2.0);
    CHECK_NEAR(mpv_db_gain(-6.0206), 0.5);
}

/* The perceptual cubic curve: 100% = unity, 50% = 1/8, 0% = silence. */
static void test_cubic_volume_curve(void)
{
    MpvGainConfig cfg = base_cfg();

    CHECK_NEAR(mpv_audio_get_gain(&cfg, NULL), 1.0);
    cfg.volume = 50.f;
    CHECK_NEAR(mpv_audio_get_gain(&cfg, NULL), 0.125);
    cfg.volume = 130.f;
    CHECK_NEAR(mpv_audio_get_gain(&cfg, NULL), pow((double)(float)1.3, 3.0));
    cfg.volume = 0.f;
    CHECK_NEAR(mpv_audio_get_gain(&cfg, NULL), 0.0);
    /* MPMAX(volume/100, 0): negative legacy values are silence, not boost. */
    cfg.volume = -5.f;
    CHECK_NEAR(mpv_audio_get_gain(&cfg, NULL), 0.0);
}

/* --volume-gain is an additive dB stage on top of the curve. */
static void test_volume_gain(void)
{
    MpvGainConfig cfg = base_cfg();

    cfg.volume_gain = 6.0206f;
    CHECK_NEAR(mpv_audio_get_gain(&cfg, NULL), 2.0);
    cfg.volume_gain = -6.0206f;
    CHECK_NEAR(mpv_audio_get_gain(&cfg, NULL), 0.5);
}

static void test_mute(void)
{
    MpvGainConfig cfg = base_cfg();

    cfg.mute = 1;
    CHECK_NEAR(mpv_audio_get_gain(&cfg, NULL), 0.0);
}

/* Clipping prevention clamps to 1/peak unless --replaygain-clip is set. */
static void test_replaygain_track(void)
{
    MpvGainConfig cfg = base_cfg();
    MpvReplayGain rg;

    cfg.rgain_mode = 1;
    memset(&rg, 0, sizeof(rg));
    rg.track_gain = 6.0206f;
    rg.track_peak = 0.5f;

    /* 1/0.5 = 2.0, exactly the tag gain: no extra clamp. */
    CHECK_NEAR(mpv_compute_replaygain(&cfg, &rg), 2.0);
    CHECK_NEAR(mpv_audio_get_gain(&cfg, &rg), 2.0);

    rg.track_peak = 0.9f;
    CHECK_NEAR(mpv_compute_replaygain(&cfg, &rg), 1.0 / 0.9);

    cfg.rgain_clip = 1; /* allow clipping: raw tag gain wins */
    CHECK_NEAR(mpv_compute_replaygain(&cfg, &rg), 2.0);
}

static void test_replaygain_album(void)
{
    MpvGainConfig cfg = base_cfg();
    MpvReplayGain rg;

    cfg.rgain_mode = 2;
    memset(&rg, 0, sizeof(rg));
    rg.album_gain = -6.0206f;
    rg.album_peak = 2.0f;

    CHECK_NEAR(mpv_compute_replaygain(&cfg, &rg), 0.5);
}

static void test_preamp(void)
{
    MpvGainConfig cfg = base_cfg();
    MpvReplayGain rg;

    cfg.rgain_mode = 1;
    cfg.rgain_preamp = 3.f;
    memset(&rg, 0, sizeof(rg));
    rg.track_gain = 0.0f;
    rg.track_peak = 0.5f; /* 1/0.5 = 2.0, above the preamped gain: no clamp */

    CHECK_NEAR(mpv_compute_replaygain(&cfg, &rg), pow(10.0, 3.0 / 20.0));
}

/*
 * mpv's --replaygain-fallback semantics, kept verbatim: it applies whenever
 * the replaygain logic is inactive (mode "no" included), and a value of 0
 * disables it because the upstream check is a plain C falsy test.
 */
static void test_fallback_semantics(void)
{
    MpvGainConfig cfg = base_cfg();

    cfg.rgain_fallback = -7.f;
    /* mode "no" with no tags: the fallback still applies (mpv behavior). */
    CHECK_NEAR(mpv_audio_get_gain(&cfg, NULL), pow(10.0, -7.0 / 20.0));

    /* mode track but no tag data: fallback applies. */
    cfg.rgain_mode = 1;
    CHECK_NEAR(mpv_audio_get_gain(&cfg, NULL), pow(10.0, -7.0 / 20.0));

    /* tag data present: fallback is ignored. */
    {
        MpvReplayGain rg;
        memset(&rg, 0, sizeof(rg));
        rg.track_gain = 0.0f;
        rg.track_peak = 1.0f;
        CHECK_NEAR(mpv_audio_get_gain(&cfg, &rg), 1.0);
    }

    /* default 0: the fallback stage is disabled entirely. */
    cfg.rgain_mode = 1;
    cfg.rgain_fallback = 0.f;
    CHECK_NEAR(mpv_audio_get_gain(&cfg, NULL), 1.0);
}

static void test_decode_rgain(void)
{
    MpvReplayGain rg;
    MpvRgTags tags;

    /* Classic tags: the " dB" suffix is tolerated (strtod), album falls
     * back to track when undefined. */
    memset(&tags, 0, sizeof(tags));
    tags.track_gain = "-3.2 dB";
    tags.track_peak = "0.8";
    CHECK_TRUE(mpv_decode_rgain(&rg, &tags));
    CHECK_NEAR(rg.track_gain, -3.2);
    CHECK_NEAR(rg.track_peak, 0.8);
    CHECK_NEAR(rg.album_gain, -3.2);
    CHECK_NEAR(rg.album_peak, 0.8);

    /* A present but invalid peak disqualifies the whole classic set. */
    memset(&tags, 0, sizeof(tags));
    tags.track_gain = "-3.2 dB";
    tags.track_peak = "0";
    CHECK_TRUE(!mpv_decode_rgain(&rg, &tags));

    /* New-style tags. */
    memset(&tags, 0, sizeof(tags));
    tags.rg_gain = "-6.02";
    tags.rg_peak = "0.5";
    CHECK_TRUE(mpv_decode_rgain(&rg, &tags));
    CHECK_NEAR(rg.track_gain, -6.02);
    CHECK_NEAR(rg.track_peak, 0.5);
    CHECK_NEAR(rg.album_gain, -6.02);

    /* Opus R128: Q7.8 fixed point (÷256) plus 5 dB reference compensation
     * (ReplayGain 2 = -18 LUFS vs EBU R128 = -23 LUFS); no peaks. */
    memset(&tags, 0, sizeof(tags));
    tags.r128_track = "1024"; /* 4 dB + 5 dB compensation */
    CHECK_TRUE(mpv_decode_rgain(&rg, &tags));
    CHECK_NEAR(rg.track_gain, 9.0);
    CHECK_NEAR(rg.album_gain, 9.0);
    CHECK_NEAR(rg.track_peak, 0.0);

    memset(&tags, 0, sizeof(tags));
    tags.r128_track = "256";
    tags.r128_album = "512";
    CHECK_TRUE(mpv_decode_rgain(&rg, &tags));
    CHECK_NEAR(rg.track_gain, 6.0);
    CHECK_NEAR(rg.album_gain, 7.0);

    /* Nothing recognized. */
    memset(&tags, 0, sizeof(tags));
    CHECK_TRUE(!mpv_decode_rgain(&rg, &tags));
}

/* The Q8.8 kernel: +128 rounds half toward +inf; negative samples shift
 * arithmetically (floor), so ties round up on the positive side only. */
static void test_kernel_s16(void)
{
    int16_t buf[8];
    int i;

    /* gain 0.5 -> gi=128: 1000 -> (128000+128)>>8 = 500 */
    for (i = 0; i < 8; i++)
        buf[i] = (int16_t)(i % 2 ? -1000 : 1000);
    mpv_process_plane(MPV_AFMT_S16, buf, 8, 0.5f);
    for (i = 0; i < 8; i++)
        CHECK_EQ(buf[i], i % 2 ? -500 : 500);

    /* Rounding ties: +1001*0.5 = 500.5 -> 501, -1001*0.5 = -500.5 -> -500
     * (the +128 bias only rounds the magnitude; the floor shift pulls
     * negatives down). */
    buf[0] = 1001;
    buf[1] = -1001;
    mpv_process_plane(MPV_AFMT_S16, buf, 2, 0.5f);
    CHECK_EQ(buf[0], 501);
    CHECK_EQ(buf[1], -500);

    /* Clamping at full scale, gain 2.0 (gi=512). */
    buf[0] = 32000;
    buf[1] = -32000;
    mpv_process_plane(MPV_AFMT_S16, buf, 2, 2.0f);
    CHECK_EQ(buf[0], INT16_MAX);
    CHECK_EQ(buf[1], INT16_MIN);

    /* Unity gain: gi == 256, the kernel skips entirely. */
    buf[0] = 12345;
    mpv_process_plane(MPV_AFMT_S16, buf, 1, 1.0f);
    CHECK_EQ(buf[0], 12345);
}

static void test_kernel_u8(void)
{
    uint8_t buf[4];

    /* Center 128 is the unsigned silence point; gain 2.0 keeps it. */
    buf[0] = 128;
    buf[1] = 200; /* 72 * 2 = 144 -> 128 + 144 = 272 -> clamp 255 */
    buf[2] = 50;  /* -78 * 2 = -156 -> 128 - 156 = -28 -> clamp 0 */
    buf[3] = 0;
    mpv_process_plane(MPV_AFMT_U8, buf, 4, 2.0f);
    CHECK_EQ(buf[0], 128);
    CHECK_EQ(buf[1], 255);
    CHECK_EQ(buf[2], 0);
    CHECK_EQ(buf[3], 0);
}

static void test_kernel_float(void)
{
    float buf[2];

    buf[0] = 0.25f;
    buf[1] = 1.5f; /* the float path has no clamp, like upstream */
    mpv_process_plane(MPV_AFMT_FLOAT, buf, 2, 2.0f);
    CHECK_NEAR(buf[0], 0.5);
    CHECK_NEAR(buf[1], 3.0);
}

static void test_kernel_s32(void)
{
    int32_t buf[2];

    /* 1e9 * 2 = 2e9 still fits in int32; 2e9 * 2 clamps. */
    buf[0] = 1000000000;
    buf[1] = 2000000000;
    mpv_process_plane(MPV_AFMT_S32, buf, 2, 2.0f);
    CHECK_EQ(buf[0], 2000000000LL);
    CHECK_EQ(buf[1], INT32_MAX);
}

/* ao_post_process_data semantics: interleaved counts frames*channels,
 * planar walks one plane at a time. */
static void test_post_process_layouts(void)
{
    int16_t inter[6];
    int16_t planar_l[3] = { 1000, 1000, 1000 };
    int16_t planar_r[3] = { -1000, -1000, -1000 };
    void *planes[2];
    void *inter_plane = inter; /* interleaved: one plane pointing at the buffer */
    int i;

    for (i = 0; i < 6; i++)
        inter[i] = 1000;
    mpv_post_process_data(MPV_AFMT_S16, 0, 2, &inter_plane, 3, 0.5f);
    for (i = 0; i < 6; i++)
        CHECK_EQ(inter[i], 500);

    planes[0] = planar_l;
    planes[1] = planar_r;
    mpv_post_process_data(MPV_AFMT_S16, 1, 2, planes, 3, 0.5f);
    for (i = 0; i < 3; i++) {
        CHECK_EQ(planar_l[i], 500);
        CHECK_EQ(planar_r[i], -500);
    }
}

/* --- informational: mpv vs VLC multiplier cross-comparison --------------- */

static void cross_table_row(const char *name, float mpv_mult, float vlc_mult)
{
    printf("  %-34s mpv=%.6f (%+6.2f dB)  vlc=%.6f (%+6.2f dB)  delta=%+6.2f dB\n",
           name, mpv_mult, 20.0 * log10(mpv_mult > 0 ? mpv_mult : 1e-9),
           vlc_mult, 20.0 * log10(vlc_mult > 0 ? vlc_mult : 1e-9),
           20.0 * (log10(mpv_mult > 0 ? mpv_mult : 1e-9) -
                   log10(vlc_mult > 0 ? vlc_mult : 1e-9)));
}

static void test_vlc_cross_comparison(void)
{
    MpvGainConfig mcfg = base_cfg();
    MpvReplayGain mrg;
    MpvRgTags tags;
    ReplayGainConfig vcfg;
    ReplayGain vrg;

    printf("mpv vs VLC multiplier comparison (mode=track, preamp=0):\n");

    /* S1: classic track tags, peak below tag gain. */
    memset(&tags, 0, sizeof(tags));
    tags.track_gain = "-6.02 dB";
    tags.track_peak = "0.9";
    CHECK_TRUE(mpv_decode_rgain(&mrg, &tags));
    mcfg.rgain_mode = 1;

    vcfg.mode = RG_TRACK;
    vcfg.preamp = 0.f;
    vcfg.default_gain = -7.f;
    vcfg.peak_protection = 1;
    vcfg.gain = 1.f;
    memset(&vrg, 0, sizeof(vrg));
    vrg.have_gain[RG_TRACK] = 1;
    vrg.gain[RG_TRACK] = -6.02f;
    vrg.have_peak[RG_TRACK] = 1;
    vrg.peak[RG_TRACK] = 0.9f;

    cross_table_row("track tags, peak 0.9",
                    mpv_audio_get_gain(&mcfg, &mrg),
                    replay_gain_calc_multiplier(&vcfg, &vrg));

    /* S2: no tags at all — mpv's fallback defaults to off, VLC to -7 dB. */
    cross_table_row("no tags (mpv fallback off, vlc -7 dB)",
                    mpv_audio_get_gain(&mcfg, NULL),
                    replay_gain_calc_multiplier(&vcfg, &vrg));

    /* S3: old REPLAYGAIN_REFERENCE_LOUDNESS=89 tags — VLC treats it as LUFS
     * and shifts by -18-89 dB; mpv ignores the tag entirely. */
    vrg.have_reference_loudness = 1;
    vrg.reference_loudness = 89.f;
    cross_table_row("REFERENCE_LOUDNESS=89 (dB SPL)",
                    mpv_audio_get_gain(&mcfg, &mrg),
                    replay_gain_calc_multiplier(&vcfg, &vrg));
    vrg.have_reference_loudness = 0;

    /* S4: Opus R128 only — mpv decodes it (Q7.8 + 5 dB), VLC has no
     * support and falls back to its default gain. */
    memset(&tags, 0, sizeof(tags));
    tags.r128_track = "1024";
    CHECK_TRUE(mpv_decode_rgain(&mrg, &tags));
    cross_table_row("R128_TRACK_GAIN=1024 (opus)",
                    mpv_audio_get_gain(&mcfg, &mrg),
                    replay_gain_calc_multiplier(&vcfg, &vrg));

    printf("  (row 'no tags'/'R128' deltas are behavioral differences, not bugs)\n");
}

int main(void)
{
    test_db_gain();
    test_cubic_volume_curve();
    test_volume_gain();
    test_mute();
    test_replaygain_track();
    test_replaygain_album();
    test_preamp();
    test_fallback_semantics();
    test_decode_rgain();
    test_kernel_s16();
    test_kernel_u8();
    test_kernel_float();
    test_kernel_s32();
    test_post_process_layouts();
    test_vlc_cross_comparison();

    if (failures) {
        printf("%d test(s) FAILED\n", failures);
        return 1;
    }
    printf("mpv gain tests: all passed\n");
    return 0;
}
