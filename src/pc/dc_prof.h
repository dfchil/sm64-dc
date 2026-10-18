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
    DCC_COUNT
};

#ifdef DC_PROF
extern unsigned int gDcProfCount[DCC_COUNT];
#define DC_PROF_COUNT(c, n) (gDcProfCount[c] += (n))
void dc_prof_mark(int mark);
void dc_prof_frame_end(void);
void dc_prof_audio_add(unsigned int us);
unsigned long long dc_prof_now_us(void);
void dc_prof_stop_audio(void);
void dc_prof_init(void);
#define DC_PROF_MARK(m) dc_prof_mark(m)
#else
#define DC_PROF_MARK(m) ((void) 0)
#define DC_PROF_COUNT(c, n) ((void) 0)
#endif

#endif
