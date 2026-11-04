// pc-endjinn host build only (pc_endjinn.mk): what src/pc/gfx/gfx_pvr.c and gfx_dc.c are on the
// Dreamcast, on pc-endjinn's emulated PVR (pvr_dr_target/pvr_dr_commit packets, pvr_poly_compile
// headers, pvr_txr_load_ex textures; SDL2 + Vulkan behind it).
//
// The gfx_retro_dc.c front-end is shared and unchanged: it bakes screen-space PVR vertices
// (dc_fast_t == pvr_vertex_t) and routes each primitive to OP (live), PT or TR (deferred buckets,
// flushed at the end of the frame), exactly as on the Dreamcast. What differs:
//   * headers come from pc-endjinn's pvr_poly_compile, which encodes list, texture, filter,
//     depth test on/off, depth write and punch-through. It has no texture environment, blend
//     factors, UV clamp/flip, offset colour or fog: TR always blends SRCALPHA/INVSRCALPHA,
//     textures always modulate and repeat;
//   * depth test GREATER instead of GEQUAL (pc-endjinn's fixed compare);
//   * no store queues, no PVR registers; pacing is a 60 Hz timer instead of the vblank.
#include <PR/gbi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#undef bool
#define bool uint8_t
#include <kos.h>

#include "pc/gfx/gfx_cc.h"
#include "pc/gfx/gfx_rendering_api.h"
#include "pc/gfx/gfx_window_manager_api.h"
#include "pc/gfx/gl_fast_vert.h"
#include "macros.h"

_Static_assert(sizeof(dc_fast_t) == sizeof(pvr_vertex_t), "dc_fast_t must match pvr_vertex_t layout");

extern int cur_frame_persp;
extern int prev_frame_had_persp;
extern int has_done_3d;
extern float screen_2d_z;
extern float gfx_frame_invw_max;
extern int clear_color;
extern void reset_texcache(void);

// ---- shaders: only the "uses a texture" flag matters to the backend ----
struct ShaderProgram {
    uint32_t shader_id;
    int num_inputs;
    bool texture_used;
};
static struct ShaderProgram sShaders[64];
static int sShaderCount;
static struct ShaderProgram *sCurShader;

// ---- textures ----
#define TEX_MAX 1024
struct HostTex {
    pvr_ptr_t addr;
    uint32_t alloc_size;
    uint16_t w, h;   // as uploaded: padded, mirrored
    int fmt;
    uint8_t filter, flipu, flipv, built_flipu, built_flipv;
    float u_scale, v_scale;
    uint16_t *src;   // the front-end's texels, for a rebuild
    uint64_t content;   // a hash of them (SM64_PVR_HASH: textures by content, not by where they went)
    size_t src_cap;
    int src_w, src_h;
};
static struct HostTex sTextures[TEX_MAX];
static uint32_t sTexCount, sBoundTex;

// ---- state ----
#define KIND_OP 0
#define KIND_PT 1
#define KIND_TR 2
static uint8_t sDepthTest = 1, sDepthWrite = 1, sListKind = KIND_OP, sOpDirty = 1;

typedef struct {
    pvr_poly_hdr_t hdr;
    uint32_t start, count;
    uint8_t test_off;   // compiled with the depth test off (gfx_pvr_cover_begin)
} Batch;
typedef struct {
    pvr_vertex_t *verts;
    Batch *batch;
    uint32_t nverts, max_verts;
    int nbatch, max_batch, cur;
    uint8_t dirty;
} Bucket;
#define BUCKET_VERTS 16384
#define BUCKET_BATCHES 1024
static pvr_vertex_t sTrVerts[BUCKET_VERTS], sPtVerts[BUCKET_VERTS];
static Batch sTrBatch[BUCKET_BATCHES], sPtBatch[BUCKET_BATCHES];
static Bucket sTR = { sTrVerts, sTrBatch, 0, BUCKET_VERTS, 0, BUCKET_BATCHES, -1, 1 };
static Bucket sPT = { sPtVerts, sPtBatch, 0, BUCKET_VERTS, 0, BUCKET_BATCHES, -1, 1 };
static int sDrops;

