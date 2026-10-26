// gfx_pvr.c — raw-KOS-PVR backend for the SM64 DC renderer (ported from MK64).
//
// Drop-in replacement for the GLdc backend (gfx_gldc.c): implements the same
// struct GfxRenderingAPI, but submits geometry straight to the PVR via the
// direct-render (store-queue) path instead of going through OpenGL. The
// gfx_retro_dc.c front-end (command interpreter, combiner, SW clip/cull/scissor)
// is shared; only this backend differs. SM64's rapi is the SINGLE-TEXTURE variant
// (shader_get_info returns the used-texture bool; no tile params), so this backend
// is adapted accordingly vs MK64's two-tile version.

#include <PR/gbi.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
//#include <stdbool.h>
#undef bool
#define bool uint8_t
#include <assert.h>
#include <kos.h>
#include <dc/video.h>

#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "macros.h"
#include "gl_fast_vert.h"   // dc_fast_t (the front-end's per-vertex emit struct)
#include "pc/dc_prof.h"
#include "sh4zam_port.h"

extern int cur_frame_persp;
extern int prev_frame_had_persp;
extern int has_done_3d;

// The DR submit path relies on these: each vertex is copied dc_fast_t -> pvr_vertex_t
// (identical 32-byte layout), and the poly header is written into a single 32-byte
// store-queue slot. If a KOS revision changes either size, fail at build time rather
// than corrupting the SQ at runtime.
_Static_assert(sizeof(dc_fast_t) == sizeof(pvr_vertex_t), "dc_fast_t must match pvr_vertex_t layout");
_Static_assert(sizeof(pvr_poly_hdr_t) == 32, "pvr_poly_hdr_t must be one 32-byte SQ slot");

// DC_FSAA=1: the PVR renders twice as wide and the scaler filters down on output, so every x the
// front-end bakes is in a (DC_FB_W << 1)-wide space (gfx_retro_dc.c DC_FSAA_X).
#ifndef DC_FSAA
#define DC_FSAA 0
#endif
#if LOWRES
#define DC_RENDER_W_1X 320
#else
#define DC_RENDER_W_1X 640
#endif
// Current render width (DC_FB_W << the current FSAA mode): FSAA can be switched off per course.
#if DC_FSAA
static int sRenderW = DC_RENDER_W_1X << DC_FSAA;
#define DC_RENDER_W sRenderW
#else
#define DC_RENDER_W DC_RENDER_W_1X
#endif
// DC_FSAA builds draw these courses without FSAA (comma-separated level numbers): their water
// and lava fill the TR list over much of the screen, which FSAA makes twice as expensive.
#include "pc/dc_video_options.h"
#include "pc/dc_overlay.h"
#ifndef DC_FSAA_OFF_LEVELS
// From the benchmark tour (make DC_PROF_BENCH=1, FSAA on, 60 fps strict, 2026-10-26): the levels
// under 98% on time. TTC 87-89%, DDD 93%, Bowser 1 94%, JRB 96%, WDW 96%, WF 97-98%; every other
// level 98% or better (LLL 100% since its lava is opaque).
#define DC_FSAA_OFF_LEVELS 14, 23, 30, 12, 11, 24   /* TTC, DDD, BOWSER_1, JRB, WDW, WF */
#endif
static void pvr_post_init(void);
#if DC_FSAA
static void gfx_pvr_request_fsaa_for_level(int level);
extern s16 gCurrLevelNum;
#endif

// DC_PROF_SKIP (profile builds): leave one part of the frame out of the PVR lists, so its PVR
// cost can be read by subtraction. The CPU still translates everything; only submission stops.
//   1 the PT list          2 the TR list            3 every 2D quad (all lists)
//   4 the 2D TR quads      5 the 3D TR batches      6 the far-pinned (depth-off) OP geometry: the sky
//   7 all 3D OP geometry   8 full-screen 2D TR quads (fades, covers)   9 the 2D OP quads
//  10 the blob shadows (TR/PT batches between the DC_GFX_SHADOW tags of bin/segment2.c)
// gDcProfSkip holds this frame's code (dc_prof.c cycles it under DC_PROF_SKIP=999).
#ifdef DC_PROF_SKIP
extern int gDcProfSkip;
enum { SKIP_PT = 1, SKIP_TR, SKIP_2D, SKIP_2D_TR, SKIP_3D_TR, SKIP_SKY, SKIP_3D_OP, SKIP_FULL_2D_TR, SKIP_2D_OP,
       SKIP_SHADOW };
#define PROF_SKIP(code) (gDcProfSkip == (code))
#else
#define PROF_SKIP(code) 0
#endif

// ---------------------------------------------------------------------------
// Shader bookkeeping — The front-end treats ShaderProgram opaquely
// (only via shader_get_info), so this layout is
// private to the backend. "Shaders" here will resolve to a small set of
// pvr_poly_cxt configs in P3; for now we only need the CC feature flags.
// ---------------------------------------------------------------------------
enum MixType {
    SH_MT_NONE,
    SH_MT_TEXTURE,
    SH_MT_COLOR,
    SH_MT_TEXTURE_TEXTURE,
    SH_MT_TEXTURE_COLOR,
    SH_MT_COLOR_COLOR,
};

struct ShaderProgram {
    uint8_t enabled;
    uint32_t shader_id;
    struct CCFeatures cc;
    enum MixType mix;
    bool texture_used;   // SM64 is single-texture
    int num_inputs;
};

static struct ShaderProgram shader_program_pool[64];
static uint8_t shader_program_pool_size = 0;
static struct ShaderProgram *cur_shader = NULL;

// ---------------------------------------------------------------------------
// PVR state
// ---------------------------------------------------------------------------
static pvr_dr_state_t sDrState;
static int sCurrentList = PVR_LIST_OP_POLY;

// Per-primitive depth state, driven by the front-end (set_depth_test/mask/zmode_decal)
// straight from the RDP other_mode bits — NOT magic shader ids. PVR bakes depth state
// into the compiled poly header, so when any of these change we mark the header dirty
// and lazily recompile+submit a fresh one before the next vertices. This is what makes
// e.g. the skybox (near geometry drawn with Z_UPD off) test-but-not-write, so it no
// longer buries the track. (P3 will cache headers + fold in blend/texture state.)
static uint8_t sDepthTest  = 1;   // PVR_DEPTHCMP_GEQUAL vs ALWAYS
static uint8_t sDepthWrite = 1;   // PVR_DEPTHWRITE_ENABLE vs DISABLE
// (zmode decal is NOT header state — handled as a baked-z nudge in the front-end; see
//  gfx_pvr_set_zmode_decal + PVR_DECAL_ZBIAS in gfx_retro_dc.c)
static uint8_t sTexEnv     = PVR_TXRENV_MODULATE;  // texel<->vtx combine, derived from N64 combiner
static uint8_t sFogEnabled = 0;   // PVR_FOG_VERTEX vs PVR_FOG_DISABLE (G_FOG geometry mode)

// --- Texture table ---------------------------------------------------------
// The front-end hands upload_texture data ALREADY in PVR 16-bit format (ARGB1555
// or ARGB4444 — the "rgba32"/GL_BGRA naming is a leftover lie from the PC port),
// so there is no pixel conversion: we POT-PAD (never resample — see memory), copy
// into VRAM, and the front-end's get_*_scale UV correction addresses the used
// sub-region. Ids are 1-based table indices; 0 == none.
#define PVR_TEX_MAX 1024
struct PvrTex {
    pvr_ptr_t addr;        // VRAM, NULL if unallocated
    uint32_t  alloc_size;  // bytes currently allocated (reused if a re-upload fits)
    uint16_t  w, h;        // padded POT dims
    int       fmt;         // PVR_TXRFMT_ARGB1555/4444 | NONTWIDDLED
    uint8_t   filter;      // 0 = point, 1 = bilinear
    uint8_t   clampu, clampv;   // G_TX_CLAMP -> PVR_UVCLAMP
    uint8_t   flipu, flipv;     // G_TX_MIRROR -> PVR_UVFLIP (mirror-repeat)
    float     u_scale, v_scale; // real/padded, consumed by gfx_pvr_get_*_scale
#ifdef DC_PROF_LISTS
    uint8_t   opaque;      // every texel's alpha is 1
    uint32_t  src;         // the N64 texture address it was imported from (gfx_pvr_prof_tex_src)
#endif
};
static struct PvrTex sTextures[PVR_TEX_MAX];
// SIX base poly headers — {colour, textured} x {OP, PT, TR} — each compiled ONCE by the official
// pvr_poly_cxt+pvr_poly_compile, so every PER-LIST field (list_type/alpha/txralpha/blend defaults)
// is correct. Tiny + cache-friendly (192 bytes; a per-TEXTURE header array would be ~64KB and
// thrash the SH4 operand cache). pvr_compile_header copies the matching base, then bit-patches the
// texture fields (addr/dims/fmt/filter/clamp/flip/env — OoT-style) + per-draw state (depth/fog/
// TR-blend). No pvr_poly_compile per draw.
static pvr_poly_hdr_t __attribute__((aligned(32))) sBaseHdr[2][3];   // [textured][PVR_KIND_*]
static uint8_t sBaseHdrValid[2][3];
static uint32_t sTexCount = 0;   // high-water id allocator
static uint32_t sBoundTex = 0;   // currently bound texture (SM64 = single texture)
static uint32_t sCurBound = 0;   // last select_texture id == upload target

