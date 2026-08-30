#include <stdlib.h>
#include <string.h>

#include <libavutil/log.h>

#include "ffplay_gain_dispatch.h"
#include "ffplay_gain.h"       /* vlc backend adapter      */
#include "ffplay_gain_mpv.h"   /* mpv backend adapter      */
#include "ffplay_gain_obs.h"   /* obs backend adapter      */

typedef enum { GAIN_BACKEND_VLC, GAIN_BACKEND_MPV, GAIN_BACKEND_OBS } GainBackend;

static GainBackend gain_backend = GAIN_BACKEND_VLC;
static int         gain_backend_resolved;

static void gain_resolve_backend(void)
{
	const char *s = getenv("FFPLAY_GAIN_BACKEND");

	gain_backend_resolved = 1;
	if (!s || !*s)
		return;
	if (!strcmp(s, "vlc"))
		gain_backend = GAIN_BACKEND_VLC;
	else if (!strcmp(s, "mpv"))
		gain_backend = GAIN_BACKEND_MPV;
	else if (!strcmp(s, "obs"))
		gain_backend = GAIN_BACKEND_OBS;
	else {
		av_log(NULL, AV_LOG_WARNING,
		       "[gain] unknown FFPLAY_GAIN_BACKEND '%s', using 'vlc'\n", s);
		gain_backend = GAIN_BACKEND_VLC;
	}
}

SDL_AudioCallback ffplay_gain_dispatch_wrap(SDL_AudioCallback orig)
{
	if (!gain_backend_resolved)
		gain_resolve_backend();

	switch (gain_backend) {
	case GAIN_BACKEND_MPV:
		return ffplay_gain_mpv_wrap_callback(orig);
	case GAIN_BACKEND_OBS:
		return ffplay_gain_obs_wrap_callback(orig);
	case GAIN_BACKEND_VLC:
	default:
		return ffplay_gain_wrap_callback(orig);
	}
}

void ffplay_gain_dispatch_on_stream(AVFormatContext *ic, AVStream *st)
{
	if (!gain_backend_resolved)
		gain_resolve_backend();

	switch (gain_backend) {
	case GAIN_BACKEND_MPV:
		ffplay_gain_mpv_on_stream(ic, st);
		break;
	case GAIN_BACKEND_OBS:
		ffplay_gain_obs_on_stream(ic, st);
		break;
	case GAIN_BACKEND_VLC:
	default:
		ffplay_gain_on_stream(ic, st);
		break;
	}
}

void ffplay_gain_dispatch_note_device(int freq, int channels)
{
	/* Only the OBS backend needs the negotiated device parameters (its
	 * frame-domain state machine and event clock run on spec.freq); the
	 * vlc/mpv backends apply a constant factor per sample and are
	 * channel/frame agnostic. */
	ffplay_gain_obs_note_device(freq, channels);
}
