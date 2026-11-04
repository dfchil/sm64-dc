# Host build of the Dreamcast port on enDjinn's pc-endjinn backend (SDL2 + Vulkan/MoltenVK, macOS
# or Linux): the same game sources, compiled with clang against pc-endjinn's KOS shim headers.
# Host-only code lives in src/pc/pc_endjinn/ (TARGET_PC_ENDJINN); the Dreamcast build never sees it.
#
#   gmake TARGET_DC=1 DC_60FPS=1 BUILD_DIR_BASE=build/dc -j8     # once: generated sources and assets
#   gmake -f pc_endjinn.mk -j8                                    # -> build/pc-endjinn/sm64
#   SM64_TAS=movie.m64 SM64_HEADLESS=1 build/pc-endjinn/sm64       # TAS playback, no window, uncapped
#   build/pc-endjinn/sm64                                          # windowed, keyboard/controller
#
# Options (same meaning as in the Dreamcast Makefile; delete build/pc-endjinn when switching):
#   DC_TRIG_TABLE=1 (default) game sine table for sins/coss, as on the N64
#   DC_EXACT_LOGIC=0 (default) src/game and src/engine with -ffast-math -ffp-contract=fast like the
#     Dreamcast build; 1 = -fno-fast-math -ffp-contract=off. (clang has no -fsingle-precision-constant.)
#   DC_GEN=build/dc/us_dc   the Dreamcast build's generated tree (headers, textures, level data)
#   SM64_CC=<compiler>   for the game's C sources (default $(CC)). The N64 reference for a TAS
#     (tools/tasref) wants GCC, e.g. SM64_CC="gcc-16 -isysroot $$(xcrun --show-sdk-path)" with
#     DC_EXACT_LOGIC=1: a float converted to s16/u16 out of its range is undefined in C, and the
#     N64 and GCC (here and for the SH4) go through an int and keep the low 16 bits, which TAS
#     speed tricks rely on; clang on arm64 keeps all 32 bits.

ENJDIR := $(abspath third_party/endjinn)/
DC_GEN ?= build/dc/us_dc
HOST_BUILD ?= build/pc-endjinn
HOST_EXE ?= $(HOST_BUILD)/sm64
DC_TRIG_TABLE ?= 1
DC_EXACT_LOGIC ?= 0
HOST_OPT ?= -O2
SM64_CC ?= $(CC)
SHZ_INCLUDE ?= $(if $(KOS_BASE),$(KOS_BASE),/opt/toolchains/dc/kos)/addons/include/sh4zam
# sh4zam's out-of-line portable C routines (the XMTRX emulation), built for the host from its source.
SHZ_SRC ?= $(HOME)/projects/dreamcast/sh4zam-v0.10.0

ENJ_HOST_BUILD_DIR := $(HOST_BUILD)
.DEFAULT_GOAL := all
include $(ENJDIR)backends/pc-endjinn/backend.mk

ifeq ($(wildcard $(DC_GEN)/include/level_headers.h),)
$(error $(DC_GEN) has no generated sources: run the Dreamcast build first (gmake TARGET_DC=1 DC_60FPS=1 BUILD_DIR_BASE=build/dc))
endif

CC := clang
CXX := clang++

# ---- sources: the Dreamcast build's list (Makefile) with the KOS-only platform files swapped ----
SRC_DIRS := src src/engine src/game src/audio src/menu src/buffers actors levels bin bin/us data assets \
            src/pc src/pc/gfx src/pc/audio src/pc/controller
