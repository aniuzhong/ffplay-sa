#include <stdlib.h>
#include <string.h>

#include "audio_actions.h"
#include "gain_core.h"

void obs_gain_actions_init(ObsGainActions *a)
{
	a->array = NULL;
	a->num   = 0;
	a->cap   = 0;
}

void obs_gain_actions_free(ObsGainActions *a)
{
	free(a->array);
	obs_gain_actions_init(a);
}

int obs_gain_actions_push(ObsGainActions *a, uint64_t frame, float mul)
{
	if (a->num == a->cap) {
		size_t      cap  = a->cap ? a->cap * 2 : 16;
		ObsGainAction *na = realloc(a->array, cap * sizeof(*na));
		if (!na)
			return -1;
		a->array = na;
		a->cap   = cap;
	}
	a->array[a->num].frame = frame;
	a->array[a->num].mul   = mul;
	a->num++;
	return 0;
}

float obs_gain_actions_expand(ObsGainActions *a, uint64_t block_start, size_t frames, float cur_mul,
			      float *vol_data)
{
	size_t frame_num = 0;
	size_t i         = 0;

	/* OBS seeds the loop from get_source_volume() - the fold happens
	 * inside, on the raw stored factor. */
	cur_mul = obs_gain_fold_volume(cur_mul);

	/* apply_audio_actions (libobs/obs-source.c), frame-domain port.
	 * cur_mul mirrors source->volume (raw storage); every read goes
	 * through get_source_volume() -> obs_gain_fold_volume(). */
	while (i < a->num) {
		ObsGainAction action = a->array[i];
		uint64_t      frame  = action.frame;
		size_t        new_frame_num;

		if (frame < block_start)
			frame = block_start; /* OBS: timestamp < audio_ts -> audio_ts */

		new_frame_num = (size_t)(frame - block_start);

		if (new_frame_num >= frames)
			break; /* OBS: break, action stays queued for the next block */

		/* OBS: da_erase(source->audio_actions, i--) - remove without
		 * advancing, the loop's i++ keeps index order. */
		a->num--;
		memmove(&a->array[i], &a->array[i + 1], (a->num - i) * sizeof(*a->array));

		/* OBS applies the action (volume = action.vol) BEFORE filling,
		 * but the fill uses the pre-apply value; the new value only
		 * becomes current after the fill. */
		if (new_frame_num > frame_num) {
			for (; frame_num < new_frame_num; frame_num++)
				vol_data[frame_num] = cur_mul;
		}

		cur_mul = action.mul;             /* raw storage, like source->volume */
		cur_mul = obs_gain_fold_volume(cur_mul); /* get_source_volume read */
	}

	for (; frame_num < frames; frame_num++)
		vol_data[frame_num] = cur_mul;

	/* OBS returns void and keeps source->volume as the state; the last
	 * applied action IS that state (already consumed from the queue), so
	 * hand it back for the caller to store. */
	return cur_mul;
}
