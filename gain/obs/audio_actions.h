/*
 * Ported from OBS Studio libobs/obs-source.c audio action queue
 * (apply_audio_actions / apply_audio_volume, GPL-2.0+).
 *
 * OBS never applies obs_source_set_volume() immediately: it queues a
 * timestamped AUDIO_ACTION_VOL entry and the audio thread applies it when
 * the block being rendered covers that timestamp, filling a per-frame
 * volume envelope (vol_data[]) with the step landing on the exact frame.
 * The step is a sample-aligned step change, NOT an interpolated ramp -
 * OBS interpolates nothing between the old and new volume.
 *
 * Domain change: OBS timestamps are ns and clamped against the block's
 * audio_ts; this port works in absolute device-frame indices (the ffplay
 * SDL callback carries no timestamps), so "clamp to block start" becomes
 * frame < block_start -> frame = block_start.  The mute / push-to-talk
 * state machine inside get_source_volume() is not ported; the envelope
 * loop itself is copied step for step.
 */

#ifndef OBS_AUDIO_ACTIONS_H
#define OBS_AUDIO_ACTIONS_H

#include <stddef.h>
#include <stdint.h>

/* libobs/media-io/audio-io.h AUDIO_OUTPUT_FRAMES: OBS renders audio in
 * fixed 1024-frame blocks and sizes its envelope array accordingly. */
#define OBS_GAIN_BLOCK_FRAMES 1024

typedef struct ObsGainAction {
	uint64_t frame; /* absolute device-frame index of the step        */
	float    mul;   /* raw (unfolded) linear factor, like action.vol  */
} ObsGainAction;

typedef struct ObsGainActions {
	ObsGainAction *array;
	size_t         num;
	size_t         cap;
} ObsGainActions;

void  obs_gain_actions_init(ObsGainActions *a);
void  obs_gain_actions_free(ObsGainActions *a);

/* Appends at the tail; the caller posts in chronological order (OBS's queue
 * is posting order too, and the expand loop consumes it index-ordered). */
int   obs_gain_actions_push(ObsGainActions *a, uint64_t frame, float mul);

/*
 * The apply_audio_actions() port.  Fills vol_data[0..frames) with the
 * per-frame volume (steps land on their exact frame, late events clamp to
 * frame 0 of the block) and consumes every action that falls inside the
 * block.  Events at or beyond the block end stay queued (OBS: the loop
 * breaks and the da_erase'd queue keeps them for the next block).
 *
 * cur_mul is the RAW stored factor (OBS: source->volume); the fold happens
 * inside, exactly where OBS calls get_source_volume() (loop seed and after
 * each applied action).  The return value is the folded factor of the last
 * state, which the caller keeps as its storage state.
 */
float obs_gain_actions_expand(ObsGainActions *a, uint64_t block_start, size_t frames, float cur_mul,
			      float *vol_data);

#endif /* OBS_AUDIO_ACTIONS_H */
