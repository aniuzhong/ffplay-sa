/*
 * ffplay gain experiment: integration hooks for a patched copy of ffplay.c.
 *
 * fftools/ffplay.c itself stays byte-identical to the upstream snapshot
 * (UPSTREAM.toml policy); cmake/gain_inject.cmake rewrites a build-tree copy
 * to include this header and to route the SDL audio callback and
 * stream_component_open() through the functions below.
 *
 * This file exists only on the experiment/volume-gain branch.
 */

#ifndef FFPLAY_GAIN_H
#define FFPLAY_GAIN_H

#include <SDL.h>
#include <libavformat/avformat.h>

/*
 * Called by audio_open() when registering the SDL audio callback. Returns a
 * trampoline that runs the original callback first and then applies the gain
 * factor to the S16 output stream.
 */
SDL_AudioCallback ffplay_gain_wrap_callback(SDL_AudioCallback orig);

/*
 * Called by stream_component_open() for every successfully opened audio
 * stream. Harvests REPLAYGAIN_* tags from the stream and file metadata and
 * recomputes the gain factor (VLC: es_out.c + audio-replay-gain-mode change).
 */
void ffplay_gain_on_stream(AVFormatContext *ic, AVStream *st);

#endif /* FFPLAY_GAIN_H */
