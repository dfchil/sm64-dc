#ifndef SM64_AICAFLOW_H
#define SM64_AICAFLOW_H
/* make DC_AICAFLOW=1: the game's music and sounds play as AICAflow assets
 * (src/pc/audio/sm64_aicaflow.c) instead of through the N64 sequencer and the
 * AICA voice driver. Assets come from tools/aica/sm64_afx.py. */
#include <PR/ultratypes.h>

int sm64_afx_init(void);
void sm64_afx_play_sequence(u8 player, u8 seqId, u16 fadeTimer);
void sm64_afx_fade_out(u8 player, u16 fadeTimer);
void sm64_afx_set_volume(u8 player, f32 scale, u16 fadeTimer);
void sm64_afx_play_sound(s32 soundBits, f32 *pos);
void sm64_afx_tick(void);
/* level_update.c init_level: read the level's scene pack (and the core pack, once) before the
 * level starts, so its songs and sounds do not load from /pc mid-scene. */
void sm64_afx_scene_prepare(s16 level);
/* external.c sound_reset: the session preset's reverb (N64 ring length in 32 kHz samples, and
 * feedback, Q15) becomes the scene's DSP program. */
void sm64_afx_reverb(u16 windowSize, u16 gain);
/* external.c: a sound's volume and pan from its source position, as the game computes them. */
void sm64_sound_dynamics(u32 soundBits, f32 *pos, f32 *volume, f32 *pan);
/* external.c: a sound's reverb send (0..127) from its script's value and its volume. */
u8 sm64_sound_reverb(u32 soundBits, f32 volume, u8 scriptReverb);
/* external.c: sm64_afx_reverb with an audio session preset's values. */
void sm64_sound_reverb_preset(u8 presetId);
#endif