LEVEL_C_FILES := $(wildcard levels/*/leveldata.c) $(wildcard levels/*/script.c) $(wildcard levels/*/geo.c)
C_FILES := $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.c)) $(LEVEL_C_FILES)
C_FILES := $(filter-out src/game/main.c,$(C_FILES))
# Replaced on the host: main loop, vblank pacing, profiler/overlay, PVR store-queue submission,
# KOS/AICA audio (audio_null is used), AICA voice driver.
C_FILES := $(filter-out src/pc/pc_main.c src/pc/dc_prof.c src/pc/dc_overlay.c src/pc/mixer.c \
           src/pc/gfx/gfx_dc.c src/pc/gfx/gfx_pvr.c src/pc/gfx/ram.c src/pc/audio/audio_dc.c src/pc/audio/dc_audio_kos.c \
           src/pc/audio/sm64_aicaflow.c src/audio/aica_synth.c,$(C_FILES))
C_FILES += $(wildcard src/pc/pc_endjinn/*.c)
ULTRA_C_FILES := $(addprefix lib/src/,alBnkfNew.c guLookAtRef.c guMtxF2L.c guNormalize.c guOrthoF.c \
                 guPerspectiveF.c guRotateF.c guScaleF.c guTranslateF.c)
GODDARD_C_FILES := $(wildcard src/goddard/*.c) $(wildcard src/goddard/dynlists/*.c)
GEN_C_FILES := $(DC_GEN)/assets/mario_anim_data.c $(DC_GEN)/assets/demo_data.c \
  $(addprefix $(DC_GEN)/bin/,$(addsuffix _skybox.c,$(notdir $(basename $(wildcard textures/skyboxes/*.png)))))
SOUND_C_FILES := sound/sound_data.c

SRC_OBJS := $(patsubst %.c,$(HOST_BUILD)/%.o,$(C_FILES) $(ULTRA_C_FILES) $(GODDARD_C_FILES) $(SOUND_C_FILES))
GEN_OBJS := $(patsubst $(DC_GEN)/%.c,$(HOST_BUILD)/gen/%.o,$(GEN_C_FILES))
HOST_COMMON_OBJS := $(addprefix $(HOST_BUILD)/enDjinn/host-common/,$(notdir $(ENJ_HOST_COMMON_SRCS:.cpp=.o)))
HOST_BACKEND_OBJS := $(addprefix $(HOST_BUILD)/enDjinn/pc-endjinn/,$(notdir $(PC_ENDJINN_PLATFORM_SRCS:.cpp=.o)))
SHZ_OBJS := $(addprefix $(HOST_BUILD)/sh4zam/,shz_complex_sw.o shz_matrix_sw.o shz_xmtrx_sw.o)
ALL_OBJS := $(SRC_OBJS) $(GEN_OBJS) $(HOST_COMMON_OBJS) $(HOST_BACKEND_OBJS) $(SHZ_OBJS)

# ---- sound data: assemble_sound.py lays out pointers at the target's width, so the 32-bit
# Dreamcast banks and sequence tables are rebuilt here for 64 bits (same inputs: the DC build's
# compressed samples and assembled sequences). ----
SND_DIR := $(HOST_BUILD)/snd/sound
SND_BITS := --endian little --bitwidth 64
SOUND_SEQUENCE_FILES := $(wildcard sound/sequences/us/*.m64) $(wildcard sound/sequences/*.m64) \
  $(patsubst %.s,$(DC_GEN)/%.m64,$(wildcard sound/sequences/us/*.s) $(wildcard sound/sequences/*.s))
$(SND_DIR)/sound_data.ctl: $(wildcard sound/sound_banks/*.json)
	@mkdir -p $(dir $@)
	python3 tools/assemble_sound.py $(DC_GEN)/sound/samples/ sound/sound_banks/ $@ $(SND_DIR)/sound_data.tbl -DVERSION_US $(SND_BITS)
$(SND_DIR)/sound_data.tbl: $(SND_DIR)/sound_data.ctl
	@true
$(SND_DIR)/sequences.bin: $(wildcard sound/sound_banks/*.json) sound/sequences.json $(SOUND_SEQUENCE_FILES)
	@mkdir -p $(dir $@)
	python3 tools/assemble_sound.py --sequences $@ $(SND_DIR)/bank_sets sound/sound_banks/ sound/sequences.json $(SOUND_SEQUENCE_FILES) -DVERSION_US $(SND_BITS)
$(SND_DIR)/bank_sets: $(SND_DIR)/sequences.bin
	@true
$(SND_DIR)/%.inc.c: $(SND_DIR)/%
	hexdump -v -e '1/1 "0x%X,"' $< > $@
	echo >> $@
$(HOST_BUILD)/sound/sound_data.o: $(addprefix $(SND_DIR)/,sound_data.ctl.inc.c sound_data.tbl.inc.c sequences.bin.inc.c bank_sets.inc.c)
$(HOST_BUILD)/sound/sound_data.o: FIRST_INC := -I$(HOST_BUILD)/snd

# ---- flags ----
# The Dreamcast build's defines (DC_60FPS default build), plus TARGET_PC_ENDJINN. TARGET_DC stays
# defined: the game code takes the Dreamcast port's paths (f64 = float, DC display-list tags, ...).
SM64_DEFINES := -D_LANGUAGE_C -DVERSION_US -DNON_MATCHING -DAVOID_UB -DF3DEX_GBI_2E -DTARGET_DC \
  -DTARGET_PC_ENDJINN -DNDEBUG -DDC_VBL_WAIT=1 -DDC_60FPS -DDC_OPAQUE_LAVA=0 -DDC_OVERLAY=0 \
  -DDC_HOLD_IDENTICAL=1 -DDC_TRIG_TABLE=$(DC_TRIG_TABLE) -DDC_SHZ_FSCA=1 -DDC_RAIL_TR=1 -DDC_PT_CHUNK=64 \
  -DKOS_GENWAIT_3ARG -DGFX_BACKEND_PVR=1 -DDC_COVERS=1 -DNO_SEGMENTED_MEMORY -DENABLE_OPENGL -DDC_TAS
# src/pc/pc_endjinn/compat first: it wraps pc-endjinn's <kos.h> with the few KOS calls SM64 needs.
HOST_SHZ_DIR := $(HOST_BUILD)/shzinc
SM64_INCLUDES := -Isrc/pc/pc_endjinn/compat -Iinclude -I$(DC_GEN) -I$(DC_GEN)/include -Isrc -I. \
  -I$(HOST_SHZ_DIR) $(PC_ENDJINN_CPPFLAGS)
SM64_CFLAGS := -std=gnu11 -D_FORTIFY_SOURCE=0 $(HOST_OPT) -g -fno-strict-aliasing -fwrapv -fsigned-char -w
ifeq ($(DC_EXACT_LOGIC),1)
LOGIC_CFLAGS := -fno-fast-math -ffp-contract=off -DDC_EXACT_MATH=1
else
LOGIC_CFLAGS := -ffast-math -ffp-contract=fast
endif
ENJ_CXXFLAGS := -std=c++20 $(HOST_OPT) -g -w -I$(ENJDIR)include $(PC_ENDJINN_CPPFLAGS)

.PHONY: all clean
all: $(HOST_EXE)

# sh4zam's headers only (its portable C backend), without the rest of the KOS addons include tree.
$(HOST_SHZ_DIR)/sh4zam:
	@mkdir -p $(HOST_SHZ_DIR)
	ln -sfn $(SHZ_INCLUDE) $@

$(HOST_BUILD)/src/game/%.o $(HOST_BUILD)/src/engine/%.o: SM64_CFLAGS += $(LOGIC_CFLAGS)

$(HOST_BUILD)/%.o: %.c | $(HOST_SHZ_DIR)/sh4zam
	@mkdir -p $(dir $@)
	$(SM64_CC) $(SM64_CFLAGS) $(SM64_DEFINES) $(FIRST_INC) $(SM64_INCLUDES) -MMD -MP -c $< -o $@

$(HOST_BUILD)/gen/%.o: $(DC_GEN)/%.c | $(HOST_SHZ_DIR)/sh4zam
	@mkdir -p $(dir $@)
	$(SM64_CC) $(SM64_CFLAGS) $(SM64_DEFINES) $(SM64_INCLUDES) -MMD -MP -c $< -o $@

$(HOST_BUILD)/sh4zam/%.o: $(SHZ_SRC)/source/sw/%.c | $(HOST_SHZ_DIR)/sh4zam
	@mkdir -p $(dir $@)
	$(CC) -std=gnu11 $(HOST_OPT) -w -DSHZ_BACKEND=SHZ_SW -DSHZ_TLS_MODEL=SHZ_TLS_DISABLED -I$(HOST_SHZ_DIR) -MMD -MP -c $< -o $@

$(HOST_BUILD)/enDjinn/host-common/%.o: $(ENJ_HOST_COMMON_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(ENJ_CXXFLAGS) -MMD -MP -c $< -o $@

$(HOST_BUILD)/enDjinn/pc-endjinn/%.o: $(PC_ENDJINN_BACKEND_DIR)%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(ENJ_CXXFLAGS) -MMD -MP -c $< -o $@

# Display lists are translated where they stand, the program's constant ones too
# (src/pc/gfx/native/nat_exec.inc.c): on macOS __TEXT's constants go to __DATA, writable.
ifeq ($(shell uname -s),Darwin)
PC_ENDJINN_LDFLAGS += -Wl,-rename_section,__TEXT,__const,__DATA,__const
endif

$(HOST_EXE): $(ALL_OBJS) $(ENJ_HOST_EXTRA_DEPS)
	$(CXX) -o $@ $(ALL_OBJS) $(PC_ENDJINN_LDFLAGS)

clean:
	rm -rf $(HOST_BUILD)

-include $(ALL_OBJS:.o=.d)
