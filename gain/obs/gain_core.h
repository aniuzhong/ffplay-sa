/*
 * OBS Studio gain kernels, ported for the ffplay gain experiment (GPL-2.0+).
 *
 * Kernels are copied from:
 * - libobs/media-io/audio-io.c  clamp_audio_output()  -> obs_gain_clamp()
 * - libobs/obs-source.c         multiply_output_audio() /
 *                               multiply_vol_data()   -> obs_gain_mul_*()
 * - libobs/obs-source.c         get_source_volume() epsilon folding
 *                               (close_float, graphics/math-defs.h EPSILON)
 *                               -> obs_gain_fold_volume()
 *
 * One adaptation the port cannot avoid: OBS runs an all-float pipeline and
 * ffplay forces the SDL device to AUDIO_S16SYS, so the port adds an S16
 * <-> float wrapper (obs_gain_s16_to_float / obs_gain_float_to_s16).  The
 * mapping is the powers-of-two convention, which round-trips exactly:
 * float = s16 / 32768.0f; s16 = clamp(roundf(float * 32768.0f)).
 */

#ifndef OBS_GAIN_CORE_H
#define OBS_GAIN_CORE_H

#include <stddef.h>
#include <stdint.h>

/* OBS clamps the mixed float output once, at the device sink (audio-io.c).
 * NaN collapses to silence; everything else saturates to [-1, 1]. */
void obs_gain_clamp(float *samples, size_t n);

/* Constant-volume kernel: libobs multiplies a whole mix buffer by one factor
 * and skips the multiply entirely at unity (apply_audio_volume fast paths:
 * vol == 1.0f -> return, vol == 0.0f -> memset).  Unity/zero shortcuts are
 * the caller's job; this is always the raw multiply. */
void obs_gain_mul_scalar(float *samples, size_t n, float multiple);

/* Volume-envelope kernel: multiply_vol_data() applies one factor per frame.
 * OBS buffers are planar (one plane per channel); ffplay's device stream is
 * interleaved, so the per-frame factor fans out over `channels` interleaved
 * samples here - mathematically the same multiply per frame and channel. */
void obs_gain_mul_envelope(float *interleaved, size_t frames, int channels, const float *vol_data);

/* get_source_volume() folds the stored factor through close_float(x, y,
 * EPSILON) with EPSILON = 1e-4 (graphics/math-defs.h): values within the
 * epsilon of unity/silence collapse to exactly 1.0f/0.0f so the fast paths
 * trigger.  Storage keeps the raw value; folding happens on read, as in OBS. */
float obs_gain_fold_volume(float volume);

/* S16 <-> float bridge (see header comment for the mapping). */
void obs_gain_s16_to_float(const int16_t *in, float *out, size_t n);
void obs_gain_float_to_s16(const float *in, int16_t *out, size_t n);

/* Single-sample convenience (tests, docs). */
static inline int16_t obs_gain_float_to_s16_one(float v)
{
	int16_t out;
	obs_gain_float_to_s16(&v, &out, 1);
	return out;
}

#endif /* OBS_GAIN_CORE_H */
