/*
 * libobs reference dumper for the ffplay gain experiment (OBS backend).
 *
 * Runs the REAL libobs audio pipeline end to end: an official media source
 * ("ffmpeg_source" from obs-ffmpeg) plays a WAV file, obs_source_set_volume()
 * applies the gain (dB -> linear via libobs' own obs_db_to_mul), and
 * obs_add_raw_audio_callback() captures the post-mix float at the device
 * sink - the same semantic point the port re-hosts with clamp_audio_output.
 * No encoder, no monitor, one source, one mix: the capture is
 * volume-then-clamp, so a reference comparison isolates exactly the
 * pipeline the port implements.
 *
 * Volume changes via the events argument are REAL OBS actions:
 * obs_source_set_volume() queues a timestamped AUDIO_ACTION_VOL and
 * libobs' audio thread applies it sample-aligned in its 1024-frame blocks
 * - the mechanism gain/obs/audio_actions.c is a verbatim port of.
 *
 * Build (Linux, apt libobs-dev + obs-studio): CMake target `obs_ref_dump`.
 * Usage: obs_ref_dump [db|-6] [out.raw] [media.wav] [events "sec:db,.."]
 * Output: raw float32 interleaved stereo at the mix rate (post-clamp mix).
 *
 * Hosted on a headless WSLg box without touching the video pipeline: the
 * audio thread runs independently, so obs_reset_video is deliberately not
 * called.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <obs.h>
#include <util/platform.h>

#define RUN_SECONDS 10
#define MAX_EVENTS  64

typedef struct {
	double sec;
	float  db;
	int    done;
} RefEvent;

static RefEvent  events[MAX_EVENTS];
static int       events_num;
static FILE     *dump_file;
static uint64_t  dump_frames;
static uint64_t  debug_nonzero;
static uint64_t  run_start;

static void parse_events(const char *s)
{
	while (s && *s && events_num < MAX_EVENTS) {
		char *end;
		double sec = strtod(s, &end);
		double db;

		if (end == s || *end != ':')
			break;
		s = end + 1;
		db = strtod(s, &end);
		if (end == s)
			break;
		events[events_num].sec = sec;
		events[events_num].db  = (float)db;
		events_num++;
		s = end;
		if (*s == ',')
			s++;
	}
}

static void capture(void *param, size_t mix_idx, struct audio_data *data)
{
	float out[2 * 1024];
	uint32_t i;

	(void)param;
	(void)mix_idx;

	if (!dump_file || data->frames == 0)
		return;

	for (i = 0; i < data->frames; i++) {
		out[2 * i]     = ((float *)data->data[0])[i];
		out[2 * i + 1] = ((float *)data->data[1])[i];
		if (out[2 * i] != 0.0f || out[2 * i + 1] != 0.0f)
			debug_nonzero++;
	}
	fwrite(out, sizeof(float), (size_t)data->frames * 2, dump_file);
	dump_frames += data->frames;
}

/* Pass-through log handler so pipeline warnings reach stderr. */
static void log_forward(int lvl, const char *format, va_list args, void *p)
{
	if (lvl < LOG_INFO)
		return;
	(void)lvl;
	(void)p;
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
}

