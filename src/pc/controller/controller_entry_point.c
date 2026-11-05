#include "macros.h"

#include "lib/src/libultra_internal.h"
#include "lib/src/osContInternal.h"

#include "controller_dc.h"

#ifdef DC_TAS
#include <stdio.h>
#include "game/tas_trace.h"
#include "game/tas_slice.h"
static void tas_load(void);
static unsigned char *sTas;
static unsigned int sTasSamples, sTasNext;
static int sTasOn;   // the movie plays the game, not the controller
#endif

static struct ControllerAPI *controller_implementations[] = {
    &controller_dc,
};

s32 osContInit(UNUSED OSMesgQueue *mq, u8 *controllerBits, UNUSED OSContStatus *status) {
    size_t i;

    for (i = 0; i < sizeof(controller_implementations) / sizeof(struct ControllerAPI *); i++) {
        controller_implementations[i]->init();
    }
#ifdef DC_TAS
    tas_load();
#endif
    *controllerBits = 1;
    return 0;
}

s32 osContStartReadData(UNUSED OSMesgQueue *mesg) {
#ifdef DC_TAS
    // The TAS trace (src/game/tas_trace.c) where the N64 reads the controller, as tools/tasref
    // traces it: the host writes every sample (SM64_TRACE), the Dreamcast every
    // DC_TAS_TRACE_EVERY-th to the console.
    if (sTas != NULL && sTasOn && sTasNext < sTasSamples) {
#ifdef TARGET_PC_ENDJINN
        extern void pc_tas_trace(unsigned int sample);
        pc_tas_trace(sTasNext);
#elif DC_TAS_TRACE_EVERY
        if (sTasNext % DC_TAS_TRACE_EVERY == 0) {
            char line[256];
            tas_trace_line(line, sizeof(line), sTasNext);
            printf("TASREF %s\n", line);
        }
#endif
    }
#endif
    return 0;
}

#ifdef DC_TAS
// make DC_TAS=1: play a Mupen64 movie (/pc/afx/tas.m64, version 3, from power-on) instead of the
// controller. SM64 reads the controller once per game frame and a movie holds one sample per read
// (lag frames have none), so sample n is game frame n. A sample is the buttons in OSContPad's
// order (high byte A B Z START U D L R; low byte .. L R C-U C-D C-L C-R), then stick x and y.
#include <stdio.h>
#include <stdlib.h>
#ifndef DC_TAS_ROOT
#define DC_TAS_ROOT "/pc/afx"
#endif

static void tas_load(void) {
    const char *path = DC_TAS_ROOT "/tas.m64";
#ifdef TARGET_PC_ENDJINN
    // pc-endjinn host build: the movie is any file, named by SM64_TAS (no /pc mount on the host).
    path = getenv("SM64_TAS");
    if (path == NULL) return;
#endif
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        printf("TAS: no %s; the controller plays\n", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    sTas = size > 0x400 ? malloc((size_t) size) : NULL;
    if (sTas == NULL || fread(sTas, 1, (size_t) size, f) != (size_t) size || sTas[0] != 'M' || sTas[3] != 0x1A) {
        printf("TAS: cannot read the movie\n");
        free(sTas);
        sTas = NULL;
    } else {
        sTasSamples = (unsigned int) ((size - 0x400) / 4);
        printf("TAS: %u samples, start type %u\n", sTasSamples, sTas[0x1C] | sTas[0x1D] << 8);
#ifndef DC_TAS_MENU
        sTasOn = 1;   // from power-on; with DC_TAS_MENU, from the replay the debug menu starts
#endif
    }
    fclose(f);
    if (sTas == NULL) {
        return;
    }
    // Slices (src/game/tas_slice.c). The host: SM64_SLICE_CAPTURE=file writes them while the whole
    // movie plays; SM64_SLICES=file SM64_SLICE_PLAY=3,10,... plays some. The Dreamcast plays
    // /pc/afx/tas_slices.bin in the order /pc/afx/tas_play.txt gives, if both are there.
#ifdef TARGET_PC_ENDJINN
    tas_slice_setup(getenv("SM64_SLICE_CAPTURE"), getenv("SM64_SLICES"), getenv("SM64_SLICE_PLAY"));
#elif defined(DC_TAS_MENU)
    tas_slice_setup(NULL, DC_TAS_ROOT "/tas_slices.bin", NULL);   // the debug menu picks the slices
#else
    static char playlist[1024];
    FILE *pf = fopen(DC_TAS_ROOT "/tas_play.txt", "rb");
    if (pf != NULL) {
        size_t n = fread(playlist, 1, sizeof(playlist) - 1, pf);
        playlist[n] = 0;
        fclose(pf);
        tas_slice_setup(NULL, DC_TAS_ROOT "/tas_slices.bin", playlist);
    }
#endif
}

// The game frame the movie has reached (the profiler logs it with each scene).
unsigned int dc_tas_frame(void) {
    return sTasNext;
}
unsigned int dc_tas_samples(void) {
    return sTasSamples;
}

// TAS slices (src/game/tas_slice.c): play on from another sample.
void dc_tas_seek(unsigned int sample) {
    sTasNext = sample;
    sTasOn = 1;
}

// The debug menu's replay is over (tas_slice.c): the controller plays again.
void dc_tas_stop(void) {
    sTasOn = 0;
}
#endif

void osContGetReadData(OSContPad *pad) {
    size_t i;

    pad->button = 0;
    pad->stick_x = 0;
    pad->stick_y = 0;
    pad->errnum = 0;
#ifdef DC_TAS
#ifdef DC_TAS_MENU
    if (sTasOn && sTasNext >= sTasSamples) {
        sTasOn = 0;   // the whole run is over, after the credits: the controller plays on
    }
#endif
    if (sTas != NULL && sTasOn) {
        if (sTasNext < sTasSamples) {
            const unsigned char *in = sTas + 0x400 + sTasNext * 4;
            pad->button = (u16) (in[0] << 8 | in[1]);
            pad->stick_x = (s8) in[2];
            pad->stick_y = (s8) in[3];
            if (++sTasNext % 1800 == 0) printf("TAS: frame %u\n", sTasNext);
        }
        return;
    }
#endif

    for (i = 0; i < sizeof(controller_implementations) / sizeof(struct ControllerAPI *); i++) {
        controller_implementations[i]->read(pad);
    }
}
