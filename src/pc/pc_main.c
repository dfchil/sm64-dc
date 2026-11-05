#include <stdlib.h>

#ifdef TARGET_WEB
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

#include "sm64.h"

#include "game/memory.h"
#include "audio/external.h"

#include "gfx/gfx_pc.h"
#include "gfx/gfx_dc.h"

#include "audio/audio_api.h"
#include "audio/audio_dc.h"
#include "audio/audio_null.h"

#include "configfile.h"
#include "dc_prof.h"

#include "compat.h"
#ifndef DC_HOLD_IDENTICAL
#define DC_HOLD_IDENTICAL 1
#endif
uint64_t gSysFrameCount = 0;

#undef CONT_C
#undef CONT_B
#undef CONT_A
#undef CONT_START
#undef CONT_DPAD_UP
#undef CONT_DPAD_DOWN
#undef CONT_DPAD_LEFT
#undef CONT_DPAD_RIGHT
#undef CONT_Z
#undef CONT_Y
#undef CONT_X
#undef CONT_D
#undef CONT_DPAD2_UP
#undef CONT_DPAD2_DOWN
#undef CONT_DPAD2_LEFT
#undef CONT_DPAD2_RIGHT


#define CONFIG_FILE "sm64config.txt"

OSMesg D_80339BEC;
OSMesgQueue gSIEventMesgQueue;

s8 gResetTimer;
s8 D_8032C648;
s8 gDebugLevelSelect;
s8 gShowProfiler;
s8 gShowDebugText;

static struct AudioAPI *audio_api;
static struct GfxWindowManagerAPI *wm_api;
static struct GfxRenderingAPI *rendering_api;

extern void gfx_run(Gfx *commands);
extern void thread5_game_loop(void *arg);

extern void create_next_audio_buffer(s16 *samplesL, s16* samplesR, u32 num_samples);
void game_loop_one_iteration(void);

void dispatch_audio_sptask(UNUSED struct SPTask *spTask) {
}

void set_vblank_handler(UNUSED s32 index, UNUSED struct VblankHandler *handler, UNUSED OSMesgQueue *queue, UNUSED OSMesg *msg) {
}

static uint8_t inited = 0;

#include "game/game_init.h" // for gGlobalTimer
#include "pc/dc_overlay.h"

#ifdef DC_60FPS
#include "pc/dc_video_options.h"
#include <dc/pvr.h>
int gDcFrameMode = DC_FRAME_MODE;
extern unsigned int gfx_dc_vbl_late;

// DC_FRAME_MODE_60_DROP: after the step's first (interpolated) frame was handed over and paced to
// its vblank, is it late? Late if it was handed over after that vblank (CPU), or if the PVR's last
// render took longer than a frame: render times hold from frame to frame, so this one will too.
// (Counting KOS's flips instead does not work: two frames are normally in flight at this point.)
#ifndef DC_LATEDROP_RENDER_US
#define DC_LATEDROP_RENDER_US 16700
#endif
static int first_frame_late(unsigned int late_before) {
    pvr_stats_t st;
    pvr_get_stats(&st);
    return gfx_dc_vbl_late != late_before || st.rnd_last_time > DC_LATEDROP_RENDER_US * 1000ull;
}
#endif

#if DC_ENJ_LOOP
static int sFrameQueued;   // send_display_list ran in this frame (the enDjinn loop below)
#endif

void send_display_list(struct SPTask *spTask) {
#if DC_ENJ_LOOP
    sFrameQueued = 1;
#endif
#ifdef DC_60FPS
    if (gDcFrameMode == DC_FRAME_MODE_30) {   // the exact matrices now; the second draw then finds
        extern s32 mtx_patch_interpolated(void);   // nothing to change and holds this frame
        mtx_patch_interpolated();
    }
#endif
#ifndef DC_60FPS
    // The scene walk always writes the interpolated matrices (rendering_graph_node.c). Without the
    // 60 fps double draw, patch the exact ones in before the only draw of this logic step.
    extern s32 mtx_patch_interpolated(void);
    mtx_patch_interpolated();
#endif
    gfx_run((Gfx *)spTask->task.t.data_ptr);
}

