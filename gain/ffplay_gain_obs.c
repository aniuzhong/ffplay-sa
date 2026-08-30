/*
 * ffplay gain experiment, OBS backend: integration layer.
 *
 * Faithful-port summary (vs. gain/vlc and gain/mpv, which port single
 * multiply kernels):
 *
 * - Volume is an OBS float-pipeline process, not an in-place S16 multiply:
 *   every SDL callback is converted S16 -> float, rendered through
 *   apply_audio_volume() semantics in OBS's fixed 1024-frame blocks
 *   (AUDIO_OUTPUT_FRAMES), clamped once at the device sink
 *   (clamp_audio_output: NaN -> 0, saturation to +/-1), and converted back.
 * - Volume changes are OBS timestamped actions (obs_source_set_volume ->
 *   AUDIO_ACTION_VOL): FFPLAY_GAIN_EVENTS="sec:db,..." schedules steps that
 *   land on exact device frames. The event clock is the frame counter
 *   (frames / sample_rate), which is more deterministic than OBS's wall
 *   clock; late events clamp to the block start exactly like OBS clamps
 *   timestamps that fall behind audio_ts.
 * - The unity/zero fast paths of apply_audio_volume() are kept: a constant
 *   1.0 factor skips the multiply entirely, 0.0 clears the buffer.
 *
 * Single-threaded by the same argument as the other backends: the state is
 * reset by the read thread in on_stream() before SDL_PauseAudio(0), and
 * only the SDL audio thread touches it afterwards - no atomics needed.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/log.h>

#include "ffplay_gain_obs.h"
#include "obs/audio_actions.h"
#include "obs/audio_math.h"
#include "obs/gain_core.h"

#define MAX_EVENTS 256

typedef struct {
	double sec; /* step position in seconds from stream start */
	float  mul; /* linear factor (raw, unfolded)              */
} GainEvent;

/* Configuration (OBS: source volume in dB; env replaces the properties UI). */
static int       obs_cfg_loaded;
static float     obs_start_mul = 1.0f;  /* FFPLAY_GAIN_DB -> db_to_mul            */
static int       obs_num_events;
static GainEvent obs_events[MAX_EVENTS]; /* FFPLAY_GAIN_EVENTS, sorted by sec     */
static FILE     *obs_dump;              /* FFPLAY_GAIN_DUMP: post-pipeline S16    */

/* Runtime state: written by the read thread before the audio thread starts,
 * then SDL-audio-thread only. */
static SDL_AudioCallback obs_orig_cb;
static int               obs_freq;            /* device spec.freq (note_device)       */
static int               obs_channels;        /* device spec.channels (note_device)   */
static uint64_t          obs_frames_done;     /* absolute frame index of block edge   */
static float             obs_cur_mul = 1.0f;  /* source->volume: raw storage          */
static ObsGainActions    obs_actions;         /* AUDIO_ACTION_VOL queue               */
static size_t            obs_next_event;      /* events not yet queued as actions     */
static float            *obs_conv;            /* S16<->float scratch, grown on demand */
static size_t            obs_conv_cap;

static float obs_env_float(const char *name, float def)
{
	const char *s = getenv(name);
	char *end;
	double v;

	if (!s || !*s)
		return def;
	v = strtod(s, &end);
	return end == s ? def : (float)v;
}

/* Parses "sec:db,sec:db,...", e.g. FFPLAY_GAIN_EVENTS="2.0:-6,5.0:0".
 * db accepts anything strtod does, including "-inf" (obs_db_to_mul folds
 * non-finite dB to a zero factor, like OBS). */
static void obs_parse_events(const char *s)
{
	while (s && *s && obs_num_events < MAX_EVENTS) {
		char *end;
		double sec, db;
		GainEvent *ev;

		sec = strtod(s, &end);
		if (end == s || *end != ':')
			break;
		s = end + 1;
		db = strtod(s, &end);
		if (end == s)
			break;
		ev = &obs_events[obs_num_events++];
		ev->sec = sec;
		ev->mul = obs_db_to_mul((float)db);
		s = end;
		if (*s == ',')
			s++;
	}
}

static int obs_cmp_events(const void *pa, const void *pb)
{
	const GainEvent *a = pa, *b = pb;

	if (a->sec < b->sec)
		return -1;
	return a->sec > b->sec;
}

static void obs_load_config(void)
{
	const char *events = getenv("FFPLAY_GAIN_EVENTS");
	const char *dump   = getenv("FFPLAY_GAIN_DUMP");

	/* obs_source_set_volume() stores the raw multiplier; FFPLAY_GAIN_DB
	 * is the properties-UI equivalent (gain-filter.c: db_to_mul(val)). */
	obs_start_mul = obs_db_to_mul(obs_env_float("FFPLAY_GAIN_DB", 0.0f));
	if (events && *events) {
		obs_parse_events(events);
		qsort(obs_events, obs_num_events, sizeof(obs_events[0]), obs_cmp_events);
	}
	if (dump && *dump)
		obs_dump = fopen(dump, "wb");

	obs_gain_actions_init(&obs_actions);
	obs_cfg_loaded = 1;

	av_log(NULL, AV_LOG_INFO, "[obs-gain] db=%.2f events=%d dump=%s\n",
	       obs_mul_to_db(obs_start_mul), obs_num_events, obs_dump ? "on" : "off");
}

