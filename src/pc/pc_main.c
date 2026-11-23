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

#include "compat.h"
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

static int sFrameQueued;   // send_display_list ran in this frame (the enDjinn loop below)

void send_display_list(struct SPTask *spTask) {
    extern s32 tas_show_fast(void);
    extern s32 mtx_patch_interpolated(void);
    if (tas_show_fast()) {   // Show star's way back: the steps run, nothing is drawn
        mtx_patch_interpolated();   // empties its table, which points into this display list
        return;
    }
    sFrameQueued = 1;
    if (gDcFrameMode == DC_FRAME_MODE_30) {   // the exact matrices now; the second draw then finds
        extern s32 mtx_patch_interpolated(void);   // nothing to change and holds this frame
        mtx_patch_interpolated();
    }
    gfx_run((Gfx *)spTask->task.t.data_ptr);
}

#include <kos.h>
#include <enDjinn/enj_mode.h>
#include <enDjinn/enj_render.h>
#include <enDjinn/enj_state.h>
#include <enDjinn/enj_debug_menu.h>
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
// if you notice the sound starts skipping, re-enable the irq_disable/enable around synthesis
//        irq_disable();
        // num samples is 448
        create_next_audio_buffer(audio_buffer[0], audio_buffer[1], 448);
        audio_api->play((u8 *)audio_buffer[0], (u8 *)audio_buffer[1], 1792);
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

static u8 pool[0x165000+0x70800] __attribute__((aligned(16384)));

#include "pc/loop/enj_loop.inc.c"

void main_func(void) {
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
    
    if (audio_api == NULL) {
        audio_api = &audio_null;
    }

    audio_init();
    sound_init();

    vblank_handler_add(&vblfunc, NULL);
    // No sequencer on the SH4: the music and sounds are AICAflow flows (src/pc/audio/sm64_aicaflow.c).
    {
        extern int sm64_afx_init(void);
        sm64_afx_init();
    }
//    (void)audio_attr;
    thread5_game_loop(NULL);

    inited = 1;

    enj_loop_run();
}


#include <kos.h>
int main(UNUSED int argc, UNUSED char *argv[]) {
    mmu_init_basic();
    mmu_page_map_static(0, 0x0C000000, PAGE_SIZE_1M, MMU_ALL_RDWR, true);

    main_func();
    return 0;
}
