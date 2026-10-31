// pc-endjinn host build only (pc_endjinn.mk): the main loop that src/pc/pc_main.c is on the
// Dreamcast. No vblank interrupt, audio thread or MMU here.
//
// Environment:
//   SM64_HEADLESS=1   no window and no rendering: the game logic runs uncapped, as fast as it can
//   SM64_TAS=<file>   play a Mupen64 movie instead of the controller (controller_entry_point.c)
//   SM64_TRACE=<file> with a movie: one line per movie sample, the state the game reads it in
//                     (tools/tasref compares it with Mupen64Plus running the N64 game)
//   SM64_LOG_EVERY=n  TASLOG period in game frames (default 300)
//   SM64_MAX_FRAMES=n stop after n game frames (default: 600 frames after a movie ends; none without)
//   SM64_UNCAPPED=1   windowed: no 60 Hz pacing
//   SM64_SCREENSHOT=<file.ppm> SM64_SCREENSHOT_AT=n  windowed: capture game frame n (pc_endjinn/capture.h)
//
// Every 300 game frames (SM64_LOG_EVERY) and on every level or area change it prints a TASLOG line (a game frame is
// one game loop iteration, i.e. one controller read, i.e. one movie sample).
#include <stdio.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <stdlib.h>
#include <string.h>

#include "sm64.h"
#include "game/memory.h"
#include "game/area.h"
#include "game/level_update.h"
#include "game/game_init.h"
#include "game/camera.h"
#include "game/tas_trace.h"
#include "game/object_list_processor.h"
#include "object_fields.h"
#include "audio/external.h"
#include "platform_info.h"

#include "pc/gfx/gfx_pc.h"
#include "pc/audio/audio_api.h"
#include "pc/audio/audio_null.h"
#include "pc/configfile.h"
#include "pc/dc_video_options.h"

#include <kos.h>
#include <pc_endjinn/capture.h>

// ---- what src/pc/pc_main.c defines on the Dreamcast ----
uint64_t gSysFrameCount = 0;
OSMesg D_80339BEC;
OSMesgQueue gSIEventMesgQueue;
s8 gResetTimer;
s8 D_8032C648;
s8 gDebugLevelSelect;
s8 gShowProfiler;
s8 gShowDebugText;
void *main_pc_pool = NULL;
void *main_pc_pool_gd = NULL;
int gDcFrameMode = DC_FRAME_MODE;

extern void thread5_game_loop(void *arg);
extern void game_loop_one_iteration(void);
extern void create_next_audio_buffer(s16 *samplesL, s16 *samplesR, u32 num_samples);
extern s32 mtx_patch_interpolated(void);
extern unsigned int dc_tas_frame(void);
extern unsigned int dc_tas_samples(void);

static int sHeadless;
static uint64_t sGameFrames;

void dispatch_audio_sptask(UNUSED struct SPTask *spTask) {
}

void set_vblank_handler(UNUSED s32 index, UNUSED struct VblankHandler *handler, UNUSED OSMesgQueue *queue,
                        UNUSED OSMesg *msg) {
}

void send_display_list(struct SPTask *spTask) {
    if (sHeadless || gDcFrameMode == DC_FRAME_MODE_30) {
        // The exact matrices now (and the table emptied: it points into this display list).
        mtx_patch_interpolated();
    }
    if (!sHeadless) {
        gfx_run((Gfx *) spTask->task.t.data_ptr);
    }
}

// The credits sync the cake to the music in wall-clock time (mario_actions_cutscene.c). Here the
// clock is the game's: 30 frames a second, so a run is the same however fast it goes.
u64 pc_realtime_us(void) {
    return sGameFrames * 1000000ull / 30u;
}

static void taslog(void) {
    struct MarioState *m = gMarioState;
    printf("TASLOG frame=%llu level=%d area=%d stars=%d pos=%.3f,%.3f,%.3f action=0x%08X\n",
           (unsigned long long) sGameFrames, gCurrLevelNum, gCurrAreaIndex, m->numStars, m->pos[0], m->pos[1],
           m->pos[2], (unsigned) m->action);
    fflush(stdout);
}

// SM64_TRACE: at each movie sample, before the game frame that reads it. Floats as their bits,
// so the comparison with tools/tasref's trace of the N64 game is exact.
static FILE *sTrace;

