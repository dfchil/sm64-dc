// pc-endjinn host build only (pc_endjinn.mk): symbols the Dreamcast build gets from files the host
// build leaves out, or never needs because LTO and --gc-sections drop their only callers.
#include <stdlib.h>
#include <string.h>
#include <PR/ultratypes.h>
#include "pc/mixer.h"

// src/pc/audio/dc_audio_kos.c
void n64_memcpy(void *dst, const void *src, size_t size) {
    memcpy(dst, src, size);
}

// The AICA voice driver (src/audio/aica_synth.c) has no host equivalent: the sequences still run,
// nothing sounds.
void AicaSynth_Init(void) {
}
void AicaSynth_Update(void) {
}
void AicaSynth_RefreshActive(void) {
}

// src/pc/mixer.c (SH4 asm): the RSP audio microcode, which synthesis.c only calls without
// TARGET_DC. The host build keeps TARGET_DC's path, so these are never reached.
void aClearBufferImpl(uint16_t addr, int nbytes) { (void) addr; (void) nbytes; abort(); }
void aLoadBufferImpl(const void *source_addr) { (void) source_addr; abort(); }
void aSaveBufferImpl(int16_t *dest_addr) { (void) dest_addr; abort(); }
void aLoadADPCMImpl(int n, const int16_t *book) { (void) n; (void) book; abort(); }
void aSetBufferImpl(uint8_t flags, uint16_t in, uint16_t out, uint16_t nbytes) {
    (void) flags; (void) in; (void) out; (void) nbytes; abort();
}
void aSetVolumeImpl(uint8_t flags, int16_t v, int16_t t, int16_t r) { (void) flags; (void) v; (void) t; (void) r; abort(); }
void aDMEMMoveImpl(uint16_t in_addr, uint16_t out_addr, int nbytes) { (void) in_addr; (void) out_addr; (void) nbytes; abort(); }
void aSetLoopImpl(ADPCM_STATE *s) { (void) s; abort(); }
void aADPCMdecImpl(uint8_t flags, ADPCM_STATE state) { (void) flags; (void) state; abort(); }
void aResampleImpl(uint8_t flags, uint16_t pitch, RESAMPLE_STATE state) { (void) flags; (void) pitch; (void) state; abort(); }
void aEnvMixerImpl(uint8_t flags, ENVMIX_STATE state) { (void) flags; (void) state; abort(); }
void aMixImpl(int16_t gain, uint16_t in_addr, uint16_t out_addr) { (void) gain; (void) in_addr; (void) out_addr; abort(); }
void aSaveBuffer2Impl(uint16_t source_addr, int16_t *dest_addr, uint16_t nbytes) {
    (void) source_addr; (void) dest_addr; (void) nbytes; abort();
}

// src/goddard/renderer.c compiles these out (#if 0); draw_objects.c's callers are unused.
void gd_draw_rect(f32 ulx, f32 uly, f32 lrx, f32 lry) { (void) ulx; (void) uly; (void) lrx; (void) lry; }
void gd_draw_border_rect(f32 ulx, f32 uly, f32 lrx, f32 lry) { (void) ulx; (void) uly; (void) lrx; (void) lry; }
