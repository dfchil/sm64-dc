#ifndef DC_PROF_H
#define DC_PROF_H

// Per-frame hardware profiler for the Dreamcast build (make DC_PROF=1).
// Marks are taken on the main thread at fixed points of a frame; everything
// is kept in RAM and printed / written to /pc only at exit, because dcload I/O
// masks interrupts and would distort the frames being measured.

enum DcProfMark {
    DCP_LOGIC,       // level script done (game logic for this tick)
    DCP_GEO,         // render_game done (scene graph walked, N64 display list built)
    DCP_WAIT_BEGIN,  // gfx_pvr_start_frame: before pvr_wait_ready
    DCP_WAIT_END,    // gfx_pvr_start_frame: after pvr_wait_ready
    DCP_DL,          // gfx_run done (display list translated and submitted)
    DCP_FINISH,      // pvr_scene_finish returned
    DCP_COUNT
};

// Per-frame event counters, summed per scene and printed as DCPROF COUNTS.
enum DcProfCount {
    DCC_TRIS,     // gfx_sp_tri1 calls
    DCC_FAST,     // triangles drawn from baked vertices
    DCC_BAKES,    // vertices baked
    DCC_EPOCHS,   // triangle-state epochs started (state commands)
    DCC_SETUPS,   // tri_state_setup runs
    DCC_VERTS,    // vertices loaded (G_VTX)
    // DC_PROF_LISTS: what each PVR list costs in triangles and on-screen pixels, and how much of
    // PT and TR could go to OP because its alpha is 1 everywhere (gfx_pvr.c prof_count_bucket).
    DCC_OP_TRIS,
    DCC_OP_PX,
    DCC_PT_TRIS,
    DCC_PT_PX,
    DCC_PT_OPQ_PX,   // PT whose alpha test can never fail
    DCC_TR_TRIS,
    DCC_TR_PX,
    DCC_TR_OPQ_TRIS, // TR whose blend gives the source colour: drawn opaque
    DCC_TR_OPQ_PX,
    DCC_HELD,        // 60 fps second draws skipped as identical to the first (pc_main.c)
    DCC_DROPPED,     // DC_FRAME_MODE_60_DROP: exact frames dropped after a late interpolated one
    DCC_COUNT
};

#ifdef DC_PROF
extern unsigned int gDcProfCount[DCC_COUNT];
#define DC_PROF_COUNT(c, n) (gDcProfCount[c] += (n))
void dc_prof_mark(int mark);
void dc_prof_frame_end(void);
void dc_prof_audio_add(unsigned int us);
// An asset read on the main thread (an AICAflow song or sound from /pc, ok = 0 if it is missing):
// logged per frame as DCPROF ASSET, and a late frame that read one is a "load" frame.
void dc_prof_asset_load(const char *name, unsigned int us, unsigned int bytes, int ok);
unsigned long long dc_prof_now_us(void);
void dc_prof_stop_audio(void);
void dc_prof_init(void);
#ifdef DC_PROF_LISTS
// One PT (kind 1) or TR (kind 2) batch of this frame: its texture's N64 address (0 untextured),
// on-screen pixels and triangles. Summed per scene and printed as DCPROF LISTSRC (largest first).
void dc_prof_list_src(int kind, unsigned int src, unsigned int px, unsigned int tris);
#endif
#define DC_PROF_MARK(m) dc_prof_mark(m)
#else
#define DC_PROF_MARK(m) ((void) 0)
#define dc_prof_asset_load(name, us, bytes, ok) ((void) 0)
#define DC_PROF_COUNT(c, n) ((void) 0)
#endif

#endif
