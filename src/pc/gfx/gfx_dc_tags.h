#ifndef GFX_DC_TAGS_H
#define GFX_DC_TAGS_H
/* Dreamcast-only display-list tags (gDPNoOpTag; gfx_retro_dc.c reads them, the N64 RDP ignores
 * G_NOOP). A COVER is solid opaque black over the whole frame outside a transition's hole
 * (screen_transition.c): drawn into the PVR's OP list above every world depth, it hides what lies
 * under it from the PT and TR lists too, so their fill is not paid behind black. */
#define DC_GFX_COVER_BEGIN 0x434F5642u   /* "COVB" */
#define DC_GFX_COVER_END   0x434F5645u   /* "COVE" */
/* The skybox (bin/segment2.c dl_skybox_begin .. dl_skybox_end): its G_RM_OPA_SURF triangles are
 * opaque and go to the OP list. Only there: goddard's Mario head and the screen borders use the same
 * render mode and need the TR path (the head's highlight blends there). */
#define DC_GFX_SKY_BEGIN 0x534B5942u     /* "SKYB" */
#define DC_GFX_SKY_END   0x534B5945u     /* "SKYE" */
/* DC_PROF_SKIP builds only: the blob shadows (bin/segment2.c dl_shadow_circle/square .. dl_shadow_end),
 * so their PVR cost can be priced by leaving them out (gfx_pvr.c SKIP_SHADOW). */
#define DC_GFX_SHADOW_BEGIN 0x53484442u  /* "SHDB" */
#define DC_GFX_SHADOW_END   0x53484445u  /* "SHDE" */
/* DC_RAIL_TR builds: BBH's chain-link fence (levels/bbh/areas/1/36) goes to the TR list instead of
 * PT. Measured 2026-10-24 (FSAA, BBH demo x5): PVR-late 110 -> 0, PVR avg 10.0 -> 9.1 ms, max
 * 20.2 -> 16.0 ms; drawn in PT it was the course's biggest fill (134k pixels a frame). */
#define DC_GFX_RAIL_TR_BEGIN 0x5241494Cu  /* "RAIL" */
#define DC_GFX_RAIL_TR_END   0x5241494Eu  /* "RAIN" */
#endif
