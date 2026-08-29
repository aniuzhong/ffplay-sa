/*
 * Amplification kernels, ported from VLC modules/audio_mixer/integer.c and
 * float.c (LGPL-2.1+). Integer formats quantize the multiplier to a fixed
 * point value first, multiply through a wider intermediate and saturate, so
 * an overdriven gain clips instead of wrapping around.
 */

#include <stdint.h>
#include <limits.h>
#include <math.h>

#include "amplify.h"

void amplify_s16(uint8_t *buf, int len, float amp)
{
    int16_t *p = (int16_t *)buf;
    int mult = lroundf(amp * (1 << 8)); /* Q8 fixed point multiplier */
    int i, n = len / sizeof(*p);

    if (mult == (1 << 8))
        return; /* nothing to do */

    for (i = 0; i < n; i++) {
        int s = (p[i] * mult) >> 8;
        if (s > INT16_MAX)
            s = INT16_MAX;
        else if (s < INT16_MIN)
            s = INT16_MIN;
        p[i] = s;
    }
}

void amplify_s32(uint8_t *buf, int len, float amp)
{
    int32_t *p = (int32_t *)buf;
    int mult = lroundf(amp * (1 << 24)); /* Q24 fixed point multiplier */
    int i, n = len / sizeof(*p);

    if (mult == (1 << 24))
        return; /* nothing to do */

    for (i = 0; i < n; i++) {
        int64_t s = ((int64_t)p[i] * mult) >> 24;
        if (s > INT32_MAX)
            s = INT32_MAX;
        else if (s < INT32_MIN)
            s = INT32_MIN;
        p[i] = s;
    }
}

void amplify_u8(uint8_t *buf, int len, float amp)
{
    uint8_t *p = buf;
    int mult = lroundf(amp * (1 << 8)); /* Q8 fixed point multiplier */
    int i, n = len;

    if (mult == (1 << 8))
        return; /* nothing to do */

    for (i = 0; i < n; i++) {
        int s = ((int)(int8_t)(p[i] - 128) * mult) >> 8; /* unsigned offset binary */
        if (s > INT8_MAX)
            s = INT8_MAX;
        else if (s < INT8_MIN)
            s = INT8_MIN;
        p[i] = s + 128;
    }
}

void amplify_fl32(uint8_t *buf, int len, float amp)
{
    float *p = (float *)buf;
    int i, n = len / sizeof(*p);

    if (amp == 1.f)
        return; /* nothing to do */

    for (i = 0; i < n; i++)
        p[i] *= amp;
}
