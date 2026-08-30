/*
 * mpv audio gain: per-sample scaling kernels.
 *
 * Ported from mpv 0.41.0 (GPLv2+) audio/out/ao.c.  The macros are copied
 * verbatim (MUL_GAIN_i relies on arithmetic right shift of negative values,
 * as upstream); mpv's MPCLAMP is replicated below.
 */

#include <limits.h>
#include <math.h>

#include "mpv_kernels.h"

/* common/common.h */
#define MPCLAMP(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))

/* audio/out/ao.c: MUL_GAIN_i, MUL_GAIN_f (verbatim). */
#define MUL_GAIN_i(d, num_samples, gain, low, center, high)                     \
    for (int n = 0; n < (num_samples); n++)                                     \
        (d)[n] = MPCLAMP(                                                       \
            ((((int64_t)((d)[n]) - (center)) * (gain) + 128) >> 8) + (center),  \
            (low), (high))

#define MUL_GAIN_f(d, num_samples, gain)                                        \
    for (int n = 0; n < (num_samples); n++)                                     \
        (d)[n] = (d)[n] * (gain)

void mpv_process_plane(int fmt, void *data, int num_samples, float gain)
{
    /* audio/out/ao.c process_plane(): the float gain is quantized to a Q8.8
     * integer for the fixed point formats; unity means zero work. */
    int gi = (int)lrint(256.0 * gain);
    if (gi == 256)
        return;

    switch (fmt) {
    case MPV_AFMT_U8:
        MUL_GAIN_i((uint8_t *)data, num_samples, gi, 0, 128, 255);
        break;
    case MPV_AFMT_S16:
        MUL_GAIN_i((int16_t *)data, num_samples, gi, INT16_MIN, 0, INT16_MAX);
        break;
    case MPV_AFMT_S32:
        MUL_GAIN_i((int32_t *)data, num_samples, gi, INT32_MIN, 0, INT32_MAX);
        break;
    case MPV_AFMT_FLOAT:
        MUL_GAIN_f((float *)data, num_samples, gain);
        break;
    case MPV_AFMT_DOUBLE:
        MUL_GAIN_f((double *)data, num_samples, gain);
        break;
    default:;
        /* all other sample formats are simply not supported */
    }
}

void mpv_post_process_data(int fmt, int planar, int channels, void **data,
                           int num_samples, float gain)
{
    int planes = planar ? channels : 1;
    int plane_samples = num_samples * (planar ? 1 : channels);
    int n;

    for (n = 0; n < planes; n++)
        mpv_process_plane(fmt, data[n], plane_samples, gain);
}
