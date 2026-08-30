/*
 * ffplay gain experiment: backend dispatcher.
 *
 * All backends are linked into one binary; FFPLAY_GAIN_BACKEND (read at
 * stream-open time, default "vlc") selects which one the injected hooks
 * talk to:
 *
 *   FFPLAY_GAIN_BACKEND=vlc  gain/ffplay_gain.c      (VLC replay gain port)
 *   FFPLAY_GAIN_BACKEND=mpv  gain/ffplay_gain_mpv.c  (mpv gain port)
 *   FFPLAY_GAIN_BACKEND=obs  gain/ffplay_gain_obs.c  (OBS float pipeline port)
 *
 * Each backend keeps its own adapter symbols (ffplay_gain_*,
 * ffplay_gain_mpv_*, ffplay_gain_obs_*); the dispatcher is what the patched
 * ffplay.c references, so hook injection is identical for every build.
 */

#ifndef FFPLAY_GAIN_DISPATCH_H
#define FFPLAY_GAIN_DISPATCH_H

#include <SDL.h>
#include <libavformat/avformat.h>

SDL_AudioCallback ffplay_gain_dispatch_wrap(SDL_AudioCallback orig);
void              ffplay_gain_dispatch_on_stream(AVFormatContext *ic, AVStream *st);
void              ffplay_gain_dispatch_note_device(int freq, int channels);

#endif /* FFPLAY_GAIN_DISPATCH_H */