static void mark_dirty(void) {
    sOpDirty = 1;
    sTR.dirty = 1;
    sPT.dirty = 1;
}

static const pvr_poly_hdr_t *sForcedHdr[3];   // gfx_pvr_native_header
// Its depth test: this file's headers keep the test on in PT and TR (compile_header), so a forced
// header cannot tell gfx_pvr_cover_begin that it was compiled with the test off.
static uint8_t sForcedTestOff[3];

static void compile_header(pvr_poly_hdr_t *out, int kind) {
    if (sForcedHdr[kind] != NULL) {
        *out = *sForcedHdr[kind];
        return;
    }
    pvr_list_t list = kind == KIND_TR ? PVR_LIST_TR_POLY : kind == KIND_PT ? PVR_LIST_PT_POLY : PVR_LIST_OP_POLY;
    struct HostTex *t = &sTextures[sBoundTex];
    int textured = sCurShader && sCurShader->texture_used && sBoundTex && t->addr;
    pvr_poly_cxt_t cxt;
    if (textured) {
        pvr_poly_cxt_txr(&cxt, list, t->fmt, t->w, t->h, t->addr, t->filter ? PVR_FILTER_BILINEAR : PVR_FILTER_NEAREST);
    } else {
        pvr_poly_cxt_col(&cxt, list);
    }
    cxt.gen.culling = PVR_CULLING_NONE;   // the front-end culls
    cxt.gen.alpha = kind == KIND_PT;
    // pc-endjinn draws every depth-test-off batch with its opaque pipeline, whatever the list. In PT
    // and TR keep the test on: what the front-end draws there without it is the 2D overlay band
    // (z >= 1, increasing in paint order), which passes GREATER over the world (z = 1/w) anyway.
    cxt.depth.comparison = sDepthTest || kind != KIND_OP ? PVR_DEPTHCMP_GREATER : PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = sDepthWrite;
    pvr_poly_compile(out, &cxt);
}

// The native renderer's headers (src/pc/gfx/gfx_native.inc.c; gfx_pvr.c has the Dreamcast's).
void gfx_pvr_header_for(int kind, pvr_poly_hdr_t *out) {
    compile_header(out, kind);
}

void gfx_pvr_native_header(int kind, const pvr_poly_hdr_t *hdr, int depth_test) {
    sForcedHdr[kind] = hdr;
    sForcedTestOff[kind] = !depth_test;
    if (kind == KIND_OP) sOpDirty = 1;
    else if (kind == KIND_PT) sPT.dirty = 1;
    else sTR.dirty = 1;
}

void gfx_pvr_native_end(void) {
    sForcedHdr[0] = sForcedHdr[1] = sForcedHdr[2] = NULL;
    mark_dirty();
}

// pc-endjinn reads a vertex colour of 0 as "use the header's colour" (white): keep a fully
// transparent black vertex transparent black.
static inline void copy_vertex(pvr_vertex_t *dst, const dc_fast_t *src, uint32_t flags) {
    memcpy(dst, src, sizeof(*dst));
    if (dst->argb == 0) dst->argb = 1;
    dst->flags = flags;
}

// SM64_PVR_HASH=file: one line per frame with a hash of every vertex sent and the header it draws
// with (not where headers fall, so batches split differently still compare equal), to compare two
// renderers' output frame by frame (a change against the build before it). SM64_PVR_DUMP=n writes
// frame n's vertices to file.n too.
static FILE *sHashFile;
static int sHashInit, sHashDumpFrame = -1;
static FILE *sHashDump;
static uint64_t sHash, sHashHdr;
static int sHashTex;   // the header is textured: an untextured one's vertices carry stale u, v
static uint32_t sHashVerts, sHashFrame;

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