// --- OP live + PT/TR deferred -----------------------------------------------
// Only ONE list can be open on the DR path at a time, so the OP (opaque) list stays
// open and LIVE the whole frame — geometry the front-end can GUARANTEE is fully opaque
// (pvr_submit_op) streams straight out as it arrives. The other two kinds are buffered
// into per-kind buckets and replayed at end_frame, each into its own list opened once:
//   * PT (sPunch): alpha-TEST cutouts (hard-edged fences/foliage/glyphs). Opaque pixels,
//     depth-tested AND depth-written, alpha-tested by the hardware. Flushed FIRST.
//   * TR (sTR): real alpha-BLEND surfaces. Depth-tested, autosorted per-pixel (pvr_init),
//     composited over OP+PT. Flushed SECOND, so it blends over the cutouts.
// The front-end classifies each primitive (gfx_pvr_set_blend -> sListKind) into OP/PT/TR.
#define TR_MAX_VERTS    8192
// Bowser fights once overflowed this (PVR_DROP batch 2): the front-end re-dirtied the poly header
// per textured triangle, so same-texture translucent tris (coins/flames) fragmented to ~1 tri per
// batch -> ~580 batches. That was fixed at the root in gfx_pvr_set_sampler_parameters (dirty only
// on real sampler-state change); the same scene now measures ~73 batches, so 512 keeps ~7x margin.
#define TR_MAX_BATCH     512

typedef struct {
    pvr_poly_hdr_t __attribute__((aligned(32))) hdr;
    uint32_t start, count;
#ifdef DC_PROF_SKIP
    uint8_t is_2d;   // a batch of 2D quads (never shares a batch with 3D under DC_PROF_SKIP)
    uint8_t is_shadow;   // a batch of blob-shadow triangles (gfx_pvr_prof_shadow)
#endif
#ifdef DC_PROF_LISTS
    // Where the batch's alpha comes from, for prof_count_bucket.
    uint8_t alpha_vtx;   // the vertex alpha reaches the pixel
    uint8_t alpha_tex;   // the texel alpha reaches the pixel, and the texture has alpha < 1
    uint8_t blend_src;   // alpha 1 gives the source colour (SRCALPHA/INVSRCALPHA or ONE/ZERO)
    uint32_t src;        // the bound texture's N64 address, 0 untextured (dc_prof_list_src)
#endif
} PvrBatch;
typedef struct {
    pvr_vertex_t *verts; PvrBatch *batch;
    uint32_t nverts, max_verts;
    int nbatch, max_batch, cur;
    uint8_t dirty;   // state changed since this bucket's current batch was compiled
} PvrBucket;

static pvr_vertex_t sTrVerts[TR_MAX_VERTS] __attribute__((aligned(32)));
static PvrBatch     sTrBatch[TR_MAX_BATCH];
static PvrBucket sTR = { sTrVerts, sTrBatch, 0, TR_MAX_VERTS, 0, TR_MAX_BATCH, -1, 1 };

// Deferred PUNCH-THROUGH bucket: alpha-test cutouts (hard-edged, opaque pixels, write
// depth). Flushed at end_frame BEFORE TR.
static pvr_vertex_t sPunchVerts[TR_MAX_VERTS] __attribute__((aligned(32)));
static PvrBatch     sPunchBatch[TR_MAX_BATCH];
static PvrBucket sPunch = { sPunchVerts, sPunchBatch, 0, TR_MAX_VERTS, 0, TR_MAX_BATCH, -1, 1 };

// Routing for the current primitive: 0 = OP (live), 1 = PT (sPunch bucket), 2 = TR (sTR).
#define PVR_KIND_OP 0
#define PVR_KIND_PT 1
#define PVR_KIND_TR 2
static uint8_t sListKind = PVR_KIND_OP;
static uint8_t sOpDirty = 1;   // live OP header needs re-emit (depth/texture changed)

// TR blend factors, derived by the front-end from the N64 blender and pushed via
// gfx_pvr_set_blend_factors. Default = standard alpha-over (SRC_ALPHA / INV_SRC_ALPHA).
static int sBlendSrc = PVR_BLEND_SRCALPHA;
static int sBlendDst = PVR_BLEND_INVSRCALPHA;

// Capped, greppable overflow logging — never drop silently.
static int sDropV = 0, sDropB = 0;

// A depth/texture/shader state change affects the live OP header and the next PT and TR
// batches. (gfx_pvr_set_blend only routes OP/PT/TR, so it does NOT dirty headers.)
static void pvr_mark_dirty(void) { sOpDirty = 1; sTR.dirty = 1; sPunch.dirty = 1; }
#ifdef DC_PROF_SKIP
// Set by the front-end between the DC_GFX_SHADOW tags; a change starts new PT/TR batches so the
// shadows never share one with other geometry.
static uint8_t sProfShadow = 0;
void gfx_pvr_prof_shadow(int on) {
    if (sProfShadow != (uint8_t) !!on) {
        sProfShadow = (uint8_t) !!on;
        sTR.dirty = 1;
        sPunch.dirty = 1;
    }
}
#endif
// Compile one of the SIX base headers ONCE via the official pvr_poly_cxt+pvr_poly_compile, so
// every PER-LIST field (list_type/alpha/txralpha/blend defaults) is correct for this list. The
// textured bases seed from whatever texture is bound on first use; their texture-specific fields
// get overwritten per draw, so the seed texture is irrelevant.
static void pvr_ensure_base(int textured, int kind, int list) {
    if (sBaseHdrValid[textured][kind]) return;
    pvr_poly_cxt_t cxt;
    if (textured) {
        struct PvrTex *t = &sTextures[sBoundTex];
        pvr_poly_cxt_txr(&cxt, list, t->fmt, t->w, t->h, t->addr,
                         t->filter ? PVR_FILTER_BILINEAR : PVR_FILTER_NONE);
    } else {
        pvr_poly_cxt_col(&cxt, list);
    }
    // SMALL (not NONE): the front-end already does N64 backface culling (G_CULL_* in
    // gfx_sp_tri1), so HW culling isn't needed for winding — but SMALL also discards
    // degenerate / near-zero-area triangles the front-end lets through, which were
    // producing visible degen artifacts. Winding-direction culling stays off via this.
    cxt.gen.culling  = PVR_CULLING_SMALL;
    cxt.gen.specular = PVR_SPECULAR_ENABLE;  // combiner routes additive offsets into oargb
    pvr_poly_compile(&sBaseHdr[textured][kind], &cxt);
    sBaseHdrValid[textured][kind] = 1;
}