void pc_tas_trace(unsigned int sample) {
    if (sTrace == NULL) {
        return;
    }
    // SM64_DUMP_AT=n[,n...]: gLakituState at those samples (sm64_lakitu_<n>.bin), to compare with
    // an N64 RAM dump (tools/tasref, TASREF_DUMP_AT).
    for (const char *at = getenv("SM64_DUMP_AT"); at != NULL && *at;) {
        char *end;
        if (strtoul(at, &end, 10) == sample) {
            extern struct LakituState gLakituState;
            char name[64];
            snprintf(name, sizeof(name), "sm64_lakitu_%u.bin", sample);
            FILE *f = fopen(name, "wb");
            if (f != NULL) {
                fwrite(&gLakituState, 1, 0xC0, f);
                fclose(f);
            }
        }
        at = *end == ',' ? end + 1 : NULL;
    }
    // SM64_DUMP_GLOBALS_AT=n: the executable's whole __DATA segment (every global and static) to
    // sm64_globals_<n>.bin, to find what differs between two runs at one sample (a slice replay
    // against the full movie): tools/tasref/diff_globals.py maps the bytes to symbols.
    const char *globalsAt = getenv("SM64_DUMP_GLOBALS_AT");
    if (globalsAt != NULL && (unsigned) atoi(globalsAt) == sample) {
        unsigned long size;
        uint8_t *data = getsegmentdata(&_mh_execute_header, "__DATA", &size);
        char name[64];
        snprintf(name, sizeof(name), "sm64_globals_%u.bin", sample);
        FILE *f = fopen(name, "wb");
        if (f != NULL && data != NULL) {
            fwrite(data, 1, size, f);
            fclose(f);
        }
    }
    // SM64_DUMP_OBJS_AT=n: every active object at sample n to sm64_objs_<n>.txt (pool slot,
    // behavior as an offset from bhvMario, action, timer, position, the first raw fields).
    const char *objsAt = getenv("SM64_DUMP_OBJS_AT");
    if (objsAt != NULL && (unsigned) atoi(objsAt) == sample) {
        extern struct Object gObjectPool[];
        extern const BehaviorScript bhvMario[];
        char name[64];
        snprintf(name, sizeof(name), "sm64_objs_%u.txt", sample);
        FILE *f = fopen(name, "w");
        for (int i = 0; f != NULL && i < OBJECT_POOL_CAPACITY; i++) {
            struct Object *o = &gObjectPool[i];
            if (!(o->activeFlags & ACTIVE_FLAG_ACTIVE)) {
                continue;
            }
            fprintf(f, "beh %+ld act %d timer %d pos %.3f %.3f %.3f raw", (long) ((const char *) o->behavior - (const char *) bhvMario),
                    o->oAction, o->oTimer, o->oPosX, o->oPosY, o->oPosZ);
            for (int k = 0x1B; k < 0x22; k++) {
                fprintf(f, " %08x", (unsigned) o->rawData.asS32[k]);
            }
            fprintf(f, " slot %d\n", i);
        }
        if (f != NULL) {
            fclose(f);
        }
    }
    char line[256];
    tas_trace_line(line, sizeof(line), sample);
    fprintf(sTrace, "%s\n", line);
}

static uint64_t env_u64(const char *name, uint64_t def) {
    const char *v = getenv(name);
    return v != NULL && *v ? strtoull(v, NULL, 0) : def;
}

#define POOL_SIZE DOUBLE_SIZE_ON_64_BIT(0x165000 + 0x70800)
static u8 sPool[POOL_SIZE] __attribute__((aligned(16384)));

static s16 sAudioBuffer[2][448 * 2 * 2 * 3];

int main(UNUSED int argc, UNUSED char *argv[]) {
    sHeadless = env_u64("SM64_HEADLESS", 0) != 0;
    const char *shot = getenv("SM64_SCREENSHOT");
    uint64_t shotAt = env_u64("SM64_SCREENSHOT_AT", 300);

    main_pc_pool = sPool;
    main_pc_pool_gd = sPool + DOUBLE_SIZE_ON_64_BIT(0x165000);
    main_pool_init(sPool, sPool + sizeof(sPool));
    gEffectsMemoryPool = mem_pool_init(0x4000, MEMORY_POOL_LEFT);

    configfile_load("sm64config.txt");
    if (getenv("SM64_TRACE") != NULL) {
        sTrace = fopen(getenv("SM64_TRACE"), "w");
    }

    if (!sHeadless) {
        extern struct GfxRenderingAPI gfx_pvr_api;
        extern struct GfxWindowManagerAPI gfx_pc_endjinn_wm;
        gfx_init(&gfx_pc_endjinn_wm, &gfx_pvr_api, "Super Mario 64", false);
    }

    audio_init();
    sound_init();
    thread5_game_loop(NULL);

    uint64_t maxFrames = env_u64("SM64_MAX_FRAMES", 0);
    if (maxFrames == 0 && dc_tas_samples() != 0) {
        maxFrames = dc_tas_samples() + 600;
    }
    uint64_t logEvery = env_u64("SM64_LOG_EVERY", 300);
    if (logEvery == 0) logEvery = 300;
    s16 lastLevel = -1, lastArea = -1;
    uint64_t t0 = timer_ns_gettime64();

    for (;;) {
        if (!sHeadless) gfx_start_frame();
        game_loop_one_iteration();
        if (!sHeadless) gfx_end_frame();
        // One audio update per game frame: the sequences and the game's sound requests advance.
        create_next_audio_buffer(sAudioBuffer[0], sAudioBuffer[1], 448);
        sGameFrames++;
        gSysFrameCount++;

        if (sGameFrames % logEvery == 0 || gCurrLevelNum != lastLevel || gCurrAreaIndex != lastArea) {
            taslog();
            lastLevel = gCurrLevelNum;
            lastArea = gCurrAreaIndex;
        }
        if (!sHeadless && shot != NULL && sGameFrames == shotAt) {
            enj_pc_capture_next_frame(shot);
        }
        if (!sHeadless && gDcFrameMode != DC_FRAME_MODE_30 && !gResetTimer) {
            // 60 fps over the 30 Hz logic, as pc_main.c: the frame just drawn had matrices
            // interpolated halfway from the last step; patch the exact ones in and draw again.
            if (mtx_patch_interpolated()) {
                gfx_start_frame();
                gfx_run((Gfx *) gGfxSPTask->task.t.data_ptr);
                gfx_end_frame();
            } else {
                extern void gfx_pc_endjinn_hold_frame(void);
                gfx_pc_endjinn_hold_frame();
            }
        }
        extern int gTasSlicesDone;
        if ((maxFrames != 0 && sGameFrames >= maxFrames) || gTasSlicesDone) {
            break;
        }
    }
    if (sTrace != NULL) {
        fclose(sTrace);
    }
    uint64_t ns = timer_ns_gettime64() - t0;
    taslog();
    printf("HOST: %llu game frames in %.2f s (%.0f frames/s), movie at sample %u of %u\n",
           (unsigned long long) sGameFrames, ns / 1e9, sGameFrames / (ns / 1e9), dc_tas_frame(), dc_tas_samples());
    return 0;
}
