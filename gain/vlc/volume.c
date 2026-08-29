/*
 * Gain volume state, ported from VLC src/audio_output/volume.c
 * (aout_volume_Amplify and the replay gain factor callback), LGPL-2.1+.
 */

#include <stddef.h>

#include "volume.h"

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>

void gain_atomic_store(GainAtomicFloat *a, float v)
{
    union { long l; float f; } u;
    u.f = v;
    _InterlockedExchange(&a->value, u.l);
}

float gain_atomic_load(GainAtomicFloat *a)
{
    union { long l; float f; } u;
    u.l = a->value;
    return u.f;
}
#endif

void gain_volume_init(GainVolume *vol)
{
    gain_atomic_store(&vol->factor, 1.f);
    vol->amplify = NULL;
}

void gain_volume_set_factor(GainVolume *vol, float factor)
{
    gain_atomic_store(&vol->factor, factor);
}

float gain_volume_get_factor(GainVolume *vol)
{
    return gain_atomic_load(&vol->factor);
}

void gain_volume_set_amplifier(GainVolume *vol, AmplifyFunc func)
{
    vol->amplify = func;
}

void gain_volume_apply(GainVolume *vol, uint8_t *buf, int len)
{
    float amp;

    if (!vol->amplify)
        return;

    amp = gain_atomic_load(&vol->factor);
    vol->amplify(buf, len, amp);
}
