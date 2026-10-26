#include <PR/ultratypes.h>

#include "audio/external.h"
#include "engine/math_util.h"
#include "game/area.h"
#include "game/game_init.h"
#include "game/level_update.h"
#include "game/main.h"
#include "game/memory.h"
#include "game/print.h"
#include "game/save_file.h"
#include "game/sound_init.h"
#include "level_table.h"
#include "seq_ids.h"
#include "sm64.h"

#if defined(DC_PROF) && !defined(DC_PROF_TITLE)
// Profile runs cycle the attract demos as the workload: start each one after ~3 s, not ~27 s.
// (DC_PROF_TITLE keeps the normal wait, to profile the title screen's Mario head.)
#define PRESS_START_DEMO_TIMER 90
#else
#define PRESS_START_DEMO_TIMER 800
#endif

#define STUB_LEVEL(textname, _1, _2, _3, _4, _5, _6, _7, _8) textname,
#define DEFINE_LEVEL(textname, _1, _2, _3, _4, _5, _6, _7, _8, _9, _10) textname,

static char gLevelSelect_StageNamesText[64][16] = {
    #include "levels/level_defines.h"
};
#undef STUB_LEVEL
#undef DEFINE_LEVEL

static u16 gDemoCountdown = 0;

#ifdef DC_PROF_BENCH
// Benchmark tour (make DC_PROF_BENCH=1): every level, each entered at its start as an attract
// demo whose inputs game_init.c generates (run_demo_inputs): the camera turns round twice, close
// and zoomed out, while Mario stands still, so the PVR sees each level in every direction and
// nothing depends on a route. The demo index only loads a valid input list.
static const struct { s8 level; u8 demo; } sTour[] = {
    { LEVEL_BOB, 1 },   { LEVEL_WF, 1 },     { LEVEL_JRB, 1 },    { LEVEL_CCM, 1 },   { LEVEL_BBH, 1 },
    { LEVEL_HMC, 1 },   { LEVEL_LLL, 1 },    { LEVEL_SSL, 1 },    { LEVEL_DDD, 1 },   { LEVEL_SL, 1 },
    { LEVEL_WDW, 1 },   { LEVEL_TTM, 1 },    { LEVEL_THI, 1 },    { LEVEL_TTC, 1 },   { LEVEL_RR, 1 },
    { LEVEL_PSS, 1 },   { LEVEL_SA, 1 },     { LEVEL_WMOTR, 1 },  { LEVEL_TOTWC, 1 }, { LEVEL_COTMC, 1 },
    { LEVEL_VCUTM, 1 }, { LEVEL_BITDW, 1 },  { LEVEL_BITFS, 1 },  { LEVEL_BITS, 1 },
    { LEVEL_BOWSER_1, 1 }, { LEVEL_BOWSER_2, 1 }, { LEVEL_BOWSER_3, 1 },
    { LEVEL_CASTLE_GROUNDS, 1 }, { LEVEL_CASTLE, 1 }, { LEVEL_CASTLE_COURTYARD, 1 },
};
#define DC_PROF_TOUR_TABLE
#endif
#if defined(DC_PROF_TOUR) && !defined(DC_PROF_TOUR_TABLE)
// Profile tour (make DC_PROF_TOUR=1): the attract demos visit every main level in turn. A level
// with an attract demo of its own plays it, so Mario and the camera move as in a real attract
// run; the others play Whomp's Fortress's inputs (the shortest demo) in a level they were not
// recorded for, so the tour fits one profile run. Demo indices are the US table's order
// (assets/demo_data.json): BITDW, WF, CCM, BBH, JRB, HMC, PSS.
static const struct { s8 level; u8 demo; } sTour[] = {
    { LEVEL_BOB, 1 },   { LEVEL_WF, 1 },     { LEVEL_JRB, 4 },    { LEVEL_CCM, 2 },  { LEVEL_BBH, 3 },
    { LEVEL_HMC, 5 },   { LEVEL_LLL, 1 },    { LEVEL_SSL, 1 },    { LEVEL_DDD, 1 },  { LEVEL_SL, 1 },
    { LEVEL_WDW, 1 },   { LEVEL_TTM, 1 },    { LEVEL_THI, 1 },    { LEVEL_TTC, 1 },
    { LEVEL_CASTLE_GROUNDS, 1 }, { LEVEL_CASTLE, 1 }, { LEVEL_BITDW, 0 }, { LEVEL_BITFS, 1 },
    { LEVEL_BITS, 1 },  { LEVEL_BOWSER_1, 1 }, { LEVEL_PSS, 6 },
    { LEVEL_RR, 1 },
};
#endif
#ifdef DC_PROF_TOUR
#ifndef DC_PROF_TOUR_START
#define DC_PROF_TOUR_START 0
#endif
#ifndef DC_PROF_TOUR_ONLY
static u32 sTourPos = DC_PROF_TOUR_START;   // make DC_PROF_TOUR_START=n starts the tour at entry n
#endif
#endif
#ifndef VERSION_JP
static s16 D_U_801A7C34 = 1;
static s16 gameOverNotPlayed = 1;
#endif