#include <kos.h>
#if DC_ENJ_LOOP
#include <enDjinn/enj_mode.h>
#include <enDjinn/enj_render.h>
#include <enDjinn/enj_state.h>
#if DC_DEBUG_MENU
#include <enDjinn/enj_debug_menu.h>
#endif
#endif
#undef bool
#undef true
#undef false
#define bool uint8_t
#define false 0
#define true 1
//void create_thread(OSThread *thread, OSId id, void (*entry)(void *), void *arg, void *sp, OSPri pri);
volatile uint64_t vblticker=0;
void vblfunc(uint32_t c, void *d) {
	(void)c;
	(void)d;
    vblticker++;
    genwait_wake_all/* one */((void*)&vblticker);
    //thd_schedule(false, 0);
}    

#define SAMPLES_HIGH 448 
//464
//#define SAMPLES_LOW 448
//432
s16 audio_buffer[2][SAMPLES_HIGH * 2 * 2 * 3] __attribute__((aligned(64)));

#if defined(DC_PROF) || defined(DC_AUDIO_RECORD)
// Set before the profile build exits: KOS shuts the sound driver down in arch_exit, and an
// audio thread still queueing AICA commands then crashes in snd_sh4_to_aica.
static volatile int sAudioThreadStop = 0;
void dc_prof_stop_audio(void) {
    sAudioThreadStop = 1;
}
#define dc_prof_stop_audio_if_any() dc_prof_stop_audio()
#else
#define dc_prof_stop_audio_if_any() ((void) 0)
#endif

void *AudioSynthesisThread(UNUSED void *arg) {
    uint64_t last_vbltick = vblticker;

    while (1) {
        while (vblticker <= last_vbltick)
#ifdef KOS_GENWAIT_3ARG
            genwait_wait((void*)&vblticker, NULL, 0);
#else
            genwait_wait((void*)&vblticker, NULL, 0/* 3 */, NULL);
#endif
        last_vbltick = vblticker;
#if defined(DC_PROF) || defined(DC_AUDIO_RECORD)
        if (sAudioThreadStop) {
            return NULL;
        }
#endif
// if you notice the sound starts skipping, re-enable the irq_disable/enable around synthesis
//        irq_disable();
        // num samples is 448
#ifdef DC_PROF
        uint64_t audio_start = dc_prof_now_us();
#endif
        create_next_audio_buffer(audio_buffer[0], audio_buffer[1], 448);
        audio_api->play((u8 *)audio_buffer[0], (u8 *)audio_buffer[1], 1792);
#ifdef DC_PROF
        dc_prof_audio_add((unsigned int) (dc_prof_now_us() - audio_start));
#endif
//        irq_enable();
    }
    return NULL;
}

extern int gProcessAudio;
void produce_one_frame(void) {
    gfx_start_frame();
    game_loop_one_iteration();
    gfx_end_frame();
}

static void save_config(void) {
    configfile_save(CONFIG_FILE);
}

static void on_fullscreen_changed(bool is_now_fullscreen) {
    configFullscreen = is_now_fullscreen;
}

void *main_pc_pool = NULL;
void *main_pc_pool_gd = NULL;

#ifdef DC_AUDIO_RECORD
static uint64_t sRecLastVbl;
static void rec_wait_tick(void) {
    if (sRecLastVbl == 0) sRecLastVbl = vblticker;
    while (vblticker < sRecLastVbl + 2) {
#ifdef KOS_GENWAIT_3ARG
        genwait_wait((void *) &vblticker, NULL, 0);
#else
        genwait_wait((void *) &vblticker, NULL, 0, NULL);
#endif
    }
    sRecLastVbl = vblticker;
}
static uint64_t rec_now_us(void) { return timer_us_gettime64(); }
static const char *rec_dir(void) { return "/pc/"; }
static int rec_seconds(int seq) { (void) seq; return DC_AUDIO_RECORD_SECONDS; }
#ifndef DC_AUDIO_RECORD_SEQS
#define DC_AUDIO_RECORD_SEQS ""
#endif
#ifndef DC_AUDIO_RECORD_SOUNDS
#define DC_AUDIO_RECORD_SOUNDS ""
#endif
static const char *rec_sequences(void) { return DC_AUDIO_RECORD_SEQS; }
static const char *rec_sounds(void) { return DC_AUDIO_RECORD_SOUNDS; }
#include "pc/record/audio_record.inc.c"
#endif

