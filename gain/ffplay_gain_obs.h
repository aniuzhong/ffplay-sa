/*
 * ffplay gain experiment, OBS backend: integration hooks for a patched copy
 * of ffplay.c (injected by cmake/gain_inject.cmake with GAIN_HEADER =
 * ffplay_gain_obs.h).
 *
 * Ports OBS Studio's gain pipeline (GPL-2.0+) onto the S16 device stream:
 * obs/ audio_math + gain_core + audio_actions are the verbatim OBS kernels;
 * this adapter supplies the float pipeline OBS assumes (S16 -> float ->
 * constant/envelope gain -> device-sink clamp -> S16) and drives OBS's
 * timestamped volume actions as FFPLAY_GAIN_EVENTS steps.
 */

#ifndef FFPLAY_GAIN_OBS_H
#define FFPLAY_GAIN_OBS_H

#include <SDL.h>
#include <libavformat/avformat.h>

/*
 * Called by audio_open() when registering the SDL audio callback. Returns a
 * trampoline that runs the original callback first and then runs the OBS
 * gain pipeline over the S16 output stream.
 */
SDL_AudioCallback ffplay_gain_obs_wrap_callback(SDL_AudioCallback orig);

/*
 * Called by stream_component_open() for every successfully opened audio
 * stream. Resets the per-stream state (frame counter, volume, action queue,
 * event schedule). OBS's gain state lives on the source and resets the same
 * way a new source would.
 */
void ffplay_gain_obs_on_stream(AVFormatContext *ic, AVStream *st);

/*
 * Called from audio_open() right after audio_hw_params->freq = spec.freq.
 * SDL opens the device with SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
 * SDL_AUDIO_ALLOW_CHANNELS_CHANGE, so the *actual* device rate/channels
 * (spec, not wanted_spec) define the frame domain the volume events and the
 * block state machine run in.
 */
void ffplay_gain_obs_note_device(int freq, int channels);

#endif /* FFPLAY_GAIN_OBS_H */
