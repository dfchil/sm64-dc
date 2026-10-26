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

void send_display_list(struct SPTask *spTask) {
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
// Recording session (make DC_AUDIO_RECORD=1): instead of the game, play each sequence of
// DC_AUDIO_RECORD_SEQS and each sound of DC_AUDIO_RECORD_SOUNDS (comma-separated, hex) in turn
// while the AICA driver's command log drains to /pc/rec_seqXX.bin or /pc/rec_sfx_XXXXXXXX.bin,
// then exit to dcload. The game loop's 30 Hz tick is what lets the audio thread process requests,
// so it is signalled here every other vblank. An item ends by itself: a sequence one second after
// its second jump back (two passes, what tools/aica/sm64_afx.py needs to loop it), or when nothing
// has sounded for two seconds; a sound when nothing has sounded for one second; or at
// DC_AUDIO_RECORD_SECONDS.
#include "audio/aica_record.h"
#include "audio/data.h"
#include "audio/external.h"
#include "audio/load.h"
#include "seq_ids.h"
#ifndef DC_AUDIO_RECORD_SEQS
#define DC_AUDIO_RECORD_SEQS ""
#endif
#ifndef DC_AUDIO_RECORD_SOUNDS
#define DC_AUDIO_RECORD_SOUNDS ""
#endif
typedef struct {
    int voices, started, jumps;
    uint64_t quiet_since, second_jump;
    u32 targets[16];   /* jump targets seen: the loop is the second jump to one of them */
} RecWatch;
static u32 rec_drain(FILE *f, RecWatch *w, uint64_t now) {
    u32 n = 0;
    while (gAicaRecTail != gAicaRecHead) {
        const AicaRecord *r = aica_record_at(gAicaRecTail);
        if (f != NULL) fwrite(r, sizeof(AicaRecord), 1, f);
        if (r->cmd == AICA_REC_START) { w->voices++; w->started = 1; }
        if (r->cmd == AICA_REC_STOP && w->voices > 0) w->voices--;
        if (r->cmd == AICA_REC_SEQ_JUMP && r->ch == SEQ_PLAYER_LEVEL) {
            int seen = 0;
            for (int i = 0; i < w->jumps && i < 16; i++) seen |= w->targets[i] == r->key;
            if (w->jumps < 16) w->targets[w->jumps] = r->key;
            w->jumps++;
            if (seen && !w->second_jump) w->second_jump = now;
        }
        gAicaRecTail++;
        n++;
    }
    if (w->voices) w->quiet_since = 0;
    else if (!w->quiet_since) w->quiet_since = now;
    return n;
}
static uint64_t sRecLastVbl;
static void rec_tick(void) {
    while (vblticker < sRecLastVbl + 2) {
#ifdef KOS_GENWAIT_3ARG
        genwait_wait((void *) &vblticker, NULL, 0);
#else
        genwait_wait((void *) &vblticker, NULL, 0, NULL);
#endif
    }
    sRecLastVbl = vblticker;
    audio_signal_game_loop_tick();
}
static void rec_item(int seq, u32 sound) {
    static f32 sPos[3];
    static int sSfxPlayer;
    char path[64];
    if (seq >= 0) snprintf(path, sizeof(path), "/pc/rec_seq%02X.bin", seq);
    else snprintf(path, sizeof(path), "/pc/rec_sfx_%08lX.bin", (unsigned long) sound);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        printf("AUDIOREC cannot write %s\n", path);
        return;
    }
    AicaRecordHeader h;
    memset(&h, 0, sizeof(h));
    h.magic = AICA_RECORD_MAGIC;
    h.updates_per_frame = (u32) gAudioUpdatesPerFrame;
    h.wave_count = 4;
    h.band_count = 1;
    h.wave_samples = 64;
    fwrite(&h, sizeof(h), 1, f);
    for (int w = 0; w < 4; w++) {
        fwrite(gWaveSamples[w], sizeof(s16), 64, f);
    }
    if (seq < 0 && !sSfxPlayer) {
        // Sounds play through the SFX sequence player, which the game starts in its own init.
        play_music(SEQ_PLAYER_SFX, SEQUENCE_ARGS(0, SEQ_SOUND_PLAYER), 0);
        for (int i = 0; i < 15; i++) rec_tick();
        sSfxPlayer = 1;
    }
    RecWatch w;
    memset(&w, 0, sizeof(w));
    rec_drain(NULL, &w, 0);   // whatever came before this item does not belong to it
    memset(&w, 0, sizeof(w));
    u32 sub0 = gAicaRecTick;
    uint64_t us0 = timer_us_gettime64(), start = timer_ms_gettime64();
    if (seq >= 0) play_music(SEQ_PLAYER_LEVEL, SEQUENCE_ARGS(4, seq), 0);
    else play_sound((s32) sound, sPos);
    // A sound without the 0x80 flag is continuous: the game keeps it alive by requesting it every
    // frame and stops it when the requests stop. Hold it for four seconds, then let it go.
    int hold = (seq < 0 && !(sound & 0x80)) ? 120 : 0;
    u32 records = 0;
    for (;;) {
        rec_tick();
        if (hold && --hold) play_sound((s32) sound, sPos);
        uint64_t now = timer_ms_gettime64();
        records += rec_drain(f, &w, now);
        if (now - start >= (uint64_t) DC_AUDIO_RECORD_SECONDS * 1000) break;
        if (w.second_jump && now - w.second_jump >= 1000) break;
        if (w.started && w.quiet_since && now - w.quiet_since >= (seq >= 0 ? 2000u : 1000u)) break;
        if (!w.started && now - start >= 5000) break;
    }
    u32 subs = gAicaRecTick - sub0;
    u32 us = (u32) (timer_us_gettime64() - us0);
    AicaRecord end = { gAicaRecTick, AICA_REC_END, 0, 0, 0, us, subs };
    fwrite(&end, sizeof(end), 1, f);
    fclose(f);
    printf("AUDIOREC %s: %u records, %d jumps, %u sub-updates in %u us\n", path, (unsigned) records, w.jumps,
           (unsigned) subs, (unsigned) us);
    // Silence before the next item: stop the music and let every voice end.
    if (seq >= 0) stop_background_music(SEQUENCE_ARGS(4, seq));
    uint64_t t0 = timer_ms_gettime64();
    do {
        rec_tick();
        rec_drain(NULL, &w, timer_ms_gettime64());
    } while ((w.voices || timer_ms_gettime64() - t0 < 1000) && timer_ms_gettime64() - t0 < 8000);
}
static void audio_record_session(void) {
    sRecLastVbl = vblticker;
    printf("AUDIOREC updates_per_frame=%d seqs=%s sounds=%s\n", (int) gAudioUpdatesPerFrame, DC_AUDIO_RECORD_SEQS,
           DC_AUDIO_RECORD_SOUNDS);
    for (const char *p = DC_AUDIO_RECORD_SEQS; *p;) {
        char *e;
        unsigned long v = strtoul(p, &e, 16);
        if (e == p) break;
        rec_item((int) v, 0);
        p = *e == ',' ? e + 1 : e;
    }
    for (const char *p = DC_AUDIO_RECORD_SOUNDS; *p;) {
        char *e;
        unsigned long v = strtoul(p, &e, 16);
        if (e == p) break;
        rec_item(-1, (u32) v);
        p = *e == ',' ? e + 1 : e;
    }
    printf("AUDIOREC done, %u dropped\n", (unsigned) gAicaRecDropped);
    dc_prof_stop_audio_if_any();
    arch_exit();
}
#endif

static u8 pool[0x165000+0x70800] __attribute__((aligned(16384)));

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
#endif
    thread5_game_loop(NULL);

    inited = 1;

    while (1) {
#ifdef DC_60FPS
        const unsigned int late0 = gfx_dc_vbl_late;
#endif
        gfx_start_frame();
        game_loop_one_iteration();
        gfx_end_frame();
#if DC_OVERLAY
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
#if DC_OVERLAY
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
#if DC_OVERLAY
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
}


#include <kos.h>
int main(UNUSED int argc, UNUSED char *argv[]) {
    mmu_init_basic();
    mmu_page_map_static(0, 0x0C000000, PAGE_SIZE_1M, MMU_ALL_RDWR, true);

    main_func();
    return 0;
}
