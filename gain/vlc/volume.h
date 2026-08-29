/*
 * Gain volume state: combines the replay gain factor with a sample format
 * specific amplifier, mirroring VLC src/audio_output/volume.c
 * (aout_volume_t / aout_volume_Amplify), LGPL-2.1+.
 *
 * VLC's "audio volume" plugin dispatch (module_need by sample format) is
 * replaced by a plain AmplifyFunc pointer; the lock-free factor update
 * semantics (VLC _Atomic float + memory_order_relaxed) are preserved so the
 * factor can change while the audio callback is running.
 */

#ifndef GAIN_VLC_VOLUME_H
#define GAIN_VLC_VOLUME_H

#include "amplify.h"

/*
 * Atomic float: Interlocked fallback for MSVC, plain assignment to an
 * _Atomic-qualified float for GCC and Clang (assignments and reads of
 * _Atomic objects are atomic with sequential consistency, in any language
 * mode; <stdatomic.h> cannot be used because C11's generic functions do not
 * cover floating types and modern GCC no longer provides atomic_float).
 */
#if defined(_MSC_VER) && !defined(__clang__)
typedef struct GainAtomicFloat {
    volatile long value;
} GainAtomicFloat;

void gain_atomic_store(GainAtomicFloat *a, float v);
float gain_atomic_load(GainAtomicFloat *a);
#else
typedef struct GainAtomicFloat {
    _Atomic float value;
} GainAtomicFloat;

static inline void gain_atomic_store(GainAtomicFloat *a, float v)
{
    a->value = v;
}

static inline float gain_atomic_load(GainAtomicFloat *a)
{
    return a->value;
}
#endif

typedef struct GainVolume {
    GainAtomicFloat factor; /* linear gain multiplier                       */
    AmplifyFunc    amplify; /* kernel for the output format, NULL = no-op   */
} GainVolume;

void  gain_volume_init(GainVolume *vol);
void  gain_volume_set_factor(GainVolume *vol, float factor);
float gain_volume_get_factor(GainVolume *vol);
void  gain_volume_set_amplifier(GainVolume *vol, AmplifyFunc func);

/* Applies the current factor to len bytes of samples (VLC aout_volume_Amplify). */
void  gain_volume_apply(GainVolume *vol, uint8_t *buf, int len);

#endif /* GAIN_VLC_VOLUME_H */
