/* make DC_OVERLAY=1: the frame rate and the frame losses of the current scene in the bottom left
 * corner, in enDjinn's quick font (third_party/endjinn). FPS counts the pictures drawn in the
 * last second. Since the level or area last changed: MISS counts presents that came after the
 * vblank they were paced for (a frame shown late), SKIP the exact second frames skipped on
 * purpose after a late first one (DC_FRAME_MODE_60_DROP). Each scene's totals also go to the
 * console when it ends. The text goes into the PVR's TR list after everything else, with depth
 * compare off, so it is drawn over the scene. Kept free of the game's headers: KOS and
 * libultra both define the CONT_* button names. */
#if DC_OVERLAY

#include <stdio.h>
#include <kos.h>
#define ENJ_INJECT_QFONT 1
#include <enDjinn/enj_defs.h>
#include <enDjinn/enj_font.h>
#include <enDjinn/enj_qfont.h>

#include "pc/dc_overlay.h"

extern volatile uint64_t vblticker;
extern unsigned int gfx_dc_vbl_late;
extern int16_t gCurrLevelNum, gCurrAreaIndex;
extern int gfx_pvr_fsaa_x(void);

static uint32_t sDraws, sFps, sSceneDraws, sSkips;
static uint64_t sSince;
static unsigned int sLateBase;
static int16_t sLevel = -1, sArea = -1;
static int sFontReady;
static pvr_sprite_hdr_t sHdr, sShadowHdr;

void dc_overlay_count_draw(void) {
    if (gCurrLevelNum != sLevel || gCurrAreaIndex != sArea) {
        if (sLevel >= 0) {
            printf("OVERLAY scene level=%d area=%d frames=%lu miss=%u skip=%lu\n", sLevel, sArea,
                   (unsigned long) sSceneDraws, gfx_dc_vbl_late - sLateBase, (unsigned long) sSkips);
        }
        sLevel = gCurrLevelNum;
        sArea = gCurrAreaIndex;
        sLateBase = gfx_dc_vbl_late;
        sSceneDraws = 0;
        sSkips = 0;
    }
    sDraws++;
    sSceneDraws++;
    if (vblticker - sSince >= 60) {
        sFps = (uint32_t) (sDraws * 60 / (vblticker - sSince));
        sDraws = 0;
        sSince = vblticker;
    }
}

void dc_overlay_count_skip(void) {
    sSkips++;
}

// A PVR restart (gfx_pvr.c, a per-course FSAA switch) drops the font's texture with the rest of
// video memory: upload it again on the next draw.
void dc_overlay_pvr_reset(void) {
    sFontReady = 0;
}

static int font_ready(void) {
    if (sFontReady) {
        return 1;
    }
    if (enj_qfont_init() != 0) {
        return 0;
    }
    enj_font_header_t *font = enj_qfont_get_header();
    pvr_sprite_cxt_t cxt;
    pvr_sprite_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB1555, 1 << font->log2width,
                       1 << font->log2height, enj_qfont_get_pvr_ptr(), PVR_FILTER_NONE);
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    pvr_sprite_compile(&sHdr, &cxt);
    sHdr.argb = 0xffffff40;
    sShadowHdr = sHdr;
    sShadowHdr.argb = 0xff202020;
    sFontReady = 1;
    return 1;
}

// One line with a drop shadow: the same text in a dark colour one pixel right and down, behind it
// (the TR list sorts by depth, so the shadow gets the lower z).
static void line_out(const char *text, int x, int y) {
    enj_font_zvalue_set(999.0f);
    enj_font_string_render(text, enj_qfont_get_header(), (int16_t) (x + 1), (int16_t) (y + 1),
                           &sShadowHdr);
    enj_font_zvalue_set(1000.0f);
    enj_font_string_render(text, enj_qfont_get_header(), (int16_t) x, (int16_t) y, &sHdr);
}

// Called by gfx_pvr.c with the TR list open.
void dc_overlay_draw(void) {
    if (!font_ready()) {
        return;
    }
    char line[32];
    // enDjinn scales x by its build-time FSAA setting (ENJ_XSCALE); divide by it and multiply
    // by the PVR's current one, so the text sits right with FSAA on or off.
    const int x = 24 * gfx_pvr_fsaa_x() / ENJ_XSCALE;
    snprintf(line, sizeof(line), "FPS %u", (unsigned) sFps);
    line_out(line, x, 404);
    snprintf(line, sizeof(line), "MISS %u", gfx_dc_vbl_late - sLateBase);
    line_out(line, x, 424);
    snprintf(line, sizeof(line), "SKIP %lu", (unsigned long) sSkips);
    line_out(line, x, 444);
}

#endif