// Build a poly header for the current state. pvr_poly_compile runs only ONCE per base (6 total);
// per draw this copies the matching base and bit-patches the texture fields + per-draw state
// in place (OoT technique) — no per-draw recompile. Per-list state stays from the base.
static void pvr_compile_header(pvr_poly_hdr_t *out, int kind) {
    int list = (kind == PVR_KIND_TR) ? PVR_LIST_TR_POLY
             : (kind == PVR_KIND_PT) ? PVR_LIST_PT_POLY
             :                         PVR_LIST_OP_POLY;
    int textured = cur_shader && cur_shader->texture_used &&
                   sBoundTex && sTextures[sBoundTex].addr;
    pvr_ensure_base(textured, kind, list);
    *out = sBaseHdr[textured][kind];

    if (textured) {
        struct PvrTex *t = &sTextures[sBoundTex];
        // Texture fields, encoded exactly as KOS pvr_poly_compile (pvr_prim.c): mode3 =
        // format | ((addr & 0x00fffff8) >> 3); mode2 USIZE/VSIZE = ctz(dim)-3; filter/clamp/flip.
        out->mode3 = (uint32_t) t->fmt |
                     ((((uint32_t)(uintptr_t) t->addr) & 0x00fffff8u) >> 3);
        out->m2.u_size      = (pvr_uv_size_t)(__builtin_ctz(t->w) - 3);
        out->m2.v_size      = (pvr_uv_size_t)(__builtin_ctz(t->h) - 3);
        out->m2.filter_mode = t->filter ? PVR_FILTER_BILINEAR : PVR_FILTER_NONE;
        out->m2.u_clamp     = t->clampu;
        out->m2.v_clamp     = t->clampv;
        out->m2.u_flip      = t->flipu;
        out->m2.v_flip      = t->flipv;
        // texenv DERIVED from the N64 combiner (REPLACE/MODULATE/DECAL/MODULATEALPHA).
        out->m2.shading     = (pvr_txr_shading_mode_t) sTexEnv;
    }
    // Per-draw state (per-list alpha/txralpha/blend defaults stay from the base):
    // z = 1/w (larger == nearer): GEQUAL keeps the nearer fragment; ALWAYS == test off.
    out->m1.depth_cmp       = sDepthTest  ? PVR_DEPTHCMP_GEQUAL : PVR_DEPTHCMP_ALWAYS;
    out->m1.depth_write_dis = sDepthWrite ? 0 : 1;
    // PVR vertex fog reads density from per-vertex oargb ALPHA (front-end packs it); off clears.
    out->m2.fog_type        = sFogEnabled ? PVR_FOG_VERTEX : PVR_FOG_DISABLE;
    if (kind == PVR_KIND_TR) {
        // Only TR overrides blend (factors DERIVED from the N64 blender). OP/PT keep the base's
        // per-list defaults (ONE/ZERO and SRCALPHA/INVSRCALPHA).
        out->m2.blend_src = (pvr_blend_mode_t) sBlendSrc;
        out->m2.blend_dst = (pvr_blend_mode_t) sBlendDst;
    }
}

#ifdef DC_PROF_LISTS
// Records where a new PT/TR batch's alpha comes from (PVR texenv: REPLACE and MODULATE take the
// texel's alpha, DECAL the vertex's, MODULATEALPHA both; untextured, the vertex's).
static void prof_batch_alpha(PvrBatch *bt, int kind) {
    int textured = cur_shader && cur_shader->texture_used && sBoundTex && sTextures[sBoundTex].addr;

    bt->alpha_vtx = !textured || sTexEnv == PVR_TXRENV_DECAL || sTexEnv == PVR_TXRENV_MODULATEALPHA;
    bt->alpha_tex = textured && sTexEnv != PVR_TXRENV_DECAL && !sTextures[sBoundTex].opaque;
    bt->blend_src = kind != PVR_KIND_TR ||
                    (sBlendSrc == PVR_BLEND_SRCALPHA && sBlendDst == PVR_BLEND_INVSRCALPHA) ||
                    (sBlendSrc == PVR_BLEND_ONE && sBlendDst == PVR_BLEND_ZERO);
    bt->src = textured ? sTextures[sBoundTex].src : 0;
}
#define PROF_BATCH_ALPHA(bt, kind) prof_batch_alpha(bt, kind)

// The front-end names the N64 address of the texture it is about to upload (import_texture).
void gfx_pvr_prof_tex_src(uint32_t addr) {
    sTextures[sCurBound].src = addr;
}
#else
#define PROF_BATCH_ALPHA(bt, kind) ((void) 0)
#endif

