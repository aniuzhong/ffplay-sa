#include <math.h>
#include <string.h>

#include "gain_core.h"

#define OBS_EPSILON 1e-4f /* graphics/math-defs.h EPSILON */

void obs_gain_clamp(float *samples, size_t n)
{
	size_t i;

	/* clamp_audio_output (libobs/media-io/audio-io.c): the mix is clamped
	 * to -1.0..1.0 exactly once, at the output stage. */
	for (i = 0; i < n; i++) {
		float val = samples[i];
		val = (val == val) ? val : 0.0f;   /* NaN -> silence */
		val = (val > 1.0f) ? 1.0f : val;
		val = (val < -1.0f) ? -1.0f : val;
		samples[i] = val;
	}
}

void obs_gain_mul_scalar(float *samples, size_t n, float multiple)
{
	size_t i;

	/* gain_filter_audio (plugins/obs-filters/gain-filter.c) and
	 * multiply_output_audio (libobs/obs-source.c): one multiply per
	 * sample, in place, no clamping (the float pipeline defers that). */
	for (i = 0; i < n; i++)
		samples[i] *= multiple;
}

void obs_gain_mul_envelope(float *interleaved, size_t frames, int channels, const float *vol_data)
{
	size_t frame;
	int ch;

	/* multiply_vol_data (libobs/obs-source.c), interleaved adaptation:
	 * one factor per frame, applied to every channel of that frame. */
	for (frame = 0; frame < frames; frame++) {
		float vol = vol_data[frame];
		float *s = interleaved + (size_t)frame * channels;

		for (ch = 0; ch < channels; ch++)
			s[ch] *= vol;
	}
}

float obs_gain_fold_volume(float volume)
{
	/* get_source_volume (libobs/obs-source.c) minus the mute/push-to-talk
	 * state this port does not have: fold through close_float so the
	 * unity/zero fast paths can trigger on near-unity factors. */
	if (fabsf(volume - 0.0f) <= OBS_EPSILON)
		return 0.0f;
	if (fabsf(volume - 1.0f) <= OBS_EPSILON)
		return 1.0f;
	return volume;
}

void obs_gain_s16_to_float(const int16_t *in, float *out, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		out[i] = (float)in[i] / 32768.0f;
}

void obs_gain_float_to_s16(const float *in, int16_t *out, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		float scaled = roundf(in[i] * 32768.0f);

		if (scaled != scaled)          /* NaN: the pipeline clamps before
						* this bridge; stay defined anyway */
			scaled = 0.0f;
		else if (scaled > 32767.0f)
			scaled = 32767.0f;
		else if (scaled < -32768.0f)
			scaled = -32768.0f;
		out[i] = (int16_t)scaled;
	}
}
