/**
 * The prebuilt dev packages omit libavutil/libm.h. Upstream's version keys
 * off configure-time HAVE_* math macros that a synthesized config.h cannot
 * reproduce and collides with UCRT/glibc declarations, so this stub just
 * exposes the standard C math API, which both platforms provide.
 */

#ifndef AVUTIL_LIBM_H
#define AVUTIL_LIBM_H

#include <math.h>

#endif /* AVUTIL_LIBM_H */