// Append n vertices (n/3 tris) of already-screen-baked dc_fast_t to a bucket,
// starting a new batch when the bucket's header state has changed.
static void pvr_append(PvrBucket *b, int kind, const dc_fast_t *tris, size_t n) {
#ifdef DC_PROF_SKIP
    if (b->cur >= 0 && b->batch[b->cur].is_2d) b->dirty = 1;
#endif
    if (b->dirty || b->cur < 0) {
        if (b->nbatch >= b->max_batch) { if (sDropB++ < 8) printf("PVR_DROP batch %d\n", kind); return; }
        b->cur = b->nbatch++;
        pvr_compile_header(&b->batch[b->cur].hdr, kind);
        PROF_BATCH_ALPHA(&b->batch[b->cur], kind);
#ifdef DC_PROF_SKIP
        b->batch[b->cur].is_2d = 0;
        b->batch[b->cur].is_shadow = sProfShadow;
#endif
        b->batch[b->cur].start = b->nverts;
        b->batch[b->cur].count = 0;
        b->dirty = 0;
    }
    if (b->nverts + n > b->max_verts) { if (sDropV++ < 8) printf("PVR_DROP verts %d\n", kind); return; }
    for (size_t i = 0; i < n; i++) {
        pvr_vertex_t *v = &b->verts[b->nverts++];
        *v = *(const pvr_vertex_t *) &tris[i];
        v->flags = ((i % 3) == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        // oargb (additive offset) carried through dc_fast_t.pad0 — do not zero.
    }
    b->batch[b->cur].count += n;
}

// Reserve n verts in the deferred bucket for `kind` (1=PT/sPunch, 2=TR/sTR) and return a write
// pointer into b->verts so the FRONT-END bakes vertices straight into the bucket — no buf_vbo, no
// copy. Starts a new batch (header compiled from current state) when the bucket is dirty, exactly
// like pvr_append. Vertex flags are stamped at flush time (pvr_flush_bucket). NULL on overflow.
dc_fast_t *pvr_reserve(int kind, size_t n) {
    PvrBucket *b = (kind == PVR_KIND_TR) ? &sTR : &sPunch;
#ifdef DC_PROF_SKIP
    if (b->cur >= 0 && b->batch[b->cur].is_2d) b->dirty = 1;
#endif
    if (b->dirty || b->cur < 0) {
        if (b->nbatch >= b->max_batch) { if (sDropB++ < 8) printf("PVR_DROP batch %d\n", kind); return NULL; }
        b->cur = b->nbatch++;
        pvr_compile_header(&b->batch[b->cur].hdr, kind);
        PROF_BATCH_ALPHA(&b->batch[b->cur], kind);
#ifdef DC_PROF_SKIP
        b->batch[b->cur].is_2d = 0;
        b->batch[b->cur].is_shadow = sProfShadow;
#endif
        b->batch[b->cur].start = b->nverts;
        b->batch[b->cur].count = 0;
        b->dirty = 0;
    }
    if (b->nverts + n > b->max_verts) { if (sDropV++ < 8) printf("PVR_DROP verts %d\n", kind); return NULL; }
    dc_fast_t *out = (dc_fast_t *) &b->verts[b->nverts];
    b->nverts += n;
    b->batch[b->cur].count += n;
    // Stamp strip flags now: the front-end bakes vert/uv/colour into this slice but never flags. These
    // are TRIANGLES (n == n_tris*3), so EOL on every 3rd vertex. (2D quads set their own 4-vert flags.)
    for (size_t i = 0; i < n; i++)
        out[i].flags = ((i % 3) == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    return out;
}

// Send one store queue's 32 bytes to the TA. Unlike pvr_dr_commit (sq_flush) this has no "memory"
// clobber: the asm only reads this vertex, so GCC keeps everything else in registers around it.
static inline void sq_send(pvr_vertex_t *v) {
    __asm__ __volatile__("pref @%0" : : "r"(v), "m"(*v));
}

// One store-queue vertex: pvr_dr_target's double-buffered address, kept in a register by the
// caller instead of the global pvr_dr_addr being loaded and stored per vertex.
static inline pvr_vertex_t *sq_next(uint32_t *addr) {
    *addr ^= 32;
    return (pvr_vertex_t *) __builtin_assume_aligned((void *) *addr, 32);
}

// Replay a bucket's batches (header + its verts) to the open list via the DR path.
static void pvr_flush_bucket(PvrBucket *b) {
    uint32_t addr = pvr_dr_addr;
    for (int i = 0; i < b->nbatch; i++) {
        PvrBatch *bt = &b->batch[i];
        if (!bt->count) continue;
#ifdef DC_PROF_SKIP
        if (bt->is_2d ? (PROF_SKIP(SKIP_2D) || (b == &sTR && PROF_SKIP(SKIP_2D_TR)))
                      : (b == &sTR && PROF_SKIP(SKIP_3D_TR))) continue;
        if (bt->is_shadow && PROF_SKIP(SKIP_SHADOW)) continue;
#endif
        pvr_poly_hdr_t *hp = (pvr_poly_hdr_t *) sq_next(&addr);
        *hp = bt->hdr;
        sq_send((pvr_vertex_t *) hp);
        const pvr_vertex_t *src = &b->verts[bt->start];
        for (uint32_t v = 0; v < bt->count; v++) {
            pvr_vertex_t *vp = sq_next(&addr);
            *vp = src[v];
            // Flags are already set in the bucket: 3-vert tris by pvr_reserve, 4-vert quads by
            // gfx_pvr_draw_quad_2d. Do NOT re-stamp here — a blanket (v%3) would corrupt the 2D quads.
            sq_send(vp);
        }
    }
    pvr_dr_addr = addr;
}

// Scratch for POT padding (max 256x256 @ 16bpp = 128KB). MK64 textures are small.
static uint16_t sPadScratch[256 * 256] __attribute__((aligned(32)));

static inline uint32_t pvr_next_pot(uint32_t v) {
    v--; v |= v>>1; v |= v>>2; v |= v>>4; v |= v>>8; v |= v>>16; return v+1;
}
static inline int pvr_is_pot(uint32_t v) { return (v & (v - 1)) == 0; }

// Pad-copy with clamp-to-edge): real image at top-left,
// pad columns/rows replicate the edge so bilinear at the border samples real texels.
static void pvr_pad16(const uint16_t *in, int iw, int ih, uint16_t *out, int ow, int oh) {
    int y;

    for (y = 0; y < ih; y++) {
        uint16_t *o = out + (y * ow);
        shz_memcpy(o, in + (y * iw), iw * 2);
        uint16_t edge = (iw > 0) ? in[(y * iw) + iw - 1] : 0;
        for (int x = iw; x < ow; x++) o[x] = edge;
    }
    if (ih > 0) {
        const uint16_t *last = out + ((ih - 1) * ow);
        for (; y < oh; y++) shz_memcpy(out + (y * ow), last, ow * 2);
    }
}

// Framebuffer dimensions — must match gfx_dc.c SCR_WIDTH/HEIGHT. LOWRES = native 240p (320x240).
#if LOWRES
#define DC_FB_W 320
#define DC_FB_H 240
#else
#define DC_FB_W 640
#define DC_FB_H 480
#endif

// Mirror of OoT's known-good params for this toolchain:
//   {OP, OP_MOD, TR, TR_MOD, PT} bin sizes, vtxbuf, dma, fsaa, autosort_disabled, overflow
// Live list is OP (guaranteed-opaque geometry). Alpha-TEST CUTOUTS (hard-edged fences/
// foliage/glyphs — texture_edge or texture-driven alpha) go to the deferred PT bucket;
// real alpha-BLEND surfaces go to the deferred TR bucket. At end_frame PT is flushed first
// (writes depth, alpha-tested opaque), then TR composites over it. All three lists' bins
// are live -> {OP, OP_MOD, TR, TR_MOD, PT}.
#ifndef DC_PT_CHUNK
#define DC_PT_CHUNK 64
#endif
_Static_assert(DC_PT_CHUNK >= 32 && DC_PT_CHUNK <= 512, "ISP_FEED_CFG punch-through chunk, vertices");
static pvr_init_params_t sPvrParams = {
    { PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_32 },
    512 * 1024,
    1,  // dma
    DC_FSAA,  // fsaa: horizontal supersampling (DC_RENDER_W wide)
    0,  // autosort_disabled (0 = autosort; presort measured no faster with FSAA, 2026-10-24)
    2,  // overflow buffers
    0,  // vbuf_doublebuf_disabled (0 = double-buffering on)
};

// ===========================================================================
// GfxRenderingAPI implementation
// ===========================================================================

static void gfx_pvr_unload_shader(UNUSED struct ShaderProgram *old_prg) {
    cur_shader = NULL;
}

static void gfx_pvr_load_shader(struct ShaderProgram *new_prg) {
    cur_shader = new_prg;
    pvr_mark_dirty();   // textured-vs-colour header depends on the combiner
}

static struct ShaderProgram *gfx_pvr_create_and_load_new_shader(uint32_t shader_id) {
    struct CCFeatures ccf;
    gfx_cc_get_features(shader_id, &ccf);

    struct ShaderProgram *prg = &shader_program_pool[shader_program_pool_size++];
    prg->shader_id = shader_id;
    prg->cc = ccf;
    prg->num_inputs = ccf.num_inputs;
    prg->texture_used = ccf.used_texture;   // SM64 single-texture CCFeatures

    if (ccf.used_texture && ccf.num_inputs) {
        prg->mix = SH_MT_TEXTURE_COLOR;
    } else if (ccf.used_texture) {
        prg->mix = SH_MT_TEXTURE;
    } else if (ccf.num_inputs > 1) {
        prg->mix = SH_MT_COLOR_COLOR;
    } else if (ccf.num_inputs) {
        prg->mix = SH_MT_COLOR;
    } else {
        prg->mix = SH_MT_NONE;
    }

    prg->enabled = 0;
    gfx_pvr_load_shader(prg);
    return prg;
}

static struct ShaderProgram *gfx_pvr_lookup_shader(uint32_t shader_id) {
    for (size_t i = 0; i < shader_program_pool_size; i++)
        if (shader_program_pool[i].shader_id == shader_id)
            return &shader_program_pool[i];
    return NULL;
}

static bool gfx_pvr_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs) {
    *num_inputs = prg->num_inputs;
    return prg->texture_used;   // SM64 rapi: returns the single used-texture flag
}

static uint32_t gfx_pvr_new_texture(void) {
    uint32_t id = ++sTexCount;
    // Clamp guards the array, but a clamp means the id space is exhausted THIS reset cycle:
    // every further texture aliases slot 1023 -> "same texture everywhere". sTexCount resets in
    // gfx_pvr_clear_all_textures, so this should not fire; if it does, a single course needs
    // >PVR_TEX_MAX live textures. Don't fail silently.
    if (id >= PVR_TEX_MAX) {
        static int sTexIdClampLog = 0;
        if (sTexIdClampLog++ < 8) printf("PVR_TEXID_CLAMP sTexCount=%u >= PVR_TEX_MAX=%u\n", (unsigned)id, (unsigned)PVR_TEX_MAX);
        id = PVR_TEX_MAX - 1;
    }
    memset(&sTextures[id], 0, sizeof(sTextures[id]));
    return id;
}

static void gfx_pvr_select_texture(uint32_t texture_id) {
    // Dirty the poly header only when the binding actually CHANGES. The front-end's texture cache
    // calls this on every lookup (~195/frame), overwhelmingly re-binding the already-bound texture;
    // an unconditional dirty forced a header re-emit + PT/TR batch split per lookup. Re-uploads
    // dirty for themselves in gfx_pvr_upload_texture, so a same-id rebind is header-neutral.
    // (Matches mk64-dc/sf64-dc, which already had this guard; SM64 was missing it.)
    uint8_t changed = (sBoundTex != texture_id);
    sCurBound = texture_id;   // upload target follows the most recent bind
    sBoundTex = texture_id;
    if (changed)
        pvr_mark_dirty();
}

static void gfx_pvr_upload_texture(const uint8_t *buf16, int width, int height, unsigned int type) {
    struct PvrTex *t = &sTextures[sCurBound];
    // Twiddle every texture. SM64's cache uploads each texture ONCE per area (miss -> upload; hits
    // reuse; whole set nuked at area/course reset), so the Morton reorder is a one-time cost, not
    // per-frame like MK64. N64 TMEM (4KB) caps every single load far under the pad scratch, so all
    // uploads end up POT after padding -- a non-twiddled/NPOT path is unreachable here.
    int fmt = type;   // twiddled == no PVR_TXRFMT_NONTWIDDLED bit

    const uint16_t *src = (const uint16_t *) buf16;
    uint32_t fw = width, fh = height;
    float us = 1.0f, vs = 1.0f;

    if (!pvr_is_pot(width) || !pvr_is_pot(height) || width < 8 || height < 8) {
        fw = (width  < 8) ? 8 : (pvr_is_pot(width)  ? (uint32_t) width  : pvr_next_pot(width));
        fh = (height < 8) ? 8 : (pvr_is_pot(height) ? (uint32_t) height : pvr_next_pot(height));
        // Gate on TOTAL texels vs the scratch capacity, not per-dimension: the title
        // background's 512x8 strips (512-wide, short) fit fine by area but a width cap
        // would wrongly reject them -> NPOT upload -> static noise over the screen.
        if ((uint32_t) fw * (uint32_t) fh <= 256u * 256u) {
            pvr_pad16(src, width, height, sPadScratch, fw, fh);
            src = sPadScratch;
            us = (float) width  / (float) fw;
            vs = (float) height / (float) fh;
        } else {
            fw = width; fh = height;   // genuinely too big for scratch: upload as-is (rare)
        }
    }

    uint32_t size = fw * fh * 2;
    // Mirror glTexImage2D: reuse the existing VRAM allocation when the new upload fits,
    // only (re)allocate when it's bigger. The whole texture VRAM set is freed at the
    // glDeleteTextures point (gfx_pvr_clear_all_textures, via nuke_everything).
    if (t->addr && size > t->alloc_size) { pvr_mem_free(t->addr); t->addr = NULL; }
    if (!t->addr) { t->addr = pvr_mem_malloc(size); t->alloc_size = t->addr ? size : 0; }
    if (t->addr) pvr_txr_load_ex((void *) src, t->addr, fw, fh, PVR_TXRLOAD_16BPP);
#ifdef DC_PROF_LISTS
    {
        const uint16_t *px = (const uint16_t *) buf16;
        uint16_t alpha = (fmt == PVR_TXRFMT_ARGB4444) ? 0xf000 : (fmt == PVR_TXRFMT_ARGB1555) ? 0x8000 : 0;

        t->opaque = 1;
        for (int i = 0; i < width * height; i++) {
            if ((px[i] & alpha) != alpha) { t->opaque = 0; break; }
        }
    }
#endif

    t->w = fw; t->h = fh; t->fmt = fmt;
    t->u_scale = us; t->v_scale = vs;
    pvr_mark_dirty();
}

static void gfx_pvr_set_sampler_parameters(bool linear_filter, uint32_t cms, uint32_t cmt) {
    struct PvrTex *t = &sTextures[sBoundTex];
    // Match GLdc gfx_cm_to_opengl precedence: CLAMP wins, else MIRROR (mirror-repeat),
    // else plain repeat. PVR clamp == GL_CLAMP, PVR flip == GL_MIRRORED_REPEAT.
    uint8_t filter = linear_filter ? 1 : 0;
    uint8_t clampu = (cms & G_TX_CLAMP) ? 1 : 0;
    uint8_t clampv = (cmt & G_TX_CLAMP) ? 1 : 0;
    uint8_t flipu  = (!(cms & G_TX_CLAMP) && (cms & G_TX_MIRROR)) ? 1 : 0;
    uint8_t flipv  = (!(cmt & G_TX_CLAMP) && (cmt & G_TX_MIRROR)) ? 1 : 0;
    // The front-end calls this for EVERY textured primitive (gfx_sp_tri1/quad/texrect), not just on
    // change. Dirtying unconditionally re-emitted the poly header per triangle: a run of same-texture
    // translucent tris (coins/flames in Bowser fights) fragmented into ONE TR batch PER TRIANGLE
    // (~580 measured), and every opaque textured tri needlessly re-emitted its OP header. Only dirty
    // when THIS texture's sampler state actually changes; texture-IDENTITY changes are already
    // dirtied by gfx_pvr_select_texture, so same-state runs now coalesce into one batch/header.
    if (t->filter == filter && t->clampu == clampu && t->clampv == clampv
            && t->flipu == flipu && t->flipv == flipv) {
        return;
    }
    t->filter = filter; t->clampu = clampu; t->clampv = clampv; t->flipu = flipu; t->flipv = flipv;
    pvr_mark_dirty();
}

// Free ALL cached texture VRAM. Called from nuke_everything at course/memory resets) — the
// point where the texture cache's VRAM (including reuse bloat) is reclaimed.
//
// CRITICAL: also RESET the id allocator. nuke_everything calls this and then
// reset_texcache() (front-end cache wipe), after which the front-end re-requests a fresh
// id (new_texture -> ++sTexCount) for every texture it re-encounters. If sTexCount is NOT
// reset here, it climbs monotonically across course/memory resets; once cumulative unique
// textures exceed PVR_TEX_MAX, new_texture clamps EVERY further id to slot 1023 and the
// whole scene draws with the last-uploaded texture (the "same texture everywhere" wedge,
// never recovers without restart). GLdc never hit this — its id space isn't capped.
void gfx_pvr_clear_all_textures(void) {
    for (uint32_t i = 0; i <= sTexCount && i < PVR_TEX_MAX; i++) {
        if (sTextures[i].addr) {
            pvr_mem_free(sTextures[i].addr);
            sTextures[i].addr = NULL;
            sTextures[i].alloc_size = 0;
        }
    }
    sTexCount = 0;   // realign with reset_texcache() (front-end), which runs right after
    sBoundTex = 0;
    sCurBound = 0;
}

extern void reset_texcache(void);

void nuke_everything(void) {
    gfx_pvr_clear_all_textures();
    reset_texcache();
#if DC_FSAA
    gfx_pvr_request_fsaa_for_level(gCurrLevelNum);
#endif
}


// PoT-padding UV correction (real/padded), consumed by the front-end's recip_tex_*
// in place of GLdc's get_current_*_scale (PVR build routes to these).
float gfx_pvr_get_u_scale(void) { return sTextures[sBoundTex].u_scale; }
float gfx_pvr_get_v_scale(void) { return sTextures[sBoundTex].v_scale; }

static void gfx_pvr_set_depth_test(bool depth_test) {
    if (sDepthTest != depth_test) { sDepthTest = depth_test; pvr_mark_dirty(); }
}
static void gfx_pvr_set_depth_mask(bool z_upd) {
    if (sDepthWrite != z_upd) { sDepthWrite = z_upd; pvr_mark_dirty(); }
}
static void gfx_pvr_set_tex_env(uint32_t mode) {
    // enum gfx_tex_env (front-end) == pvr_txr_shading_mode order, so store straight through.
    if (sTexEnv != (uint8_t) mode) { sTexEnv = (uint8_t) mode; pvr_mark_dirty(); }
}

static void gfx_pvr_set_zmode_decal(UNUSED bool zmode_decal) {
    // N64 ZMODE_DEC decal z-fighting is handled entirely in the front-end bake (gfx_sp_tri1):
    // a decal vert's z (1/w) is nudged nearer by PVR_DECAL_ZBIAS so it wins the GEQUAL depth
    // test against its coplanar base. No PVR poly-header change is needed, so this is a no-op —
    // which also avoids recompiling the header every time decal mode toggles (frequent).
}

// PVR vertex fog enable/disable — baked into the poly header, so a change dirties all lists.
void gfx_pvr_set_fog(uint8_t enabled) {
    enabled = enabled ? 1 : 0;
    if (sFogEnabled != enabled) { sFogEnabled = enabled; pvr_mark_dirty(); }
}

// Vertex fog colour register (0x00b4). Global, read at render time — write it directly
// (KOS's pvr_fog_vertex_color is a stub). Not part of the poly header, so no dirty needed.
void gfx_pvr_set_fog_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    PVR_SET(PVR_FOG_VERTEX_COLOR,
            PVR_PACK_COLOR(a * (1.0f / 255.0f), r * (1.0f / 255.0f),
                           g * (1.0f / 255.0f), b * (1.0f / 255.0f)));
}
static void gfx_pvr_set_viewport(UNUSED int x, UNUSED int y, UNUSED int w, UNUSED int h) { /* front-end SW viewport */ }
static void gfx_pvr_set_scissor(UNUSED int x, UNUSED int y, UNUSED int w, UNUSED int h)  { /* front-end SW scissor */ }
static void gfx_pvr_set_use_alpha(UNUSED bool use_alpha)        { /* OP/PT/TR routing comes via gfx_pvr_set_blend */ }

