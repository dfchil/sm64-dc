// make DC_TAS=1: the game state as one line of text at a movie sample, written the same way by
// the Dreamcast build, the pc-endjinn host build (SM64_TRACE) and tools/tasref (the N64 game in
// Mupen64Plus), so tools/tasref/compare.py can find where a run leaves the N64's. Floats are
// given as their bits, so the comparison is exact.
#ifdef DC_TAS

#include <stdio.h>
#include <string.h>

#include "sm64.h"
#include "camera.h"
#include "game_init.h"
#include "level_update.h"
#include "tas_trace.h"

extern u16 tas_random_seed16(void);

static u32 fbits(f32 f) {
    u32 u;
    memcpy(&u, &f, sizeof(u));
    return u;
}

// At the controller read, before the game frame that uses the sample.
int tas_trace_line(char *buf, size_t size, unsigned int sample) {
    struct MarioState *m = &gMarioStates[0];
    return snprintf(buf, size,
                    "%u t=%u L=%d A=%d act=%08x st=%04x tm=%04x pos=%08x,%08x,%08x vel=%08x,%08x,%08x "
                    "fv=%08x yaw=%04x rng=%04x c=%d s=%d cf=%08x,%08x,%08x cp=%08x,%08x,%08x",
                    sample, (unsigned) gGlobalTimer, gCurrLevelNum, gCurrAreaIndex, (unsigned) m->action,
                    (unsigned) m->actionState, (unsigned) m->actionTimer, (unsigned) fbits(m->pos[0]),
                    (unsigned) fbits(m->pos[1]), (unsigned) fbits(m->pos[2]), (unsigned) fbits(m->vel[0]),
                    (unsigned) fbits(m->vel[1]), (unsigned) fbits(m->vel[2]), (unsigned) fbits(m->forwardVel),
                    (unsigned) (u16) m->faceAngle[1], (unsigned) tas_random_seed16(), m->numCoins, m->numStars,
                    (unsigned) fbits(gLakituState.curFocus[0]), (unsigned) fbits(gLakituState.curFocus[1]),
                    (unsigned) fbits(gLakituState.curFocus[2]), (unsigned) fbits(gLakituState.curPos[0]),
                    (unsigned) fbits(gLakituState.curPos[1]), (unsigned) fbits(gLakituState.curPos[2]));
}

#endif