void ffplay_gain_obs_on_stream(AVFormatContext *ic, AVStream *st)
{
	if (!obs_cfg_loaded)
		obs_load_config();

	/* A fresh OBS source starts at its configured volume with an empty
	 * action queue; events are relative to this stream's start. */
	obs_frames_done = 0;
	obs_cur_mul     = obs_start_mul;
	obs_next_event  = 0;
	obs_actions.num = 0;
}

void ffplay_gain_obs_note_device(int freq, int channels)
{
	obs_freq     = freq;
	obs_channels = channels;
}

/* One OBS render block: apply_audio_volume() over
 * [block_start, block_start + frames).  Returns nothing; the folded current
 * factor lives in obs_cur_mul (raw storage, folded on read). */
static void obs_render_block(uint64_t block_start, size_t frames, float *buf)
{
	float vol_data[OBS_GAIN_BLOCK_FRAMES];
	uint64_t block_end = block_start + frames;
	float vol;
	size_t i, total;

	/* OBS's audio thread checks the action queue at each block edge and
	 * renders every action that falls inside the block (apply_audio_volume:
	 * action.timestamp < audio_ts + duration).  Our events are pre-sorted
	 * by second, so delivering in index order matches OBS's posting order. */
	while (obs_next_event < (size_t)obs_num_events) {
		uint64_t target = (uint64_t)llround(obs_events[obs_next_event].sec * (double)obs_freq);

		if (target >= block_end)
			break;
		obs_gain_actions_push(&obs_actions, target, obs_events[obs_next_event].mul);
		obs_next_event++;
	}

	/* Fast-path check of apply_audio_volume(): only when no queued action
	 * falls inside this block is the constant-volume path taken. */
	if (obs_actions.num == 0 || obs_actions.array[0].frame >= block_end) {
		vol = obs_gain_fold_volume(obs_cur_mul);
		if (vol == 1.0f)
			return;
		total = frames * (size_t)obs_channels;
		if (vol == 0.0f) {
			memset(buf, 0, total * sizeof(float));
		} else {
			obs_gain_mul_scalar(buf, total, vol);
		}
		return;
	}

	/* Envelope path: vol_data[] gets one factor per frame, steps land on
	 * their exact frame (late events clamp to the block start).  The
	 * expand folds the raw stored factor itself (OBS: get_source_volume
	 * inside apply_audio_actions). */
	obs_cur_mul = obs_gain_actions_expand(&obs_actions, block_start, frames,
					      obs_cur_mul, vol_data);
	obs_gain_mul_envelope(buf, frames, obs_channels, vol_data);
}

static int obs_ensure_conv(size_t n)
{
	if (n <= obs_conv_cap)
		return 0;
	free(obs_conv);
	obs_conv = malloc(n * sizeof(float));
	if (!obs_conv) {
		obs_conv_cap = 0;
		return -1;
	}
	obs_conv_cap = n;
	return 0;
}

/* OBS float pipeline over one SDL callback of interleaved S16. */
static void obs_process(Uint8 *stream, int len)
{
	size_t bpf    = (size_t)obs_channels * sizeof(int16_t);
	size_t frames = (size_t)len / bpf;
	size_t total  = frames * (size_t)obs_channels;
	size_t done   = 0;
	uint64_t block_start = obs_frames_done;

	if (obs_ensure_conv(total) < 0)
		return;

	obs_gain_s16_to_float((const int16_t *)stream, obs_conv, total);

	/* OBS renders in fixed 1024-frame blocks; the callback boundary is an
	 * additional edge, so the last sub-block of a callback may be partial
	 * (OBS blocks are always full, but the block math - queue check, step
	 * placement, clamp - is per-block and independent of block length). */
	while (done < frames) {
		size_t n = frames - done;
		if (n > OBS_GAIN_BLOCK_FRAMES)
			n = OBS_GAIN_BLOCK_FRAMES;

		obs_render_block(block_start + done, n, obs_conv + done * (size_t)obs_channels);
		obs_gain_clamp(obs_conv + done * (size_t)obs_channels,
			       n * (size_t)obs_channels);
		done += n;
	}

	obs_frames_done += frames;
	obs_gain_float_to_s16(obs_conv, (int16_t *)stream, total);
}

static void SDLCALL obs_gain_tramp(void *opaque, Uint8 *stream, int len)
{
	obs_orig_cb(opaque, stream, len);

	if (obs_freq > 0 && obs_channels > 0 && len > 0 &&
	    (size_t)len % ((size_t)obs_channels * sizeof(int16_t)) == 0)
		obs_process(stream, len);

	if (obs_dump && len > 0 && fwrite(stream, 1, len, obs_dump) != (size_t)len) {
		av_log(NULL, AV_LOG_ERROR, "[obs-gain] dump write failed\n");
		fclose(obs_dump);
		obs_dump = NULL;
	}
}

SDL_AudioCallback ffplay_gain_obs_wrap_callback(SDL_AudioCallback orig)
{
	if (!obs_cfg_loaded)
		obs_load_config();
	obs_orig_cb = orig;
	return obs_gain_tramp;
}