// Front-end list classifier, called directly (not via rapi) from gfx_sp_tri1/gfx_sp_quad_2d:
// kind 0 = OP (fully opaque -> live list), 1 = PT (alpha-test cutout -> sPunch bucket),
// 2 = TR (real alpha-blend -> sTR bucket). Routing only — does NOT dirty headers (each
// list's header is its own identity + depth/texture state). Persists across frames in
// lockstep with the front-end's tracker, so start_frame does not reset it.
void gfx_pvr_set_blend(uint8_t kind) {
    sListKind = kind;   // 0=OP (live), 1=PT (sPunch bucket), 2=TR (sTR bucket)
}

// Map the front-end's abstract gfx_blend_factor codes to PVR blend modes for the TR list.
// Only TR blends, so a change dirties only the TR header (not OP/PT). No shader-id keying.
void gfx_pvr_set_blend_factors(uint8_t src_code, uint8_t dst_code) {
    static const int kMap[] = {
        [GFX_BLENDF_ZERO]        = PVR_BLEND_ZERO,
        [GFX_BLENDF_ONE]         = PVR_BLEND_ONE,
        [GFX_BLENDF_SRCALPHA]    = PVR_BLEND_SRCALPHA,
        [GFX_BLENDF_INVSRCALPHA] = PVR_BLEND_INVSRCALPHA,
        [GFX_BLENDF_DSTALPHA]    = PVR_BLEND_DESTALPHA,
        [GFX_BLENDF_INVDSTALPHA] = PVR_BLEND_INVDESTALPHA,
    };
    int s = kMap[src_code], d = kMap[dst_code];
    if (s != sBlendSrc || d != sBlendDst) {
        sBlendSrc = s; sBlendDst = d;
        sTR.dirty = 1;   // blend state lives only in the TR header
#ifdef PVR_BLEND_DEBUG
        static int n = 0;
        if (n++ < 48)
            printf("BLENDDBG sid=%08lx src=%d dst=%d\n",
                   (unsigned long)(cur_shader ? cur_shader->shader_id : 0), s, d);
#endif
    }
}

