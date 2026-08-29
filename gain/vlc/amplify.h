/*
 * Software amplification kernels for one sample format each.
 *
 * Ported from VLC (LGPL-2.1+):
 *   - modules/audio_mixer/integer.c  (FilterS16N, FilterS32N, FilterU8)
 *   - modules/audio_mixer/float.c    (FilterFL32; FilterFL64 omitted, ffplay
 *                                     never outputs double samples)
 *
 * In VLC these kernels are "audio volume" plugin modules dispatched by sample
 * format; here a plain function pointer takes their place (see volume.h).
 */

#ifndef GAIN_VLC_AMPLIFY_H
#define GAIN_VLC_AMPLIFY_H

#include <stdint.h>

/* Amplifier entry point: scales len bytes of interleaved samples in place. */
typedef void (*AmplifyFunc)(uint8_t *buf, int len, float amp);

void amplify_s16(uint8_t *buf, int len, float amp);
void amplify_s32(uint8_t *buf, int len, float amp);
void amplify_u8(uint8_t *buf, int len, float amp);
void amplify_fl32(uint8_t *buf, int len, float amp);

#endif /* GAIN_VLC_AMPLIFY_H */
