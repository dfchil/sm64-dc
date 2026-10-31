#ifndef TAS_SLICE_H
#define TAS_SLICE_H

#include <PR/ultratypes.h>

void tas_slice_setup(const char *captureFile, const char *sliceFile, const char *playlist);
s32 tas_slice_first_level(s32 levelNum);
void tas_slice_warp(s16 *destLevel, s16 *destArea, s16 *destWarpNode, s32 *arg);
void tas_slice_init_level(void);

#endif