int main(int argc, char **argv)
{
	double db       = argc > 1 ? atof(argv[1]) : -6.0;
	const char *out = argc > 2 ? argv[2] : "obs_ref.raw";
	const char *media = argc > 3 ? argv[3] : NULL;
	const char *events_spec = argc > 4 ? argv[4] : NULL;
	obs_data_t *settings;
	obs_source_t *src;
	int e;

	if (events_spec)
		parse_events(events_spec);

	if (!obs_startup("en-US", NULL, NULL)) {
		fprintf(stderr, "obs_startup failed\n");
		return 1;
	}
	base_set_log_handler(log_forward, NULL);

	/* Official media-source plugin only: loading everything drags in
	 * Qt-dependent modules (frontend-tools) that abort on this headless
	 * host. obs-ffmpeg provides "ffmpeg_source". */
	{
		obs_module_t *mod = NULL;
		int rc = obs_open_module(&mod,
					 "/usr/lib/x86_64-linux-gnu/obs-plugins/obs-ffmpeg.so", NULL);
		if (rc != MODULE_SUCCESS || !mod || !obs_init_module(mod)) {
			fprintf(stderr, "loading obs-ffmpeg module failed (rc=%d)\n", rc);
			return 1;
		}
	}

	/* Video IS required: audio_tick() walks obs->video.mixes to find the
	 * canvas sources to mix, so without a video reset root_nodes stays
	 * empty and nothing is ever mixed.  WSLg provides EGL/GL, so the
	 * stock libobs-opengl module initializes. */
	if (obs_reset_video(&(struct obs_video_info){
				    .adapter = 0,
				    .graphics_module = "libobs-opengl",
				    .fps_num = 30,
				    .fps_den = 1,
				    .base_width = 640,
				    .base_height = 360,
				    .output_width = 640,
				    .output_height = 360,
				    .output_format = VIDEO_FORMAT_NV12,
				    .colorspace = VIDEO_CS_709,
				    .range = VIDEO_RANGE_PARTIAL,
				    .scale_type = OBS_SCALE_BICUBIC,
				    .gpu_conversion = true,
			    }) != OBS_VIDEO_SUCCESS) {
		fprintf(stderr, "obs_reset_video failed (video mixes drive the mixer)\n");
		return 1;
	}

	/* Audio thread only; no video pipeline on this headless host. */
	if (!obs_reset_audio(&(struct obs_audio_info){ .samples_per_sec = 48000,
						       .speakers = SPEAKERS_STEREO })) {
		fprintf(stderr, "obs_reset_audio failed\n");
		return 1;
	}

	settings = obs_data_create();
	obs_data_set_string(settings, "local_file", media);
	obs_data_set_bool(settings, "is_local_file", true);
	obs_data_set_bool(settings, "restart_on_activate", false);
	obs_data_set_bool(settings, "close_when_inactive", false);
	src = obs_source_create("ffmpeg_source", "ffplay-gain-ref", settings, NULL);
	obs_data_release(settings);
	if (!src) {
		fprintf(stderr, "obs_source_create(ffmpeg_source) failed\n");
		return 1;
	}

	/* The mixer only walks sources attached to a canvas channel
	 * (root_nodes in obs-audio.c) - this is "add source to the scene". */
	obs_set_output_source(0, src);

	/* Static volume: obs_source_set_volume queues a timestamped
	 * AUDIO_ACTION_VOL, applied by the audio thread like every OBS
	 * volume change. */
	obs_source_set_volume(src, obs_db_to_mul((float)db));

	dump_file = fopen(out, "wb");
	if (!dump_file) {
		fprintf(stderr, "cannot open %s\n", out);
		return 1;
	}
	obs_add_raw_audio_callback(0, NULL, capture, NULL);

	fprintf(stderr, "[obs-ref] volume %.2f dB (factor %.6f), events=%d, media=%s\n", db,
		(double)obs_db_to_mul((float)db), events_num, media ? media : "(none)");

	/* Media source starts playing once active; events fire at wall-clock
	 * time like a user moving the volume slider. */
	run_start = os_gettime_ns();
	for (e = 0; e < events_num; e++) {
		os_sleepto_ns(run_start + (uint64_t)(events[e].sec * 1e9));
		obs_source_set_volume(src, obs_db_to_mul(events[e].db));
		fprintf(stderr, "[obs-ref] set_volume %.2f dB at t=%.3fs\n", events[e].db,
			(double)(os_gettime_ns() - run_start) / 1e9);
	}

	/* Run past the media end so the window consumes the whole timeline. */
	os_sleepto_ns(run_start + (uint64_t)(RUN_SECONDS * 1e9));

	fprintf(stderr, "[obs-ref] captured %llu frames, nonzero samples %llu\n",
		(unsigned long long)dump_frames, (unsigned long long)debug_nonzero);

	obs_remove_raw_audio_callback(0, capture, NULL);
	fclose(dump_file);
	obs_set_output_source(0, NULL);
	obs_source_release(src);
	obs_shutdown();
	return 0;
}