#ifdef DC_PROF_LISTS
// On-screen pixels of a triangle: clipped to the framebuffer (Sutherland-Hodgman), then the
// shoelace area. Off-screen parts of big floor and sky triangles cost the PVR nothing.
static float prof_tri_px(const pvr_vertex_t *a, const pvr_vertex_t *b, const pvr_vertex_t *c) {
    float px[2][9][2] = { { { a->x, a->y }, { b->x, b->y }, { c->x, c->y } } };
    int n = 3, cur = 0;

    for (int edge = 0; edge < 4 && n > 0; edge++) {
        int axis = edge & 1;
        float bound = (edge < 2) ? 0.0f : (axis ? (float) DC_FB_H : (float) DC_RENDER_W);
        float dir = (edge < 2) ? 1.0f : -1.0f;   // inside: dir * (p - bound) >= 0
        int m = 0;

        for (int i = 0; i < n; i++) {
            const float *p = px[cur][i], *q = px[cur][(i + 1) % n];
            float dp = dir * (p[axis] - bound), dq = dir * (q[axis] - bound);

            if (dp >= 0.0f) {
                px[cur ^ 1][m][0] = p[0]; px[cur ^ 1][m][1] = p[1]; m++;
            }
            if ((dp >= 0.0f) != (dq >= 0.0f)) {
                float t = dp / (dp - dq);
                px[cur ^ 1][m][0] = p[0] + (q[0] - p[0]) * t;
                px[cur ^ 1][m][1] = p[1] + (q[1] - p[1]) * t;
                m++;
            }
        }
        n = m;
        cur ^= 1;
    }
    float area = 0.0f;
    for (int i = 0; i < n; i++) {
        const float *p = px[cur][i], *q = px[cur][(i + 1) % n];
        area += p[0] * q[1] - q[0] * p[1];
    }
    return 0.5f * __builtin_fabsf(area);
}

static void __attribute__((noinline, cold)) prof_count_op(const pvr_vertex_t *a, const pvr_vertex_t *b,
                                                          const pvr_vertex_t *c) {
    DC_PROF_COUNT(DCC_OP_TRIS, 1);
    DC_PROF_COUNT(DCC_OP_PX, (unsigned int) prof_tri_px(a, b, c));
}

// Triangles and pixels of a PT or TR bucket, and the part whose alpha is provably 1 everywhere:
// those could be drawn in the OP list (PT: the alpha test never fails; TR: the blend gives the
// source colour). Batches hold strips ended by EOL flags: 3-vertex triangles and 4-vertex quads.
static void prof_count_bucket(const PvrBucket *b, int kind) {
    for (int i = 0; i < b->nbatch; i++) {
        const PvrBatch *bt = &b->batch[i];
        const pvr_vertex_t *v = &b->verts[bt->start];
        uint32_t strip = 0, batch_px = 0, batch_tris = 0;

        for (uint32_t k = 0; k < bt->count; k++) {
            if (++strip >= 3) {
                const pvr_vertex_t *t0 = &v[k - 2], *t1 = &v[k - 1], *t2 = &v[k];
                unsigned int px = (unsigned int) prof_tri_px(t0, t1, t2);
                int opaque = bt->blend_src && !bt->alpha_tex &&
                             (!bt->alpha_vtx || (t0->argb & t1->argb & t2->argb) >> 24 == 0xff);

                DC_PROF_COUNT(kind == PVR_KIND_TR ? DCC_TR_TRIS : DCC_PT_TRIS, 1);
                DC_PROF_COUNT(kind == PVR_KIND_TR ? DCC_TR_PX : DCC_PT_PX, px);
                batch_px += px;
                batch_tris++;
                if (opaque && kind == PVR_KIND_TR) {
                    DC_PROF_COUNT(DCC_TR_OPQ_TRIS, 1);
                    DC_PROF_COUNT(DCC_TR_OPQ_PX, px);
                } else if (opaque) {
                    DC_PROF_COUNT(DCC_PT_OPQ_PX, px);
                }
            }
            if (v[k].flags == PVR_CMD_VERTEX_EOL) {
                strip = 0;
            }
        }
        if (batch_tris) {
            dc_prof_list_src(kind, bt->src, batch_px, batch_tris);
        }
    }
}
#define PROF_COUNT_OP(a, b, c) prof_count_op(a, b, c)
#else
#define PROF_COUNT_OP(a, b, c) ((void) 0)
#endif

// (Re)emit the live OP header to the open OP list when depth/texture state changed.
static inline void pvr_emit_op_header(void) {
    if (!sOpDirty) return;
    pvr_poly_hdr_t __attribute__((aligned(32))) hdr;
    pvr_compile_header(&hdr, PVR_KIND_OP);
    pvr_poly_hdr_t *hp = (pvr_poly_hdr_t *) pvr_dr_target(sDrState);
    *hp = hdr;
    pvr_dr_commit(hp);
    sOpDirty = 0;
}

// Stream OP geometry straight to the live OP list via DR. Called per source-triangle from the
// front-end (no buf_vbo) AND internally. Header re-emitted only on state change (sOpDirty), so a
// same-state run streams headerless after the first. External — no wrapper needed. (oargb carried
// through dc_fast_t.pad0 — do NOT zero it here.)
void pvr_submit_op(const dc_fast_t *tris, size_t n) {
    if (PROF_SKIP(SKIP_3D_OP) || (PROF_SKIP(SKIP_SKY) && n && tris[0].vert.z < 0.0001f)) return;
    pvr_emit_op_header();
    for (size_t i = 0; i < n; i++) {
        pvr_vertex_t *v = (pvr_vertex_t *) pvr_dr_target(sDrState);
        *v = *(const pvr_vertex_t *) &tris[i];
        v->flags = ((i % 3) == 2) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        pvr_dr_commit(v);
    }
#ifdef DC_PROF_LISTS
    for (size_t i = 0; i + 2 < n; i += 3)
        PROF_COUNT_OP((const pvr_vertex_t *) &tris[i], (const pvr_vertex_t *) &tris[i + 1],
                      (const pvr_vertex_t *) &tris[i + 2]);
#endif
}

// One triangle from three separately stored vertices (the front-end's baked-vertex cache).
__attribute__((section(".sub0.sm64_hot"))) void pvr_submit_op_tri(const dc_fast_t *a, const dc_fast_t *b,
                                                                   const dc_fast_t *c) {
    if (PROF_SKIP(SKIP_3D_OP) || (PROF_SKIP(SKIP_SKY) && a->vert.z < 0.0001f)) return;
    pvr_emit_op_header();
    uint32_t addr = pvr_dr_addr;
    pvr_vertex_t *v;
    v = sq_next(&addr);
    *v = *(const pvr_vertex_t *) a;
    v->flags = PVR_CMD_VERTEX;
    sq_send(v);
    v = sq_next(&addr);
    *v = *(const pvr_vertex_t *) b;
    v->flags = PVR_CMD_VERTEX;
    sq_send(v);
    v = sq_next(&addr);
    *v = *(const pvr_vertex_t *) c;
    v->flags = PVR_CMD_VERTEX_EOL;
    sq_send(v);
    pvr_dr_addr = addr;
    PROF_COUNT_OP((const pvr_vertex_t *) a, (const pvr_vertex_t *) b, (const pvr_vertex_t *) c);
}

