/*
 * Ported from OBS Studio libobs/media-io/audio-math.h (GPL-2.0+, Lain Bailey).
 * The two conversion functions are copied verbatim; only the c99defs.h
 * include was dropped (nothing here needs it) so this header is standalone.
 */

#ifndef OBS_AUDIO_MATH_H
#define OBS_AUDIO_MATH_H

#include <math.h>

#ifdef _MSC_VER
#include <float.h>

#pragma warning(push)
#pragma warning(disable : 4056)
#pragma warning(disable : 4756)
#endif

static inline float obs_mul_to_db(const float mul)
{
	return (mul == 0.0f) ? -INFINITY : (20.0f * log10f(mul));
}

static inline float obs_db_to_mul(const float db)
{
	return isfinite((double)db) ? powf(10.0f, db / 20.0f) : 0.0f;
}

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif /* OBS_AUDIO_MATH_H */
