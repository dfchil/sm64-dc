#ifndef DC_VIDEO_OPTIONS_H
#define DC_VIDEO_OPTIONS_H
/* The video options a player can pick, as two independent settings (make DC_FSAA_MODE=n /
 * DC_FRAME_MODE=n set the defaults).
 *
 * FSAA (DC_FSAA builds; gDcFsaaMode, gfx_pvr.c). The PVR's FSAA mode changes between courses only
 * (it takes a PVR restart). */
#define DC_FSAA_MODE_OFF      0   /* never */
#define DC_FSAA_MODE_ON       1   /* always */
#define DC_FSAA_MODE_CURATED  2   /* always, except DC_FSAA_OFF_LEVELS (water and lava courses) */
#ifndef DC_FSAA_MODE
#define DC_FSAA_MODE DC_FSAA_MODE_CURATED
#endif
extern int gDcFsaaMode;

/* Frame rate (DC_60FPS builds; gDcFrameMode, pc_main.c). Each 30 Hz logic step is shown as an
 * interpolated frame and then the exact one. */
#define DC_FRAME_MODE_30      0   /* the exact frame only, for two vblanks */
#define DC_FRAME_MODE_60      1   /* both; a late frame delays all that follow (the game slows) */
#define DC_FRAME_MODE_60_DROP 2   /* both; when the interpolated frame is late, the exact one is
                                     dropped, so the step keeps its two vblanks and game time keeps up */
#ifndef DC_FRAME_MODE
#define DC_FRAME_MODE DC_FRAME_MODE_60_DROP
#endif
extern int gDcFrameMode;
#endif