// 2D quad depth counter owned by the front-end (gfx_draw_rectangle increments it).
// GLdc's start_frame reset it each frame; the PVR backend must too, or it grows
// unbounded. Positive base because PVR z is inverse-depth (negative would clip).
extern float screen_2d_z;
extern float gfx_frame_invw_max;

// A transition cover starts (gfx_dc_tags.h, gfx_retro_dc.c gfx_dc_tag). Its OP polys write depth
// above world_z_max, the frame's largest world 1/w, so the scene's PT and TR fail the depth test
// under it and cost no fill. 2D drawn before the cover in PT/TR (the HUD) would land on top of it:
// those lists composite after OP, with the depth test off. As on the N64 the cover must hide it
// and the transition's hole must show it, so lift that 2D (the z >= 1.0 overlay band; far-pinned
// backdrops stay) above the world and below the cover, and depth-test it. 2D drawn after the
// cover continues above it through screen_2d_z.
void gfx_pvr_cover_begin(float world_z_max) {
    PvrBucket *buckets[2] = { &sPunch, &sTR };
    for (int k = 0; k < 2; k++) {
        PvrBucket *b = buckets[k];
        for (int i = 0; i < b->nbatch; i++) {
            PvrBatch *bt = &b->batch[i];
            pvr_vertex_t *v = &b->verts[bt->start];
            uint32_t n = bt->count, j;
            if (bt->hdr.m1.depth_cmp != PVR_DEPTHCMP_ALWAYS || !n) continue;
            for (j = 0; j < n && v[j].z >= 1.0f; j++) {}
            if (j < n) continue;   // not all in the overlay band: leave the batch as it is
            for (j = 0; j < n; j++) v[j].z += world_z_max;
            bt->hdr.m1.depth_cmp = PVR_DEPTHCMP_GEQUAL;
        }
        b->dirty = 1;   // later 2D starts a batch with its own (test-off) header
    }
    screen_2d_z += world_z_max + 1.0f;
}

static void gfx_pvr_draw_triangles(float buf_vbo[], UNUSED size_t buf_vbo_len,
                                   size_t buf_vbo_num_tris) {
    // STAGE 3a: front-end now bakes screen-space dc_fast_t (gfx_sp_tri1 PVR path). dc_fast_t
    // shares pvr_vertex_t's 32-byte layout, so each vert is a direct SQ submit. The front-end
    // doesn't classify yet (stage 3b), so sListKind stays OP -> all geometry is opaque for now.
    const dc_fast_t *tris = (const dc_fast_t *) buf_vbo;
    const size_t n = buf_vbo_num_tris * 3;
    if (sListKind == PVR_KIND_TR)
        pvr_append(&sTR, PVR_KIND_TR, tris, n);
    else if (sListKind == PVR_KIND_PT)
        pvr_append(&sPunch, PVR_KIND_PT, tris, n);
    else
        pvr_submit_op(tris, n);   // OP, live
}

// 2D path (called directly by gfx_sp_quad_2d, not via the rapi table). The front-end builds
// the 4 screen-space quad verts ALREADY in PVR triangle-STRIP order (ul, ll, ur, lr), so this
// just emits them as one native 4-vertex strip (v3 = EOL -> 2 tris). Routing via
// gfx_pvr_set_blend: opaque HUD -> live OP; alpha-test glyphs -> PT; real alpha-blend -> TR.
static void gfx_pvr_draw_quad_2d(void *quad, UNUSED size_t buf_vbo_len, UNUSED size_t buf_vbo_num_tris) {
    const dc_fast_t *q = (const dc_fast_t *) quad;
#ifdef DC_PROF_SKIP
    if (PROF_SKIP(SKIP_2D)) return;
    if (sListKind == PVR_KIND_TR && PROF_SKIP(SKIP_FULL_2D_TR)) {
        // Full-screen: the quad (strip order ul, ll, ur, lr) spans at least 90% of each axis.
        float w = q[3].vert.x - q[0].vert.x, h = q[3].vert.y - q[0].vert.y;
        if (w >= 0.9f * DC_RENDER_W && h >= 0.9f * DC_FB_H) return;
    }
#endif

    if (sListKind == PVR_KIND_OP) {
        if (PROF_SKIP(SKIP_2D_OP)) return;
        pvr_emit_op_header();
        for (int i = 0; i < 4; i++) {
            pvr_vertex_t *o = (pvr_vertex_t *) pvr_dr_target(sDrState);
            *o = *(const pvr_vertex_t *) &q[i];
            o->flags = (i == 3) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            pvr_dr_commit(o);
        }
        PROF_COUNT_OP((const pvr_vertex_t *) &q[0], (const pvr_vertex_t *) &q[1], (const pvr_vertex_t *) &q[2]);
        PROF_COUNT_OP((const pvr_vertex_t *) &q[1], (const pvr_vertex_t *) &q[2], (const pvr_vertex_t *) &q[3]);
        return;
    }

    PvrBucket *b = (sListKind == PVR_KIND_TR) ? &sTR : &sPunch;
    int kind     = (sListKind == PVR_KIND_TR) ? PVR_KIND_TR : PVR_KIND_PT;
#ifdef DC_PROF_SKIP
    if (b->cur >= 0 && !b->batch[b->cur].is_2d) b->dirty = 1;
#endif
    if (b->dirty || b->cur < 0) {
        if (b->nbatch >= b->max_batch) { if (sDropB++ < 8) printf("PVR_DROP batch %d\n", kind); return; }
        b->cur = b->nbatch++;
        pvr_compile_header(&b->batch[b->cur].hdr, kind);
        PROF_BATCH_ALPHA(&b->batch[b->cur], kind);
#ifdef DC_PROF_SKIP
        b->batch[b->cur].is_2d = 1;
        b->batch[b->cur].is_shadow = 0;
#endif
        b->batch[b->cur].start = b->nverts;
        b->batch[b->cur].count = 0;
        b->dirty = 0;
    }
    if (b->nverts + 4 > b->max_verts) { if (sDropV++ < 8) printf("PVR_DROP verts %d\n", kind); return; }
    for (int i = 0; i < 4; i++) {
        pvr_vertex_t *o = &b->verts[b->nverts++];
        *o = *(const pvr_vertex_t *) &q[i];
        o->flags = (i == 3) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    }
    b->batch[b->cur].count += 4;
}

static void gfx_pvr_init(void) {
#ifdef __DREAMCAST__
#if LOWRES
    // Native 240p (progressive). NTSC 320x240 is non-interlaced; VGA has a 320x240 mode too.
    vid_set_mode(vid_check_cable() != CT_VGA ? DM_320x240_NTSC : DM_320x240_VGA, PM_RGB888);//_565);
#else
    if (vid_check_cable() != CT_VGA)
        vid_set_mode(DM_640x480_NTSC_IL, PM_RGB888);//, PM_RGB565);
    else
        vid_set_mode(DM_640x480_VGA, PM_RGB888);//, PM_RGB565);
#endif
#endif
    pvr_init(&sPvrParams);
    pvr_post_init();
}

// Everything set after pvr_init, again after a restart (gfx_pvr_switch_fsaa).
static void pvr_post_init(void) {
#if DC_PT_CHUNK != 64
    // Punch-through chunk of the tile renderer: ISP_FEED_CFG (PVR register 0x098) bits 13..4, in
    // vertices. Per tile the ISP takes PT polygons this many vertices at a time and repeats its
    // passes per chunk. pvr_init writes 0x00800408 (chunk 64); only the chunk field changes.
#ifndef PVR_ISP_FEED_CFG
#define PVR_ISP_FEED_CFG PVR_UNK_0098   /* KOS before 315741eb named it by its address */
#endif
    PVR_SET(PVR_ISP_FEED_CFG, 0x00800008u | ((unsigned) DC_PT_CHUNK << 4));
#endif
#if defined(__DREAMCAST__) && LOWRES
    PVR_SET(PVR_SCALER_CFG, 0x400);   // 240p: disable the vertical flicker/scanline filter
#endif
#define PM_DITHER_BIT 8
    uint32_t cfg = PVR_GET(PVR_FB_CFG_2);
    cfg &= ~PM_DITHER_BIT;
    PVR_SET(PVR_FB_CFG_2, cfg);

    // PT alpha-test reference: punch-through discards texels with alpha <= this, giving
    // N64 cutout/texture-edge transparency. 0x80 matches OoT. (ARGB1555 alpha is 0/255,
    // so this cleanly drops the transparent bit; ARGB4444 keeps alpha >= ~8.)
    *(volatile uint32_t *) 0xA05F811C = 0x80 + 0x40;

    // Pre-first-frame default only — gfx_pvr_start_frame drives the real clear from the game's
    // clear_color global every frame (see sm_rgba5551_to_rgbf below).
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);

    // Headers are compiled lazily per depth/texture/list state at draw time.
}

