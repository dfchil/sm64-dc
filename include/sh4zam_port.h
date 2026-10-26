#ifndef SH4ZAM_PORT_H
#define SH4ZAM_PORT_H

// sh4zam from the KOS addons, plus the few SM64-side helpers the port built on it (they used to
// live in a vendored, outdated copy of sh4zam.h).
#if defined(__FAST_MATH__) && DC_SHZ_FSCA
// SH4ZAM 0.10 turns its sincos into two __builtin_sinf/cosf calls on float radians under
// __FAST_MATH__ (our -ffast-math), which differs from one FSCA on the 16-bit angle by one unit on
// ~17% of angles and moves the game's sins/coss. Its routines are inline: read without the macro,
// they keep the FSCA path, and nothing else is compiled differently.
#pragma push_macro("__FAST_MATH__")
#undef __FAST_MATH__
#include <sh4zam/shz_sh4zam.h>
#pragma pop_macro("__FAST_MATH__")
#else
#include <sh4zam/shz_sh4zam.h>
#endif
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