static void hash_header(int list, const pvr_poly_hdr_t *h) {
    // The texture by its texels: the same texture uploads elsewhere when uploads come in another order.
    pvr_poly_hdr_t c = *h;
    uintptr_t base = (uintptr_t) c.reserved[0] | (uintptr_t) ((uint64_t) c.reserved[1] << 32);
    if (base != 0) {
        for (uint32_t i = 1; i <= sTexCount && i < TEX_MAX; i++) {
            if ((uintptr_t) sTextures[i].addr == base) {
                c.reserved[0] = (uint32_t) sTextures[i].content;
                c.reserved[1] = (uint32_t) (sTextures[i].content >> 32);
                break;
            }
        }
    }
    h = &c;
    sHashHdr = fnv(0xcbf29ce484222325ull ^ (uint64_t) list, h, sizeof(*h));
    sHashTex = (((const uint32_t *) h)[0] & (1u << 3)) != 0;   // the parameter word's texture bit
    if (sHashDump != NULL) {
        const uint32_t *w = (const uint32_t *) h;
        fprintf(sHashDump, "H%d %016llx %08x %08x %08x %08x %08x %08x %08x %08x\n", list, (unsigned long long) sHashHdr,
                w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    }
}

static void submit(const pvr_vertex_t *v) {
    pvr_vertex_t *t = (pvr_vertex_t *) pvr_dr_target();
    *t = *v;
    pvr_dr_commit(t);
    if (sHashFile != NULL) {
        // An untextured polygon's u, v are whatever the bake left there (it does not write them).
        pvr_vertex_t hv = *v;
        if (!sHashTex) hv.u = hv.v = 0.0f;
        sHash = fnv(sHash, &sHashHdr, sizeof(sHashHdr));
        sHash = fnv(sHash, &hv, sizeof(hv));
        sHashVerts++;
        if (sHashDump != NULL) {
            fprintf(sHashDump, "%016llx %08x %f %f %f %08x %08x %f %f\n", (unsigned long long) sHashHdr,
                    (unsigned) v->flags, v->x, v->y, v->z, (unsigned) v->argb, (unsigned) v->oargb, v->u, v->v);
        }
    }
}

static void emit_op_header(void) {
    if (!sOpDirty) return;
    pvr_poly_hdr_t *h = (pvr_poly_hdr_t *) pvr_dr_target();
    compile_header(h, KIND_OP);
    hash_header(KIND_OP, h);
    pvr_dr_commit(h);
    sOpDirty = 0;
}

static int bucket_open(Bucket *b, int kind, size_t n) {
    if (b->dirty || b->cur < 0) {
        if (b->nbatch >= b->max_batch) {
            if (sDrops++ < 8) printf("PVR_DROP batch %d\n", kind);
            return 0;
        }
        b->cur = b->nbatch++;
        compile_header(&b->batch[b->cur].hdr, kind);
        b->batch[b->cur].test_off = sForcedHdr[kind] != NULL ? sForcedTestOff[kind] : !sDepthTest;
        b->batch[b->cur].start = b->nverts;
        b->batch[b->cur].count = 0;
        b->dirty = 0;
    }
    if (b->nverts + n > b->max_verts) {
        if (sDrops++ < 8) printf("PVR_DROP verts %d\n", kind);
        return 0;
    }
    return 1;
}

dc_fast_t *pvr_reserve(int kind, size_t n) {
    Bucket *b = kind == KIND_TR ? &sTR : &sPT;
    if (!bucket_open(b, kind, n)) return NULL;
    dc_fast_t *out = (dc_fast_t *) &b->verts[b->nverts];
    b->nverts += n;
    b->batch[b->cur].count += n;
    for (size_t i = 0; i < n; i++) out[i].flags = (i % 3) == 2 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    return out;
}

void pvr_submit_op(const dc_fast_t *tris, size_t n) {
    emit_op_header();
    for (size_t i = 0; i < n; i++) {
        pvr_vertex_t v;
        copy_vertex(&v, &tris[i], (i % 3) == 2 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX);
        submit(&v);
    }
}

void pvr_submit_op_tri(const dc_fast_t *a, const dc_fast_t *b, const dc_fast_t *c) {
    pvr_vertex_t v;
    emit_op_header();
    copy_vertex(&v, a, PVR_CMD_VERTEX);
    submit(&v);
    copy_vertex(&v, b, PVR_CMD_VERTEX);
    submit(&v);
    copy_vertex(&v, c, PVR_CMD_VERTEX_EOL);
    submit(&v);
}

static void flush_bucket(Bucket *b) {
    for (int i = 0; i < b->nbatch; i++) {
        Batch *bt = &b->batch[i];
        if (!bt->count) continue;
        pvr_poly_hdr_t *h = (pvr_poly_hdr_t *) pvr_dr_target();
        *h = bt->hdr;
        hash_header(b == &sTR ? KIND_TR : KIND_PT, h);
        pvr_dr_commit(h);
        for (uint32_t k = 0; k < bt->count; k++) {
            pvr_vertex_t *v = &b->verts[bt->start + k];
            if (v->argb == 0) v->argb = 1;
            submit(v);
        }
    }
}

// gfx_pvr.c's transition cover (gfx_dc_tags.h): lift the 2D overlay band already in PT/TR above the
// world and depth-test it, so the cover hides it.
void gfx_pvr_cover_begin(float world_z_max) {
    Bucket *buckets[2] = { &sPT, &sTR };
    for (int k = 0; k < 2; k++) {
        Bucket *b = buckets[k];
        for (int i = 0; i < b->nbatch; i++) {
            Batch *bt = &b->batch[i];
            pvr_vertex_t *v = &b->verts[bt->start];
            uint32_t n = bt->count, j;
            if (!bt->test_off || !n) continue;
            for (j = 0; j < n && v[j].z >= 1.0f; j++) {}
            if (j < n) continue;
            for (j = 0; j < n; j++) v[j].z += world_z_max;
            bt->test_off = 0;
        }
        b->dirty = 1;
    }
    screen_2d_z += world_z_max + 1.0f;
}

// ---- GfxRenderingAPI ----
static void r_unload_shader(UNUSED struct ShaderProgram *p) {
    sCurShader = NULL;
}
static void r_load_shader(struct ShaderProgram *p) {
    sCurShader = p;
    mark_dirty();
}
static struct ShaderProgram *r_create_and_load_new_shader(uint32_t id) {
    struct CCFeatures ccf;
    gfx_cc_get_features(id, &ccf);
    struct ShaderProgram *p = &sShaders[sShaderCount++];
    p->shader_id = id;
    p->num_inputs = ccf.num_inputs;
    p->texture_used = ccf.used_texture;
    r_load_shader(p);
    return p;
}
static struct ShaderProgram *r_lookup_shader(uint32_t id) {
    for (int i = 0; i < sShaderCount; i++)
        if (sShaders[i].shader_id == id) return &sShaders[i];
    return NULL;
}
static bool r_shader_get_info(struct ShaderProgram *p, uint8_t *num_inputs) {
    *num_inputs = p->num_inputs;
    return p->texture_used;
}
static uint32_t r_new_texture(void) {
    uint32_t id = ++sTexCount;
    if (id >= TEX_MAX) id = TEX_MAX - 1;
    struct HostTex *t = &sTextures[id];
    uint16_t *src = t->src;
    size_t cap = t->src_cap;
    pvr_ptr_t addr = t->addr;
    uint32_t alloc = t->alloc_size;
    memset(t, 0, sizeof(*t));
    t->src = src;   // buffers are reused
    t->src_cap = cap;
    t->addr = addr;
    t->alloc_size = alloc;
    return id;
}
static void r_select_texture(uint32_t id) {
    if (sBoundTex != id) {
        sBoundTex = id;
        mark_dirty();
    }
}

static int is_pot(uint32_t v) {
    return v && (v & (v - 1)) == 0;
}
static uint32_t next_pot(uint32_t v) {
    v--; v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16;
    return v + 1;
}
static uint16_t sPad[512 * 512];

// The front-end hands 16-bit PVR texels (ARGB1555/ARGB4444). As gfx_pvr.c: pad to a power of two
// by edge replication, and the front-end scales the UVs by get_*_scale. pc-endjinn's headers carry
// no UV flip, so a G_TX_MIRROR texture is laid out mirrored here instead, twice as wide or high
// (the front-end's UVs then cover half of it per repeat), rebuilt when its mirror state changes.
static void build_texture(struct HostTex *t) {
    const int sw = t->src_w, sh = t->src_h;
    const int mx = t->flipu && is_pot(sw), my = t->flipv && is_pot(sh);
    const int w = sw << mx, h = sh << my;
    uint32_t fw = w < 8 ? 8 : next_pot(w), fh = h < 8 ? 8 : next_pot(h);
    const uint16_t *src = t->src;
    if (fw * fh > 512u * 512u) {
        fw = sw;
        fh = sh;
    } else {
        for (uint32_t y = 0; y < fh; y++) {
            int ty = (int) y < h ? (int) y : h - 1;
            if (ty >= sh) ty = 2 * sh - 1 - ty;
            uint16_t *o = sPad + y * fw;
            for (uint32_t x = 0; x < fw; x++) {
                int tx = (int) x < w ? (int) x : w - 1;
                if (tx >= sw) tx = 2 * sw - 1 - tx;
                o[x] = src[ty * sw + tx];
            }
        }
        src = sPad;
    }
    uint32_t size = fw * fh * 2;
    if (t->addr && size > t->alloc_size) {
        pvr_mem_free(t->addr);
        t->addr = NULL;
    }
    if (!t->addr) {
        t->addr = pvr_mem_malloc(size);
        t->alloc_size = t->addr ? size : 0;
    }
    if (t->addr) pvr_txr_load_ex(src, t->addr, fw, fh, PVR_TXRLOAD_16BPP);
    t->w = fw;
    t->h = fh;
    t->u_scale = (float) sw / fw;
    t->v_scale = (float) sh / fh;
    t->built_flipu = t->flipu;
    t->built_flipv = t->flipv;
    mark_dirty();
}

static void r_upload_texture(const uint8_t *buf16, int width, int height, unsigned int type) {
    struct HostTex *t = &sTextures[sBoundTex];
    size_t bytes = (size_t) width * height * 2;
    if (bytes > t->src_cap) {
        free(t->src);
        t->src = malloc(bytes);
        t->src_cap = t->src ? bytes : 0;
        if (!t->src) return;
    }
    memcpy(t->src, buf16, bytes);
    t->content = fnv(fnv(0xcbf29ce484222325ull, &width, sizeof(width)), buf16, bytes) ^ (uint64_t) type;
    t->src_w = width;
    t->src_h = height;
    t->fmt = (int) type;
    build_texture(t);
}
static void r_set_sampler_parameters(bool linear_filter, uint32_t cms, uint32_t cmt) {
    struct HostTex *t = &sTextures[sBoundTex];
    uint8_t flipu = !(cms & G_TX_CLAMP) && (cms & G_TX_MIRROR);
    uint8_t flipv = !(cmt & G_TX_CLAMP) && (cmt & G_TX_MIRROR);
    if (t->filter != (linear_filter ? 1 : 0)) {
        t->filter = linear_filter ? 1 : 0;
        mark_dirty();
    }
    t->flipu = flipu;
    t->flipv = flipv;
    if (t->src && (flipu != t->built_flipu || flipv != t->built_flipv)) build_texture(t);
}
static void r_set_depth_test(bool on) {
    if (sDepthTest != on) { sDepthTest = on; mark_dirty(); }
}
static void r_set_depth_mask(bool on) {
    if (sDepthWrite != on) { sDepthWrite = on; mark_dirty(); }
}
static void r_set_zmode_decal(UNUSED bool on) {
}
static void r_set_tex_env(UNUSED uint32_t mode) {
}
static void r_set_viewport(UNUSED int x, UNUSED int y, UNUSED int w, UNUSED int h) {
}
static void r_set_scissor(UNUSED int x, UNUSED int y, UNUSED int w, UNUSED int h) {
}
static void r_set_use_alpha(UNUSED bool on) {
}

static void r_draw_triangles(float buf_vbo[], UNUSED size_t len, size_t num_tris) {
    const dc_fast_t *tris = (const dc_fast_t *) buf_vbo;
    size_t n = num_tris * 3;
    if (sListKind == KIND_OP) {
        pvr_submit_op(tris, n);
        return;
    }
    dc_fast_t *out = pvr_reserve(sListKind, n);
    if (out) {
        for (size_t i = 0; i < n; i++) {
            uint32_t f = out[i].flags;
            out[i] = tris[i];
            out[i].flags = f;
        }
    }
}

// A 2D quad: 4 vertices in strip order (ul, ll, ur, lr).
static void r_draw_quad_2d(void *quad, UNUSED size_t len, UNUSED size_t num_tris) {
    const dc_fast_t *q = (const dc_fast_t *) quad;
    if (sListKind == KIND_OP) {
        emit_op_header();
        for (int i = 0; i < 4; i++) {
            pvr_vertex_t v;
            copy_vertex(&v, &q[i], i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX);
            submit(&v);
        }
        return;
    }
    Bucket *b = sListKind == KIND_TR ? &sTR : &sPT;
    if (!bucket_open(b, sListKind, 4)) return;
    for (int i = 0; i < 4; i++) copy_vertex(&b->verts[b->nverts++], &q[i], i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX);
    b->batch[b->cur].count += 4;
}

static void r_init(void) {
    pvr_init_params_t params = { { PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_32 },
                                 512 * 1024, 0, 0, 0, 2, 0 };
    if (pvr_init(&params) != 0) {
        fprintf(stderr, "pc-endjinn: pvr_init failed\n");
        exit(1);
    }
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
}
static void r_on_resize(void) {
}

static void r_start_frame(void) {
    uint16_t c = (uint16_t) clear_color;
    if (!sHashInit) {
        sHashInit = 1;
        const char *f = getenv("SM64_PVR_HASH"), *d = getenv("SM64_PVR_DUMP");
        if (f != NULL) sHashFile = fopen(f, "w");
        if (d != NULL) sHashDumpFrame = atoi(d);
    }
    sHash = 0xcbf29ce484222325ull;
    sHashVerts = 0;
    if (sHashFile != NULL && (int) sHashFrame == sHashDumpFrame) {
        char name[512];
        snprintf(name, sizeof(name), "%s.%d", getenv("SM64_PVR_HASH"), sHashDumpFrame);
        sHashDump = fopen(name, "w");
    }
    pvr_wait_ready();
    pvr_scene_begin();
    pvr_set_bg_color(((c >> 11) & 31) / 31.0f, ((c >> 6) & 31) / 31.0f, ((c >> 1) & 31) / 31.0f);
    pvr_list_begin(PVR_LIST_OP_POLY);
    screen_2d_z = 1.0f;
    sTR.nverts = sPT.nverts = 0;
    sTR.nbatch = sPT.nbatch = 0;
    sTR.cur = sPT.cur = -1;
    sTR.dirty = sPT.dirty = 1;
    sOpDirty = 1;
    prev_frame_had_persp = cur_frame_persp;
    cur_frame_persp = 0;
    gfx_frame_invw_max = 0.0f;
    has_done_3d = 0;
}

static void r_end_frame(void) {
    pvr_list_finish();
    if (sPT.nbatch > 0) {
        pvr_list_begin(PVR_LIST_PT_POLY);
        flush_bucket(&sPT);
        pvr_list_finish();
    }
    if (sTR.nbatch > 0) {
        pvr_list_begin(PVR_LIST_TR_POLY);
        flush_bucket(&sTR);
        pvr_list_finish();
    }
}

static void r_finish_render(void) {
    if (sHashFile != NULL) {
        fprintf(sHashFile, "%u %u %016llx\n", (unsigned) sHashFrame, (unsigned) sHashVerts, (unsigned long long) sHash);
        if (sHashDump != NULL) {
            fclose(sHashDump);
            sHashDump = NULL;
        }
    }
    sHashFrame++;
    pvr_scene_finish();
}

struct GfxRenderingAPI gfx_pvr_api = {
    r_unload_shader, r_load_shader, r_create_and_load_new_shader, r_lookup_shader, r_shader_get_info,
    r_new_texture, r_select_texture, r_upload_texture, r_set_sampler_parameters, r_set_depth_test,
    r_set_depth_mask, r_set_zmode_decal, r_set_tex_env, r_set_viewport, r_set_scissor, r_set_use_alpha,
    r_draw_triangles, r_draw_quad_2d, r_init, r_on_resize, r_start_frame, r_end_frame, r_finish_render,
};

// ---- the front-end's direct calls into gfx_pvr.c ----
void gfx_pvr_set_blend(uint8_t kind) {
    sListKind = kind;
}
void gfx_pvr_set_blend_factors(UNUSED uint8_t src, UNUSED uint8_t dst) {
}
void gfx_pvr_set_fog(UNUSED uint8_t enabled) {
}
void gfx_pvr_set_fog_color(UNUSED uint8_t r, UNUSED uint8_t g, UNUSED uint8_t b, UNUSED uint8_t a) {
}
float gfx_pvr_get_u_scale(void) {
    return sTextures[sBoundTex].u_scale;
}
float gfx_pvr_get_v_scale(void) {
    return sTextures[sBoundTex].v_scale;
}
int gfx_pvr_fsaa_x(void) {
    return 1;
}
// Area and course loads (gfx_pvr.c): every texture goes, and the front-end's cache with them.
void nuke_everything(void) {
    for (uint32_t i = 0; i <= sTexCount && i < TEX_MAX; i++) {
        if (sTextures[i].addr) pvr_mem_free(sTextures[i].addr);
        sTextures[i].addr = NULL;
        sTextures[i].alloc_size = 0;
    }
    sTexCount = 0;
    sBoundTex = 0;
    reset_texcache();
}

// ---- GfxWindowManagerAPI (gfx_dc.c): 640x480, paced to 60 Hz by the clock ----
static uint64_t now_ns(void) {
    return timer_ns_gettime64();
}
static uint64_t sNextPresent;

static void wm_init(UNUSED const char *name, UNUSED bool fullscreen) {
}
static void wm_set_keyboard_callbacks(UNUSED bool (*down)(int), UNUSED bool (*up)(int), UNUSED void (*all_up)(void)) {
}
static void wm_set_fullscreen_changed_callback(UNUSED void (*cb)(bool)) {
}
static void wm_set_fullscreen(UNUSED bool enable) {
}
static void wm_main_loop(void (*run)(void)) {
    for (;;) run();
}
static void wm_get_dimensions(uint32_t *w, uint32_t *h) {
    *w = 640;
    *h = 480;
}
static void wm_handle_events(void) {
}
static bool wm_start_frame(void) {
    return 1;
}
static void wm_swap_buffers_begin(void) {
}
// One presented frame per 1/60 s (the Dreamcast's vblank), whatever the display's refresh rate.
void gfx_pc_endjinn_hold_frame(void) {
    static int sUncapped = -1;
    if (sUncapped < 0) sUncapped = getenv("SM64_UNCAPPED") != NULL && atoi(getenv("SM64_UNCAPPED")) != 0;
    if (sUncapped) return;
    const uint64_t frame = 1000000000ull / 60u;
    uint64_t t = now_ns();
    if (sNextPresent == 0 || t > sNextPresent + 4 * frame) sNextPresent = t;
    while ((t = now_ns()) < sNextPresent) {
        uint64_t left = sNextPresent - t;
        struct timespec ts = { 0, (long) (left > 1000000 ? left - 500000 : 0) };
        if (ts.tv_nsec) nanosleep(&ts, NULL);
    }
    sNextPresent += frame;
}
static void wm_swap_buffers_end(void) {
    gfx_pc_endjinn_hold_frame();
}
static float wm_get_time(void) {
    return 0.0f;
}

struct GfxWindowManagerAPI gfx_pc_endjinn_wm = {
    wm_init, wm_set_keyboard_callbacks, wm_set_fullscreen_changed_callback, wm_set_fullscreen, wm_main_loop,
    wm_get_dimensions, wm_handle_events, wm_start_frame, wm_swap_buffers_begin, wm_swap_buffers_end, wm_get_time,
};