#if DC_FSAA
static int sFsaaWant = DC_FSAA;

// nuke_everything (a course load, inside a frame whose scene is open): choose the new course's
// mode; gfx_pvr_start_frame switches before the next scene begins.
// The player's choice (dc_video_options.h): off, on, or on except the courses whose fill does not
// fit 60 fps with it.
int gDcFsaaMode = DC_FSAA_MODE;

static void gfx_pvr_request_fsaa_for_level(int level) {
    static const s16 kOff[] = { DC_FSAA_OFF_LEVELS };
    sFsaaWant = gDcFsaaMode != DC_FSAA_MODE_OFF;
    if (gDcFsaaMode != DC_FSAA_MODE_CURATED) return;
    for (unsigned i = 0; i < sizeof(kOff) / sizeof(kOff[0]); i++) {
        if (kOff[i] == level) sFsaaWant = 0;
    }
}

extern void reset_texcache(void);
extern void gfx_dc_set_fsaa_x(int fsaa);

// Restart the PVR in the wanted FSAA mode (the DKR/enDjinn recipe). pvr_shutdown resets the VRAM
// allocator, so every texture pointer is forgotten, never freed, and the front-end cache is wiped:
// the course's textures upload again on first use, in the new mode.
static void gfx_pvr_switch_fsaa(void) {
    pvr_wait_ready();
    pvr_shutdown();
    sPvrParams.fsaa_enabled = sFsaaWant;
    pvr_init(&sPvrParams);
    pvr_post_init();
#if DC_OVERLAY
    dc_overlay_pvr_reset();
#endif
    for (uint32_t i = 0; i < PVR_TEX_MAX; i++) {
        sTextures[i].addr = NULL;
        sTextures[i].alloc_size = 0;
    }
    sTexCount = 0;
    sBoundTex = 0;
    sCurBound = 0;
    reset_texcache();
    sRenderW = DC_RENDER_W_1X << sFsaaWant;
    gfx_dc_set_fsaa_x(sFsaaWant);
    pvr_mark_dirty();
}
#endif

static void gfx_pvr_on_resize(void) { }

// The N64 scene clear color, an RGBA5551 value (low 16 bits) stashed by the game's
// clear_frame_buffer/clear_viewport (game_init.c) and geo_process_background. On this port those
// paths emit NO fillrect — they only set this global — so the BACKEND must consume it as its
// clear/background color. Without it every clear_frame_buffer scene shows the backend's hardcoded color.
extern int clear_color;

static inline void sm_rgba5551_to_rgbf(uint16_t c, float *r, float *g, float *b) {
    const float inv31 = 1.0f / 31.0f;
    *r = ((c >> 11) & 0x1F) * inv31;
    *g = ((c >>  6) & 0x1F) * inv31;
    *b = ((c >>  1) & 0x1F) * inv31;
}

static void gfx_pvr_start_frame(void) {
    float cr, cg, cb;
#if DC_FSAA
    if (sFsaaWant != sPvrParams.fsaa_enabled) {
        gfx_pvr_switch_fsaa();
    }
#endif
    DC_PROF_MARK(DCP_WAIT_BEGIN);
    pvr_wait_ready();
    DC_PROF_MARK(DCP_WAIT_END);
    pvr_scene_begin();
    // drive the PVR background plane from the game's clear_color (set last game-loop iteration).
    sm_rgba5551_to_rgbf((uint16_t) clear_color, &cr, &cg, &cb);
    pvr_set_bg_color(cr, cg, cb);
    // OP list open + live for the whole frame (guaranteed-opaque geometry only).
    // Anything with alpha (blend or alpha-test cutout) is deferred to the TR bucket.
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_dr_init(&sDrState);
    sCurrentList = PVR_LIST_OP_POLY;
    // 2D depth base: PVR z is 1/w, larger == nearer. The per-rect +0.0005 increment
    // (gfx_draw_rectangle) encodes 2D paint order into z, so the PT list resolves
    // 2D-among-2D layering correctly. Base 1.0 keeps those small increments well within
    // the depth buffer's resolution (a high base, e.g. 100, crushes them -> fill-rect
    // priority/ordering breaks). The black-rect issue this was once bumped for turned
    // out to be the 0x01200A00 shader, not depth, so 1.0 is the right base.
    screen_2d_z = 1.0f;

    // Disable the PVR near/far z-clip. The front-end already near-clips in software
    // (gfx_build_clipped_fan), and our far-plane background (the ortho skybox baked to
    // 1/w≈0.00001) sits below the default clip threshold and would otherwise be culled.
    pvr_set_zclip(0.0f);

    // Reset the deferred PT + TR buckets and force the live OP header to re-emit for the
    // new scene. sDepth*/sListKind persist in lockstep with the front-end's trackers.
    sTR.nverts = 0;
    sTR.nbatch = 0;
    sTR.cur = -1;
    sTR.dirty = 1;
    sPunch.nverts = 0;
    sPunch.nbatch = 0;
    sPunch.cur = -1;
    sPunch.dirty = 1;
    sOpDirty = 1;

    // Latch "did the last frame have a 3D (perspective) scene" so gfx_sp_tri1 can tell a
    // Z-off ortho BACKGROUND (over-skybox clouds, in a race) from a Z-off ortho OVERLAY
    // (menu glyph text). Prev-frame value avoids draw-order races within the frame.
    prev_frame_had_persp = cur_frame_persp;
    cur_frame_persp = 0;
    gfx_frame_invw_max = 0.0f;   // the frame's world 1/w bound (transition covers)
    has_done_3d = 0;   // reset: no 3D drawn yet this frame (title bg -> far until flag)
}

static void gfx_pvr_end_frame(void) {
    pvr_list_finish();   // close the OP list (opened once this frame)
#ifdef DC_PROF_LISTS
    prof_count_bucket(&sPunch, PVR_KIND_PT);
    prof_count_bucket(&sTR, PVR_KIND_TR);
#endif

    // Flush the deferred buckets, each into its own list opened once. PT FIRST: alpha-test
    // cutouts are opaque pixels that write depth, so they must be down before TR composites.
    if (sPunch.nbatch > 0 && !PROF_SKIP(SKIP_PT)) {
        pvr_list_begin(PVR_LIST_PT_POLY);
        pvr_dr_init(&sDrState);
        pvr_flush_bucket(&sPunch);
        pvr_list_finish();
    }
    // TR SECOND: real alpha-blend surfaces, autosorted, composited over OP + PT (depth-tested).
    const int tr = sTR.nbatch > 0 && !PROF_SKIP(SKIP_TR);
    if (tr || DC_OVERLAY) {
        pvr_list_begin(PVR_LIST_TR_POLY);
        pvr_dr_init(&sDrState);
        if (tr) pvr_flush_bucket(&sTR);
#if DC_OVERLAY
        dc_overlay_draw();   // last, over everything
#endif
        pvr_list_finish();
    }
}

// The x scale of the PVR's current mode: 2 with FSAA (twice as wide), else 1.
int gfx_pvr_fsaa_x(void) {
    return sPvrParams.fsaa_enabled ? 2 : 1;
}

static void gfx_pvr_finish_render(void) {
    // Submits the scene to the GPU and flips.
    pvr_scene_finish();
}

struct GfxRenderingAPI gfx_pvr_api = {
    gfx_pvr_unload_shader,
    gfx_pvr_load_shader,
    gfx_pvr_create_and_load_new_shader,
    gfx_pvr_lookup_shader,
    gfx_pvr_shader_get_info,
    gfx_pvr_new_texture,
    gfx_pvr_select_texture,
    gfx_pvr_upload_texture,
    gfx_pvr_set_sampler_parameters,
    gfx_pvr_set_depth_test,
    gfx_pvr_set_depth_mask,
    gfx_pvr_set_zmode_decal,
    gfx_pvr_set_tex_env,
    gfx_pvr_set_viewport,
    gfx_pvr_set_scissor,
    gfx_pvr_set_use_alpha,
    gfx_pvr_draw_triangles,
    gfx_pvr_draw_quad_2d,
    gfx_pvr_init,
    gfx_pvr_on_resize,
    gfx_pvr_start_frame,
    gfx_pvr_end_frame,
    gfx_pvr_finish_render,
};
