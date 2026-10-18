#ifndef SH4ZAM_PORT_H
#define SH4ZAM_PORT_H

// sh4zam from the KOS addons, plus the few SM64-side helpers the port built on it (they used to
// live in a vendored, outdated copy of sh4zam.h).
#include <sh4zam/shz_sh4zam.h>
#include <PR/ultratypes.h>

// One SM64 binary angle unit (65536 per turn) in radians.
#define TRIG_ARG_SCALE 0.00009587f

// Sine and cosine of an SM64 s16 angle: its units are exactly the SH4 fsca instruction's.
static inline void sincoss(s16 angle, f32 *s, f32 *c) {
    shz_sincos_t sc = shz_sincosu16((uint16_t) angle);
    *s = sc.sin;
    *c = sc.cos;
}

static inline void scaled_sincoss(s16 angle, f32 *s, f32 *c, f32 scale) {
    shz_sincos_t sc = shz_sincosu16((uint16_t) angle);
    *s = sc.sin * scale;
    *c = sc.cos * scale;
}

#endif