static u8 pool[0x165000+0x70800] __attribute__((aligned(16384)));

#if DC_ENJ_LOOP
// enDjinn's enj_state_run is the frame loop. Each of its frames is one scene of the game's: a logic
// step, whose display list send_display_list queues as the opaque list's renderer (gfx_pvr.c), or at
// 60 fps the step's second, exact draw of that list, or the last scene shown again. enDjinn then
// submits the scene, and its post-render callback ends the frame as the plain loop in main_func does
// after each draw: the scene's end, the pacing to its vblank, the profiler's frame.
extern void gfx_dc_hold_frame(void);
static int sFrameHeld;    // this frame shows the last scene again
static int sStepped;      // a logic step has run
#ifdef DC_60FPS
static int sExactNext;    // the next frame is the step's exact draw,
static int sDropExact;    // unless the interpolated one was late (DC_FRAME_MODE_60_DROP)
static unsigned int sLate0;
#endif

static void enj_frame_step(void) {
    if (sStepped) {
        thd_pass();   // between steps, as the plain loop
    }
    sStepped = 1;
#ifdef DC_60FPS
    sLate0 = gfx_dc_vbl_late;
    sExactNext = 1;
#endif
    sFrameQueued = 0;
    gfx_start_frame();
    game_loop_one_iteration();
    sFrameHeld = !sFrameQueued;   // nothing drawn (the reset bars): the screen stays
    if (sFrameHeld) {
        enj_render_hold_frame();
    }
}

static void enj_frame(UNUSED void *data) {
#ifdef DC_60FPS
    if (sExactNext) {
        extern s32 mtx_patch_interpolated(void);
        sExactNext = 0;
        if (sDropExact) {
            mtx_patch_interpolated();   // still empties its table (it points into this display list)
            DC_PROF_COUNT(DCC_DROPPED, 1);
#if DC_OVERLAY_CODE
            dc_overlay_count_skip();
#endif
        } else if (!gResetTimer) {
            // When the exact matrices equal the interpolated ones the second draw would repeat the
            // first: show that frame again.
            sFrameHeld = !(mtx_patch_interpolated() || !DC_HOLD_IDENTICAL);
            if (sFrameHeld) {
                enj_render_hold_frame();
                DC_PROF_COUNT(DCC_HELD, 1);
            } else {
                gfx_start_frame();
                send_display_list(gGfxSPTask);
            }
            return;
        }
    }
#endif
    enj_frame_step();
}

#if DC_DEBUG_MENU
// enDjinn's debug menu (third_party/endjinn enj_debug_menu.h): hold both triggers and press
// Up+X+Y. It is a mode on top of the game's, so no logic step runs while it is open; behind it the
// game's last display list is drawn again as it was last drawn, and those frames are the menu's,
// not the game's (enj_frame_shown). The quick font it draws with is the overlay's (dc_overlay.c),
// which a PVR restart drops and loads again.

static int sMenuFrame;   // this frame is the one behind the menu

static void dbg_menu_behind(UNUSED void *data) {
    sMenuFrame = 1;
    dc_overlay_font_ready();
    if (sStepped && gGfxSPTask != NULL) {   // else nothing has been drawn: the background colour
        gfx_run((Gfx *) gGfxSPTask->task.t.data_ptr);
    }
}

#ifdef DC_60FPS
static const char *const sDbgFrameModes[] = { "30 fps", "60 fps", "60 fps, drops" };
#endif
#if DC_FSAA
extern int gDcFsaaMode;
extern void gfx_pvr_fsaa_mode_changed(void);
static const char *const sDbgFsaaModes[] = { "Off", "On", "Auto" };

static void dbg_fsaa_changed(UNUSED enj_menu_row_t *row, UNUSED void *data) {
    gfx_pvr_fsaa_mode_changed();   // the PVR restarts before the game's next frame
}
#endif
static const char *const sDbgOffOn[] = { "Off", "On" };
#if defined(DC_AICAFLOW) && DC_AICAFLOW_REVERB
#include "audio/sm64_aicaflow.h"

