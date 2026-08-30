/*
 * ffplay gain experiment, mpv backend: integration hooks for a patched copy
 * of ffplay.c.  Same injection contract as the VLC backend (ffplay_gain.h):
 *
 * fftools/ffplay.c itself stays byte-identical to the upstream snapshot
 * (UPSTREAM.toml policy); cmake/gain_inject.cmake rewrites a build-tree copy
 * to include this header and to route the SDL audio callback and
 * stream_component_open() through the functions below.
 *
 * The gain factor is mpv's audio_get_gain() (player/audio.c) applied with
 * mpv's process_plane() kernel (audio/out/ao.c) — see gain/mpv/.  This file
 * exists only on the experiment/volume-gain branch.
 */

#ifndef FFPLAY_GAIN_MPV_H
#define FFPLAY_GAIN_MPV_H

#include <SDL.h>
#include <libavformat/avformat.h>

/*
 * Called by audio_open() when registering the SDL audio callback. Returns a
 * trampoline that runs the original callback first and then applies the mpv
 * total gain factor to the S16 output stream (mirroring mpv: the gain is the
 * last processing stage before the device, audio/out/buffer.c
 * ao_read_data -> ao_post_process_data).
 */
SDL_AudioCallback ffplay_gain_mpv_wrap_callback(SDL_AudioCallback orig);

/*
 * Called by stream_component_open() for every successfully opened audio
 * stream.  Harvests replaygain data the way mpv's demuxers do (stream
 * AV_PKT_DATA_REPLAYGAIN side data first, then stream tags, then file tags,
 * first wins — demux/demux.c demux_update_replaygain + demux/demux_lavf.c
 * export_replaygain) and recomputes the total gain factor.
 */
void ffplay_gain_mpv_on_stream(AVFormatContext *ic, AVStream *st);

#endif /* FFPLAY_GAIN_MPV_H */
