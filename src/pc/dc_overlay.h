#ifndef DC_OVERLAY_H
#define DC_OVERLAY_H
/* make DC_OVERLAY=1: frame rate and missed frames on screen (dc_overlay.c). */
#ifndef DC_OVERLAY
#define DC_OVERLAY 0
#endif
#if DC_OVERLAY
void dc_overlay_count_draw(void);   /* pc_main.c, after each drawn picture */
void dc_overlay_count_skip(void);   /* pc_main.c, an exact frame skipped after a late one */
void dc_overlay_draw(void);         /* gfx_pvr.c, with the TR list open */
void dc_overlay_pvr_reset(void);    /* gfx_pvr.c, after a PVR restart */
#endif
#endif