// run the demo timer on the PRESS START screen.
// this function will return a non-0 timer once
// the demo starts, signaling to the subsystem that
// the demo needs to be ran.

// don't shift this function from being the first function in the segment.
// the level scripts assume this function is the first, so it cant be moved.
int run_press_start_demo_timer(s32 timer) {
    gCurrDemoInput = NULL;

    if (timer == 0) {
        if (!gPlayer1Controller->buttonDown && !gPlayer1Controller->stickMag) {
#ifdef DC_PROF_TOUR
            // A demo that ends by Mario's death (Wing Mario Over the Rainbow, Rainbow Ride with
            // inputs recorded elsewhere) does not reset the countdown, which then never equals
            // the limit again and the tour stops on the title screen.
            if ((++gDemoCountdown) >= PRESS_START_DEMO_TIMER) {
#else
            if ((++gDemoCountdown) == PRESS_START_DEMO_TIMER) {
#endif
                // start the demo. 800 frames has passed while
                // player is idle on PRESS START screen.
                D_U_801A7C34 = 1;

#ifdef DC_PROF_TOUR
#ifdef DC_PROF_TOUR_ONLY
                const u32 tourPos = DC_PROF_TOUR_ONLY;   // make DC_PROF_TOUR_ONLY=n: entry n every time
#else
                const u32 tourPos = sTourPos++ % ARRAY_COUNT(sTour);
#endif
                gDemoInputListID = sTour[tourPos].demo;
                // The same demo twice in a row: load_patchable_table would skip the DMA (same source
                // address as last time) although the level load reused the buffer, leaving Mario
                // without inputs. Force the reload.
                gDemo.currentAnimAddr = NULL;
#endif
                // start the Mario demo animation for the demo list.
                load_patchable_table(&gDemo, gDemoInputListID);

                // if the next demo sequence ID is the count limit, reset it back to
                // the first sequence.
                if (++gDemoInputListID == gDemo.animDmaTable->count) {
                    gDemoInputListID = 0;
                }

                // add 1 (+4) to the pointer to skip the demoID.
                gCurrDemoInput = ((struct DemoInput *) gDemo.targetAnim) + 1;
                timer = (s8)((struct DemoInput *) gDemo.targetAnim)->timer;
#ifdef DC_PROF_TOUR
                timer = sTour[tourPos].level;
#endif
                gCurrSaveFileNum = 1;
                gCurrActNum = 1;
            }
        } else { // activity was detected, so reset the demo countdown.
            gDemoCountdown = 0;
        }
    }
    return timer;
}

// input loop for the level select menu. updates the selected stage
// count if an input was received. signals the stage to be started
// or the level select to be exited if start or the quit combo is
// pressed.

s16 level_select_input_loop(void) {
    s32 stageChanged = FALSE;

    // perform the ID updates per each button press.
    if (gPlayer1Controller->buttonPressed & A_BUTTON) {
        ++gCurrLevelNum, stageChanged = TRUE;
    }
    if (gPlayer1Controller->buttonPressed & B_BUTTON) {
        --gCurrLevelNum, stageChanged = TRUE;
    }
    if (gPlayer1Controller->buttonPressed & U_JPAD) {
        --gCurrLevelNum, stageChanged = TRUE;
    }
    if (gPlayer1Controller->buttonPressed & D_JPAD) {
        ++gCurrLevelNum, stageChanged = TRUE;
    }
    if (gPlayer1Controller->buttonPressed & L_JPAD) {
        gCurrLevelNum -= 10, stageChanged = TRUE;
    }
    if (gPlayer1Controller->buttonPressed & R_JPAD) {
        gCurrLevelNum += 10, stageChanged = TRUE;
    }

    // if the stage was changed, play the sound for changing a stage.
    if (stageChanged) {
        play_sound(SOUND_GENERAL_LEVEL_SELECT_CHANGE, gDefaultSoundArgs);
    }

    // TODO: enum counts for the stage lists
    if (gCurrLevelNum > LEVEL_MAX) {
        gCurrLevelNum = LEVEL_MIN; // exceeded max. set to min.
    }

    if (gCurrLevelNum < LEVEL_MIN) {
        gCurrLevelNum = LEVEL_MAX; // exceeded min. set to max.
    }

    gCurrSaveFileNum = 4; // file 4 is used for level select tests
    gCurrActNum = 6;
    print_text_centered(160, 80, "SELECT STAGE");
    print_text_centered(160, 30, "PRESS START BUTTON");
    print_text_fmt_int(40, 60, "%2d", gCurrLevelNum);
    print_text(80, 60, gLevelSelect_StageNamesText[gCurrLevelNum - 1]); // print stage name

#define QUIT_LEVEL_SELECT_COMBO (Z_TRIG | START_BUTTON | L_CBUTTONS | R_CBUTTONS)

    // start being pressed signals the stage to be started. that is, unless...
    if (gPlayer1Controller->buttonPressed & START_BUTTON) {
        // ... the level select quit combo is being pressed, which uses START. If this
        // is the case, quit the menu instead.
        if (gPlayer1Controller->buttonDown == QUIT_LEVEL_SELECT_COMBO) {
            gDebugLevelSelect = 0;
            return -1;
        }
        play_sound(SOUND_MENU_STAR_SOUND, gDefaultSoundArgs);
        return gCurrLevelNum;
    }
    return 0;
}

int intro_default(void) {
    s32 sp1C = 0;

#ifndef VERSION_JP
    if (D_U_801A7C34 == 1) {
        if (gGlobalTimer < 0x81) {
            play_sound(SOUND_MARIO_HELLO, gDefaultSoundArgs);
        } else {
            play_sound(SOUND_MARIO_PRESS_START_TO_PLAY, gDefaultSoundArgs);
        }
        D_U_801A7C34 = 0;
    }
#endif
    print_intro_text();

    if (gPlayer1Controller->buttonPressed & START_BUTTON) {
#ifdef VERSION_JP
        play_sound(SOUND_MENU_STAR_SOUND, gDefaultSoundArgs);
        sp1C = 100 + gDebugLevelSelect;
#else
        play_sound(SOUND_MENU_STAR_SOUND, gDefaultSoundArgs);
        sp1C = 100 + gDebugLevelSelect;
        D_U_801A7C34 = 1;
#endif
    }
    return run_press_start_demo_timer(sp1C);
}

int intro_game_over(void) {
    s32 sp1C = 0;

#ifndef VERSION_JP
    if (gameOverNotPlayed == 1) {
        play_sound(SOUND_MARIO_GAME_OVER, gDefaultSoundArgs);
        gameOverNotPlayed = 0;
    }
#endif

    print_intro_text();

    if (gPlayer1Controller->buttonPressed & START_BUTTON) {
        play_sound(SOUND_MENU_STAR_SOUND, gDefaultSoundArgs);
        sp1C = 100 + gDebugLevelSelect;
#ifndef VERSION_JP
        gameOverNotPlayed = 1;
#endif
    }
    return run_press_start_demo_timer(sp1C);
}

int intro_play_its_a_me_mario(void) {
    set_background_music(0, SEQ_SOUND_PLAYER, 0);
    play_sound(SOUND_MENU_COIN_ITS_A_ME_MARIO, gDefaultSoundArgs);
    return 1;
}

s32 lvl_intro_update(s16 arg1, UNUSED s32 arg2) {
    s32 retVar = 0;

    switch (arg1) {
        case 0:
            retVar = intro_play_its_a_me_mario();
            break;
        case 1:
            retVar = intro_default();
            break;
        case 2:
            retVar = intro_game_over();
            break;
        case 3:
            retVar = level_select_input_loop();
            break;
    }
    return retVar;
}
