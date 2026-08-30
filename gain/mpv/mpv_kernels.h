/*
 * mpv audio gain: per-sample scaling kernels.
 *
 * Ported from mpv 0.41.0 (GPLv2+) audio/out/ao.c: MUL_GAIN_i, MUL_GAIN_f,
 * process_plane() and ao_post_process_data().  The Q8.8 fixed-point integer
 * path (int64 intermediate, +128 rounding, MPCLAMP) and the unconditional
 * float path are kept verbatim; the struct ao plumbing is replaced by
 * explicit format/planar arguments.
 *
 * ffplay forces the SDL device to AUDIO_S16SYS interleaved, so in-situ only
 * the S16 interleaved branch runs; the rest is unit tested standalone
 * (gain/tests/test_mpv_gain.c).
 */

#ifndef GAIN_MPV_KERNELS_H
#define GAIN_MPV_KERNELS_H

#include <stdint.h>

/* audio/out/ao.c: af_fmt_from_planar(ao->format) values handled below. */
enum MpvAFormat {
    MPV_AFMT_U8,
    MPV_AFMT_S16,
    MPV_AFMT_S32,
    MPV_AFMT_FLOAT,
    MPV_AFMT_DOUBLE,
};

/*
 * audio/out/ao.c process_plane(): multiplies num_samples scalar samples in
 * place.  Skips entirely when the Q8.8 quantized gain is unity
 * (lrint(256 * gain) == 256), like upstream.  Other sample formats are
 * simply not supported, as upstream.
 */
void mpv_process_plane(int fmt, void *data, int num_samples, float gain);

/*
 * audio/out/ao.c ao_post_process_data(): dispatches the kernel over
 * planar/interleaved layouts.  num_samples counts frames; for interleaved
 * data the kernel then sees frames * channels scalar samples, matching
 * upstream plane_samples arithmetic.
 */
void mpv_post_process_data(int fmt, int planar, int channels, void **data,
                           int num_samples, float gain);

#endif /* GAIN_MPV_KERNELS_H */
