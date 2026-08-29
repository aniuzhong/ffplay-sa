/**
 * The gyan dev package omits libavutil/libm.h. Upstream's version keys off
 * configure-time HAVE_* math macros that a synthesized config.h cannot
 * reproduce and collides with UCRT declarations, so this stub just exposes
 * the standard C math API; MSVC/UCRT provides all of it.
 */

#ifndef AVUTIL_LIBM_H
#define AVUTIL_LIBM_H

#include <math.h>

#endif /* AVUTIL_LIBM_H */