static void dbg_reverb_changed(UNUSED enj_menu_row_t *row, UNUSED void *data) {
    sm64_afx_reverb_returns();
}
#endif

// The player's video options (dc_video_options.h) and the overlay.
static enj_menu_row_t sDbgRows[] = {
#ifdef DC_60FPS
    { .name = "Frame rate",
      .help = "30 fps shows each logic step for two refreshes. 60 fps adds a frame between steps: "
              "a late one slows the game, or with drops it is skipped.",
      .type = ENJ_MENU_CHOICE,
      .choice = { .value = &gDcFrameMode, .labels = sDbgFrameModes, .count = 3 } },
#endif
#if DC_FSAA
    { .name = "FSAA",
      .help = "Twice the width, filtered down. Auto: off in the water and lava courses. Switches "
              "when the menu closes, and the textures load again.",
      .type = ENJ_MENU_CHOICE,
      .choice = { .value = &gDcFsaaMode, .labels = sDbgFsaaModes, .count = 3 },
      .on_change = dbg_fsaa_changed,
      .apply_on_close = 1 },
#endif
#if defined(DC_AICAFLOW) && DC_AICAFLOW_REVERB
    { .name = "Reverb",
      .help = "The N64's echo, as the AICA DSP's program. Off silences its output; the program "
              "runs on. Changes at once.",
      .type = ENJ_MENU_CHOICE,
      .choice = { .value = &gDcReverbOn, .labels = sDbgOffOn, .count = 2 },
      .on_change = dbg_reverb_changed },
#endif
    { .name = "Show FPS",
      .help = "The frame rate, late frames (MISS) and skipped ones (SKIP) of the scene, in the "
              "bottom left corner.",
      .type = ENJ_MENU_CHOICE,
      .choice = { .value = &gDcOverlayOn, .labels = sDbgOffOn, .count = 2 } },
};

static enj_debug_menu_t sDbgMenu = {
    .title = "Super Mario 64",
    .rows = sDbgRows,
    .row_count = sizeof(sDbgRows) / sizeof(sDbgRows[0]),
    .draw_behind = dbg_menu_behind,
};
#endif

static void enj_frame_shown(UNUSED void *data) {
#if DC_DEBUG_MENU
    if (sMenuFrame) {   // behind the debug menu: shown and paced, but none of the game's frames
        sMenuFrame = 0;
        gfx_end_frame();
        return;
    }
#endif
    if (sFrameHeld) {
        gfx_dc_hold_frame();
    } else {
        DC_PROF_MARK(DCP_DL);
        gfx_end_frame();
#if DC_OVERLAY_CODE
        dc_overlay_count_draw();
#endif
    }
#ifdef DC_60FPS
    if (sExactNext) {   // the step's own frame: decide its exact draw now, as the plain loop does
        sDropExact = gDcFrameMode == DC_FRAME_MODE_60_DROP && first_frame_late(sLate0);
    }
#endif
#ifdef DC_PROF
    dc_prof_frame_end();
#endif
    gSysFrameCount++;
}

static void enj_loop_run(void) {
    static enj_mode_t sGameMode;
    sGameMode.mode_updater = enj_frame;
    sGameMode.no_soft_reset = 1;
    strncpy(sGameMode.name, "sm64", sizeof(sGameMode.name) - 1);
    enj_render_post_callback_set(enj_frame_shown, NULL);
    enj_mode_push(&sGameMode);
#if DC_DEBUG_MENU
    if (enj_state_get()->flags.no_input_init) {
        // The menu's buttons are read through enDjinn's controller layer, which the game's own
        // maple code (controller_dc.c) leaves unstarted.
        extern void enj_ctrl_init_local_devices(void);
        enj_ctrl_init_local_devices();
    }
    enj_debug_menu_set(&sDbgMenu);
#endif
    enj_state_run();
}
#endif

