// tasref's input plugin: plays a Mupen64 movie (version 3, one controller) and, before handing
// the game each sample, writes the line pc_endjinn_main.c's SM64_TRACE writes, read from the N64
// game's RAM (US ROM addresses).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define M64P_PLUGIN_PROTOTYPES 1
#include <mupen64plus/m64p_common.h>
#include <mupen64plus/m64p_plugin.h>

static unsigned char *(*sRdram)(void);
static unsigned char *sMovie;
static unsigned int sSamples, sNext, sDone;
static FILE *sTrace;
static long sDumpAt[8];
static int sDumps;

EXPORT void CALL tasref_setup(unsigned char *(*rdram)(void), unsigned char *movie, unsigned int samples,
                              FILE *trace) {
    sRdram = rdram;
    sMovie = movie;
    sSamples = samples;
    sTrace = trace;
    // TASREF_DUMP_AT=n[,n...]: write RDRAM (8 MB, as Mupen keeps it: native 32-bit words) to
    // tasref_rdram_<n>.bin at sample n, to find where something lives in the N64 game's memory.
    const char *at = getenv("TASREF_DUMP_AT");
    while (at != NULL && *at && sDumps < 8) {
        char *end;
        sDumpAt[sDumps++] = strtol(at, &end, 10);
        at = *end == ',' ? end + 1 : NULL;
    }
}

// Frames since the movie ended (the frontend stops a while after).
EXPORT unsigned int CALL tasref_done(void) {
    return sDone;
}

EXPORT unsigned int CALL tasref_sample(void) {
    return sNext;
}

// Mupen keeps RDRAM as native 32-bit words.
static uint32_t rd32(uint32_t a) {
    return *(const uint32_t *) (sRdram() + (a & 0x7FFFFC));
}

static uint32_t rd16(uint32_t a) {
    uint32_t w = rd32(a);
    return a & 2 ? w & 0xFFFF : w >> 16;
}

#define MARIO 0x8033B170    // gMarioStates[0]
#define LAKITU 0x8033C698   // gLakituState (its curFocus, then curPos)

static void trace(unsigned int sample) {
    if (sTrace == NULL) {
        return;
    }
    fprintf(sTrace, "%u t=%u L=%d A=%d act=%08x st=%04x tm=%04x pos=%08x,%08x,%08x vel=%08x,%08x,%08x "
                    "fv=%08x yaw=%04x rng=%04x c=%d s=%d cf=%08x,%08x,%08x cp=%08x,%08x,%08x\n",
            sample, rd32(0x8032D5D4), (int16_t) rd16(0x8032DDF8), (int16_t) rd16(0x8033BACA), rd32(MARIO + 0x0C),
            rd16(MARIO + 0x18), rd16(MARIO + 0x1A), rd32(MARIO + 0x3C), rd32(MARIO + 0x40), rd32(MARIO + 0x44),
            rd32(MARIO + 0x48), rd32(MARIO + 0x4C), rd32(MARIO + 0x50), rd32(MARIO + 0x54), rd16(MARIO + 0x2E),
            rd16(0x8038EEE0), (int16_t) rd16(MARIO + 0xA8), (int16_t) rd16(MARIO + 0xAA), rd32(LAKITU),
            rd32(LAKITU + 4), rd32(LAKITU + 8), rd32(LAKITU + 12), rd32(LAKITU + 16), rd32(LAKITU + 20));
}

EXPORT void CALL GetKeys(int control, BUTTONS *keys) {
    (void) control;
    keys->Value = 0;
}

// The controller's side of the PIF (RawData): buf is a channel's [tx length, rx length, command,
// ..., reply]. Status queries come every frame besides the reads; only a read (command 1) takes
// a movie sample, as when the movie was recorded.
EXPORT void CALL ReadController(int control, unsigned char *buf) {
    if (control < 0 || buf == NULL) {
        return;
    }
    unsigned char *rx = buf + 2 + (buf[0] & 0x3F);
    if (control != 0) {
        buf[1] |= 0x80;   // no controller
        return;
    }
    switch (buf[2]) {
        case 0x00:
        case 0xFF:   // status: a standard controller, no pak
            rx[0] = 0x05;
            rx[1] = 0x00;
            rx[2] = 0x02;
            break;
        case 0x01:
            if (sNext < sSamples) {
                trace(sNext);
                for (int i = 0; i < sDumps; i++) {
                    if ((long) sNext == sDumpAt[i]) {
                        char name[64];
                        snprintf(name, sizeof(name), "tasref_rdram_%u.bin", sNext);
                        FILE *f = fopen(name, "wb");
                        if (f != NULL) {
                            fwrite(sRdram(), 1, 0x800000, f);
                            fclose(f);
                        }
                    }
                }
                const unsigned char *in = sMovie + 0x400 + sNext * 4;
                rx[0] = in[0];
                rx[1] = in[1];
                rx[2] = in[2];
                rx[3] = in[3];
                sNext++;
            } else {
                rx[0] = rx[1] = rx[2] = rx[3] = 0;
                sDone++;
            }
            break;
        default:
            buf[1] |= 0x80;
            break;
    }
}

EXPORT void CALL InitiateControllers(CONTROL_INFO info) {
    info.Controls[0].Present = 1;
    info.Controls[0].RawData = 1;
    info.Controls[0].Plugin = PLUGIN_NONE;
    for (int i = 1; i < 4; i++) {
        info.Controls[i].Present = 0;
    }
}

EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle core, void *context,
                                     void (*debug)(void *, int, const char *)) {
    (void) core, (void) context, (void) debug;
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginShutdown(void) {
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type *type, int *version, int *api, const char **name,
                                        int *caps) {
    if (type) *type = M64PLUGIN_INPUT;
    if (version) *version = 0x010000;
    if (api) *api = 0x020100;
    if (name) *name = "tasref input";
    if (caps) *caps = 0;
    return M64ERR_SUCCESS;
}

EXPORT int CALL RomOpen(void) {
    return 1;
}

EXPORT void CALL RomClosed(void) {}
EXPORT void CALL ControllerCommand(int control, unsigned char *command) { (void) control, (void) command; }
EXPORT void CALL SDL_KeyDown(int keymod, int keysym) { (void) keymod, (void) keysym; }
EXPORT void CALL SDL_KeyUp(int keymod, int keysym) { (void) keymod, (void) keysym; }
EXPORT void CALL RenderCallback(void) {}
