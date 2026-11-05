#ifndef DC_OVERLAY_H
#define DC_OVERLAY_H
/* make DC_OVERLAY=1: frame rate and missed frames on screen (dc_overlay.c). The debug menu
 * (DC_DEBUG_MENU, pc_main.c) builds it in as well, and turns it on and off. */
#ifndef DC_OVERLAY
#define DC_OVERLAY 0
#endif
#ifndef DC_DEBUG_MENU
#define DC_DEBUG_MENU 0
#endif
#define DC_OVERLAY_CODE (DC_OVERLAY || DC_DEBUG_MENU)
#if DC_OVERLAY_CODE
extern int gDcOverlayOn;            /* shown: from the start with DC_OVERLAY=1, else off */
#define DC_OVERLAY_SHOWN gDcOverlayOn
void dc_overlay_count_draw(void);   /* pc_main.c, after each drawn picture */
void dc_overlay_count_skip(void);   /* pc_main.c, an exact frame skipped after a late one */
void dc_overlay_draw(void);         /* gfx_pvr.c, with the TR list open */
void dc_overlay_pvr_reset(void);    /* gfx_pvr.c, after a PVR restart */
int dc_overlay_font_ready(void);    /* the quick font in video memory, for the debug menu too */
#else
#define DC_OVERLAY_SHOWN 0
#endif
#endif