void main_func(void) {
#ifdef DC_PROF
    dc_prof_init();
#endif
    main_pc_pool = &pool;
    main_pc_pool_gd = &pool[0x165000];

    main_pool_init(pool, pool + sizeof(pool) / sizeof(pool[0]));
    gEffectsMemoryPool = mem_pool_init(0x4000, MEMORY_POOL_LEFT);

    configfile_load(CONFIG_FILE);
    atexit(save_config);

    extern struct GfxRenderingAPI gfx_pvr_api;
    rendering_api = &gfx_pvr_api;   // make GFX_BACKEND=pvr
    wm_api = &gfx_dc;

    gfx_init(wm_api, rendering_api, "Super Mario 64", configFullscreen);
    
    wm_api->set_fullscreen_changed_callback(on_fullscreen_changed);
    
#ifndef DC_AICAFLOW
    // DC_AICAFLOW: AICAflow loads its own AICA firmware; KOS's sound driver stays off.
    if (audio_api == NULL && audio_dc.init()) {
        audio_api = &audio_dc;
    }
#endif

    if (audio_api == NULL) {
        audio_api = &audio_null;
    }

    audio_init();
    sound_init();

    vblank_handler_add(&vblfunc, NULL);
#ifdef DC_AICAFLOW
    // No sequencer on the SH4: the music and sounds are AICAflow flows (src/pc/audio/sm64_aicaflow.c).
    {
        extern int sm64_afx_init(void);
        sm64_afx_init();
    }
#else
    kthread_attr_t audio_attr;
    audio_attr.create_detached = 1;
	audio_attr.stack_size = 32768;
	audio_attr.stack_ptr = NULL;
	audio_attr.prio = PRIO_DEFAULT - 1;
	audio_attr.label = "AudioSynthesis";
    thd_create_ex(&audio_attr, &AudioSynthesisThread, NULL);
#endif
//    (void)audio_attr;
#ifdef DC_AUDIO_RECORD
    audio_record_session();
    dc_prof_stop_audio_if_any();
    arch_exit();
#endif
    thread5_game_loop(NULL);

    inited = 1;

#if DC_ENJ_LOOP
    enj_loop_run();
#else
    while (1) {
#ifdef DC_60FPS
        const unsigned int late0 = gfx_dc_vbl_late;
#endif
        gfx_start_frame();
        game_loop_one_iteration();
        gfx_end_frame();
#if DC_OVERLAY_CODE
        dc_overlay_count_draw();
#endif
#ifdef DC_60FPS
        const int drop = gDcFrameMode == DC_FRAME_MODE_60_DROP && first_frame_late(late0);
#endif
#ifdef DC_PROF
        dc_prof_frame_end();
#endif
        gSysFrameCount++;
#ifdef DC_60FPS
        // 60 fps over SM64's 30 Hz logic (the PC 60fps patch's scheme): the display list the game
        // just built carries matrices interpolated halfway between the previous and the current
        // logic step, and was drawn above. Patch the real matrices back in and draw it again, so
        // each logic step shows as two frames, one vblank apart.
        if (drop) {   // the interpolated frame took this step's second vblank: no exact frame
            extern s32 mtx_patch_interpolated(void);
            mtx_patch_interpolated();   // still empties its table (it points into this display list)
            DC_PROF_COUNT(DCC_DROPPED, 1);
#if DC_OVERLAY_CODE
            dc_overlay_count_skip();
#endif
        } else if (!gResetTimer) {
            extern s32 mtx_patch_interpolated(void);
            extern void gfx_dc_hold_frame(void);
            // When the exact matrices equal the interpolated ones the second draw would repeat the
            // first (the title's goddard head is never interpolated): show that frame again.
            if (mtx_patch_interpolated() || !DC_HOLD_IDENTICAL) {
                gfx_start_frame();
                send_display_list(gGfxSPTask);
                gfx_end_frame();
#if DC_OVERLAY_CODE
                dc_overlay_count_draw();
#endif
            } else {
                gfx_dc_hold_frame();
                DC_PROF_COUNT(DCC_HELD, 1);
            }
#ifdef DC_PROF
            dc_prof_frame_end();
#endif
            gSysFrameCount++;
        }
#endif
        thd_pass();
    }
#endif
}


#include <kos.h>
int main(UNUSED int argc, UNUSED char *argv[]) {
    mmu_init_basic();
    mmu_page_map_static(0, 0x0C000000, PAGE_SIZE_1M, MMU_ALL_RDWR, true);

    main_func();
    return 0;
}
