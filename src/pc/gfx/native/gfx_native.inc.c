// DC_NATIVE=1: static display lists drawn from a compiled form (included by gfx_retro_dc.c, whose
// state and triangle path it shares).
//
// A display list in the program's data, one the game never builds at run time, is compiled the
// first time it is called: it runs through the interpreter as always, one command at a time, and
// what it does is recorded. Vertex loads keep their source vertices and the state they are lit and
// scaled with; triangles are grouped by the triangle state they draw with (a segment: the computed
// sTri, the list kind and the PVR header that state gives, with its texture already uploaded). No
// command is decoded and no triangle state is set up when the list is drawn again from that.
//
// What the list reads of the state it is called in is part of what it compiled to: the render mode
// of its layer, the geometry mode, a texture or combiner set before it. A compiled form (a variant)
// keeps those values, and is drawn only when they are the same again; a list called in up to
// NAT_MAX_VARIANTS states has that many. The environment and primitive colours are the exception:
// objects fade by the environment alpha set before their list, so a segment whose combiner reads a
// colour the list did not set compiles that combiner again per draw instead. What the list leaves
// set (its exit state) is put back after it draws, as the interpreter would have left it.
//
// The vertex loads are grouped into chunks (a run of loads and the triangles drawn from them), each
// with a bounding sphere: a chunk wholly outside one clip plane is skipped, loads and triangles,
// since every triangle in it would have been rejected anyway. The image is the interpreter's.
//
// The compiled forms hold PVR texture addresses: a texture cache reset (a level change, an FSAA
// switch) starts the cache over (sNatGen).

#define NAT_MAX_VARIANTS 4
#define NAT_HASH 4096                // display lists (power of two)
#define NAT_ARENA_BYTES (1024 * 1024)
#define NAT_REC_WORDS 16384          // one list's operation stream while it is recorded
#define NAT_REC_SEGS 256
#define NAT_REC_LOADS 1024
#define NAT_REC_LIGHTS 128
#define NAT_REC_CHUNKS 1024

// State groups a list can read from the state it is called in, or leave changed.
enum {
    NG_OTHERMODE = 1 << 0,
    NG_GEOM = 1 << 1,
    NG_COMBINE = 1 << 2,
    NG_TEX = 1 << 3,
    NG_SCALE = 1 << 4,
    NG_LIGHTS = 1 << 5,
    NG_FOG = 1 << 6,
    NG_ENV = 1 << 7,
    NG_PRIM = 1 << 8,
    NG_FOGCOL = 1 << 9,
    NG_FILL = 1 << 10,
};

// The parts of the texture state (NG_TEX) a list can set: a texture is the list's own only once it
// has set all of them; a load reads the image the caller may have set.
enum { NTW_TIMG = 1, NTW_LOAD = 2, NTW_TILE = 4, NTW_SIZE = 8, NTW_ALL = 15 };

// What triangles and vertex loads read of the geometry mode and the render mode's high word.
#define NAT_TRI_GEOM (G_ZBUFFER | G_CULL_BOTH | G_FOG)
#define NAT_VTX_GEOM (G_LIGHTING | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR | G_FOG)
#define NAT_TRI_OMH ((3U << G_MDSFT_CYCLETYPE) | (3U << G_MDSFT_TEXTFILT))

typedef struct {
    uint32_t oml, omh;
    uint32_t geom;
    uint32_t cmode, w0, w1;
    const uint8_t *timg_addr;
    uint8_t timg_siz;
    int timg_width;
    const uint8_t *ltex_addr;
    uint32_t ltex_size;
    __typeof__(rdp.texture_tile) tile;
    uint16_t s_scale, t_scale;
    Light_t lights[MAX_LIGHTS + 1];
    uint8_t num_lights;
    int16_t fog_mul, fog_offset;
    struct RGBA env, prim, fogcol, fill;
} NatState;

typedef struct __attribute__((aligned(32))) {
    pvr_poly_hdr_t hdr;
    __typeof__(sTri) tri;
    uint32_t geom;            // the geometry mode (the triangles' culling)
    uint32_t w0, w1;          // the combiner, for a per-draw compile (cc_live)
    uint16_t uls, ult;        // the tile origin the near-clip path reads
    uint8_t kind;             // OP, PT, TR
    uint8_t cc_live;          // NG_ENV / NG_PRIM: read from the caller's state, compiled per draw
    struct RGBA env, prim;    // the list's own colours otherwise
} NatSeg;

typedef struct {
    uint32_t geom;
    uint16_t s_scale, t_scale;
    int16_t fog_mul, fog_offset;
    int16_t lights;           // NatLights index, -1 unlit
    uint8_t scale_set;        // the list set the texture scale before this load
    // The light directions are taken (into model space) at this load: the first lit load after a
    // matrix or light count change (rsp.lights_changed). Other loads take only the colours of their
    // lights and keep the directions taken before, as the RSP does (Koopa's pink shorts).
    uint8_t dirs;
    uint8_t lights_in;        // the lights (bit i light i, bit 7 the count) it takes from the caller
} NatLoad;

typedef struct {
    Light_t lights[MAX_LIGHTS + 1];
    uint8_t num;
} NatLights;

typedef struct NatVariant {
    // The caller's state this was compiled in, compared for the groups in sig_mask (render mode
    // and geometry mode by bit: sig_oml/sig_omh/sig_geom).
    uint32_t sig_mask, sig_oml, sig_omh, sig_geom;
    NatState sig;
    // What the list leaves set: groups, and render/geometry mode bits.
    uint32_t exit_mask, exit_oml, exit_omh, exit_geom;
    uint8_t exit_tex;         // NTW_* parts of the texture state the list sets
    uint8_t lc_in;            // a lit load takes its light directions from rsp.lights_changed being set
    uint8_t sig_lights;       // the lights compared (NG_LIGHTS): bit i light i, bit 7 the count
    uint8_t ortho;            // compiled under an orthographic projection (its depth: tri_bake_vertex)
    int8_t lc_out;            // rsp.lights_changed as the list leaves it (-1: as it was)
    NatState exit;
    float sphere[4];          // the whole list's bounds (r < 0: not culled)
    const NatSeg *segs;
    const NatLoad *loads;
    const NatLights *lights;
    const float (*spheres)[4];   // the chunks' bounds
    const uintptr_t *ops;
} NatVariant;

typedef struct {
    const Gfx *dl;
    uint8_t nvar;
    uint8_t bad;              // never compiled: interpreted as always
    NatVariant *var[NAT_MAX_VARIANTS];
} NatEntry;

// The operation stream, in words that hold a pointer.
enum { NOP_END, NOP_VTX, NOP_TRI, NOP_CHUNK, NOP_FOGCOL };
//   NOP_VTX | n << 8 | dest << 16 | NAT_LIVE, vertices, load index
#define NAT_LIVE (1u << 24)   // the load's vertices outlive the list: loaded even in a culled chunk
#define NAT_CONV (1u << 25)   // the vertices are NatVtx (converted when compiled), not Vtx

// A load's vertices converted when the list compiles: the position as floats, the texture
// coordinates scaled by the load's texture scale as gfx_sp_vertex scales them, and the colour, or
// for a lit load the normal and alpha. (Not for texture-generating loads: they read the normal
// against the lookat, and stay Vtx.)
typedef struct {
    float x, y, z;
    float u, v;
    union {
        struct RGBA c;
        struct {
            int8_t n[3];
            uint8_t a;
        } ln;
    };
} NatVtx;
//   NOP_TRI | count << 8 | seg << 16, then count words: a | b << 8 | c << 16 (gfx_sp_tri1's order)
//   NOP_CHUNK | words to skip << 8, its bounds' index in spheres
//   NOP_FOGCOL, rgba

static NatEntry *sNatHash;
static uint8_t *sNatArena;
static uint32_t sNatArenaUsed, sNatArenaHigh;
static uint32_t sNatCacheGen;   // sNatGen + 1 when the cache was started (0: never)
static uint8_t sNatOff;                      // allocation failed: interpret everything

// Recording one list.
static struct {
    uint8_t active, fail;
    uint32_t read, written;
    uint32_t om_read_l, om_read_h, om_w_l, om_w_h, geom_read, geom_w;
    uint8_t light_w;          // lights written: bit i light i, bit 7 the count
    uint8_t tex_w;            // texture state written: NTW_*
    uint8_t light_read;       // lights read from the caller, as light_w
    uint8_t lc, lc_in, lc_touched;   // rsp.lights_changed as the list goes; read from the caller; set
    uint8_t soft;             // cannot compile in this state (but may in another)
    NatState entry;
    uintptr_t ops[NAT_REC_WORDS];
    uint32_t nops;
    uint32_t tri_op;          // the open NOP_TRI, or ~0
    NatSeg segs[NAT_REC_SEGS];
    uint32_t nsegs;
    int cur_seg;
    uint32_t seg_epoch;
    NatLoad loads[NAT_REC_LOADS];
    uint32_t nloads;
    NatLights lights[NAT_REC_LIGHTS];
    uint32_t nlights;
    uint32_t fogcols[16];
    uint32_t nfogcols;
    // Chunks: where each starts in ops, and which chunk a slot was last loaded in.
    uint32_t chunk_op[NAT_REC_CHUNKS];
    int16_t chunk_merge[NAT_REC_CHUNKS];   // the first chunk this one's triangles reach back to
    uint32_t nchunks;
    uint8_t chunk_has_tri;
    int16_t slot_chunk[MAX_VERTICES + 4];
    int16_t slot_load[MAX_VERTICES + 4];
    uint8_t dyn_vtx;          // a load from outside the program's data: no culling
} sRec;

#ifdef TARGET_PC_ENDJINN
#include <dlfcn.h>
// SM64_NAT_DBG=n: list the native draws of gfx_run's n-th frame (host debugging).
static int sNatDbgFrame = -2;
static unsigned sNatFrame;
static int nat_dbg(void) {
    if (sNatDbgFrame == -2) {
        const char *e = getenv("SM64_NAT_DBG");
        sNatDbgFrame = e != NULL ? atoi(e) : -1;
    }
    return (int) sNatFrame == sNatDbgFrame;
}
static const char *nat_name(const void *p) {
    Dl_info di;
    return dladdr(p, &di) && di.dli_sname != NULL && di.dli_saddr == p ? di.dli_sname : "?";
}
// The host prints what the native path did at exit (the Dreamcast counts it in DC_PROF's COUNTS).
static unsigned long sNatStat[6];   // draws, compiles, failed, chunks drawn, culled, variants full
#define NAT_STAT(i) (sNatStat[i]++)
static void nat_stat_print(void) {
    printf("NATIVE: draws %lu compiles %lu failed %lu chunks %lu culled %lu full %lu arena %u KB\n", sNatStat[0],
           sNatStat[1], sNatStat[2], sNatStat[3], sNatStat[4], sNatStat[5], (unsigned) (sNatArenaHigh >> 10));
}
#else
#define NAT_STAT(i) ((void) 0)
#endif

static Gfx __attribute__((aligned(8))) sNatOne[4];   // one command (up to three words) and an end

#ifdef TARGET_PC_ENDJINN
#define nat_memalign(a, n) aligned_alloc(a, n)
#else
#define nat_memalign(a, n) memalign(a, n)
#endif

void gfx_pvr_header_for(int kind, pvr_poly_hdr_t *out);
void gfx_pvr_native_header(int kind, const pvr_poly_hdr_t *hdr);
void gfx_pvr_native_end(void);

// ---- Where display lists live ----------------------------------------------------------------

// Display lists built at run time but never changed after (goddard's head shapes, built at its
// start): the game names them with gfx_native_stable and they compile as the program's own do.
// Their vertices may change (goddard moves them every frame), so they are read when drawn.
#define NAT_STABLE_MAX 64
static struct { uintptr_t lo, hi; } sNatStable[NAT_STABLE_MAX];
static uint32_t sNatStableCount;

void gfx_native_stable(const void *start, size_t bytes) {
    if (sNatStableCount < NAT_STABLE_MAX) {
        sNatStable[sNatStableCount].lo = (uintptr_t) start;
        sNatStable[sNatStableCount].hi = (uintptr_t) start + bytes;
        sNatStableCount++;
    }
}

static int nat_is_stable(const void *p) {
    for (uint32_t i = 0; i < sNatStableCount; i++) {
        if ((uintptr_t) p >= sNatStable[i].lo && (uintptr_t) p < sNatStable[i].hi) return 1;
    }
    return 0;
}

#if defined(TARGET_PC_ENDJINN) && defined(__APPLE__)
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
static struct { uintptr_t lo, hi; } sNatStatic[3];
static void nat_static_init(void) {
    static const char *const seg[3][2] = { { "__DATA", "__data" }, { "__DATA_CONST", "__const" }, { "__TEXT", "__const" } };
    for (int i = 0; i < 3; i++) {
        unsigned long size = 0;
        uint8_t *p = getsectiondata(&_mh_execute_header, seg[i][0], seg[i][1], &size);
        sNatStatic[i].lo = (uintptr_t) p;
        sNatStatic[i].hi = (uintptr_t) p + size;
    }
}
static inline int nat_is_static(const void *p) {
    uintptr_t a = (uintptr_t) p;
    for (int i = 0; i < 3; i++) {
        if (a >= sNatStatic[i].lo && a < sNatStatic[i].hi) return 1;
    }
    return 0;
}
#elif defined(TARGET_PC_ENDJINN)
extern char __executable_start[], edata[];
static void nat_static_init(void) {}
static inline int nat_is_static(const void *p) {
    return (const char *) p >= __executable_start && (const char *) p < edata;
}
#else
// The program's code and data, before its zeroed data (.bss) and the heap where the game builds
// its display lists each frame.
// (Linker symbols: the SH ABI prefixes C names with an underscore.)
extern char nat_image_start[] __asm__("_start");
extern char nat_bss_start[] __asm__("__bss_start");
static void nat_static_init(void) {}
static inline int nat_is_static(const void *p) {
    return (const char *) p >= nat_image_start && (const char *) p < nat_bss_start;
}
#endif

// ---- State groups ------------------------------------------------------------------------------

static void nat_state_get(NatState *s) {
    s->oml = rdp.other_mode_l;
    s->omh = rdp.other_mode_h;
    s->geom = rsp.geometry_mode;
    s->cmode = rdp.combine_mode;
    s->w0 = rdp.combine_w0;
    s->w1 = rdp.combine_w1;
    s->timg_addr = rdp.texture_to_load.addr;
    s->timg_siz = rdp.texture_to_load.siz;
    s->timg_width = last_set_texture_image_width;
    s->ltex_addr = rdp.loaded_texture.addr;
    s->ltex_size = rdp.loaded_texture.size_bytes;
    s->tile = rdp.texture_tile;
    s->s_scale = rsp.s_scale;
    s->t_scale = rsp.t_scale;
    memcpy(s->lights, rsp.current_lights, sizeof(s->lights));
    s->num_lights = rsp.current_num_lights;
    s->fog_mul = rsp.fog_mul;
    s->fog_offset = rsp.fog_offset;
    s->env = rdp.env_color;
    s->prim = rdp.prim_color;
    s->fogcol = rdp.fog_color;
    s->fill = rdp.fill_color;
}

// The caller's state (read where it lives, rdp and rsp) matches what v was compiled in.
static int nat_sig_match(const NatVariant *v) {
    const uint32_t m = v->sig_mask;
    const NatState *g = &v->sig;
    if (v->lc_in && !rsp.lights_changed) return 0;
    if (v->ortho != (proj_is_ortho != 0)) return 0;
    if (((rdp.other_mode_l ^ g->oml) & v->sig_oml) || ((rdp.other_mode_h ^ g->omh) & v->sig_omh) ||
        ((rsp.geometry_mode ^ g->geom) & v->sig_geom)) {
        return 0;
    }
    if (m == 0) return 1;
    if ((m & NG_COMBINE) && (rdp.combine_w0 != g->w0 || rdp.combine_w1 != g->w1 || rdp.combine_mode != g->cmode)) {
        return 0;
    }
    if (m & NG_TEX) {
        if (rdp.texture_to_load.addr != g->timg_addr || rdp.texture_to_load.siz != g->timg_siz ||
            last_set_texture_image_width != g->timg_width || rdp.loaded_texture.addr != g->ltex_addr ||
            rdp.loaded_texture.size_bytes != g->ltex_size ||
            rdp.texture_tile.fmt != g->tile.fmt || rdp.texture_tile.siz != g->tile.siz ||
            rdp.texture_tile.cms != g->tile.cms || rdp.texture_tile.cmt != g->tile.cmt ||
            rdp.texture_tile.uls != g->tile.uls || rdp.texture_tile.ult != g->tile.ult ||
            rdp.texture_tile.lrs != g->tile.lrs || rdp.texture_tile.lrt != g->tile.lrt ||
            rdp.texture_tile.line_size_bytes != g->tile.line_size_bytes) {
            return 0;
        }
    }
    if ((m & NG_SCALE) && (rsp.s_scale != g->s_scale || rsp.t_scale != g->t_scale)) return 0;
    if (m & NG_LIGHTS) {
        if ((v->sig_lights & 0x80) && rsp.current_num_lights != g->num_lights) return 0;
        for (int i = 0; i <= MAX_LIGHTS; i++) {
            if ((v->sig_lights & (1 << i)) && memcmp(&rsp.current_lights[i], &g->lights[i], sizeof(Light_t)) != 0) {
                return 0;
            }
        }
    }
    if ((m & NG_FOG) && (rsp.fog_mul != g->fog_mul || rsp.fog_offset != g->fog_offset)) return 0;
    return 1;
}

// Leave the state as the list leaves it.
static void nat_state_exit(const NatVariant *v) {
    const uint32_t m = v->exit_mask;
    const NatState *e = &v->exit;
    rdp.other_mode_l = (rdp.other_mode_l & ~v->exit_oml) | (e->oml & v->exit_oml);
    rdp.other_mode_h = (rdp.other_mode_h & ~v->exit_omh) | (e->omh & v->exit_omh);
    rsp.geometry_mode = (rsp.geometry_mode & ~v->exit_geom) | (e->geom & v->exit_geom);
    if (m & NG_COMBINE) {
        rdp.combine_mode = e->cmode;
        rdp.combine_w0 = e->w0;
        rdp.combine_w1 = e->w1;
    }
    if (m & NG_TEX) {
        if (v->exit_tex & NTW_TIMG) {
            rdp.texture_to_load.addr = e->timg_addr;
            rdp.texture_to_load.siz = e->timg_siz;
            last_set_texture_image_width = e->timg_width;
        }
        if (v->exit_tex & NTW_LOAD) {
            rdp.loaded_texture.addr = e->ltex_addr;
            rdp.loaded_texture.size_bytes = e->ltex_size;
        }
        if (v->exit_tex & NTW_TILE) {
            rdp.texture_tile.fmt = e->tile.fmt;
            rdp.texture_tile.siz = e->tile.siz;
            rdp.texture_tile.cms = e->tile.cms;
            rdp.texture_tile.cmt = e->tile.cmt;
            rdp.texture_tile.line_size_bytes = e->tile.line_size_bytes;
        }
        if (v->exit_tex & NTW_SIZE) {
            rdp.texture_tile.uls = e->tile.uls;
            rdp.texture_tile.ult = e->tile.ult;
            rdp.texture_tile.lrs = e->tile.lrs;
            rdp.texture_tile.lrt = e->tile.lrt;
        }
    }
    if (m & NG_SCALE) {
        rsp.s_scale = e->s_scale;
        rsp.t_scale = e->t_scale;
    }
    if (m & NG_LIGHTS) {
        memcpy(rsp.current_lights, e->lights, sizeof(e->lights));
        rsp.current_num_lights = e->num_lights;
    }
    if (m & NG_FOG) {
        rsp.fog_mul = e->fog_mul;
        rsp.fog_offset = e->fog_offset;
    }
    if (m & NG_ENV) rdp.env_color = e->env, env_a = e->env.a;
    if (m & NG_PRIM) rdp.prim_color = e->prim;
    if (m & NG_FOGCOL) rdp.fog_color = e->fogcol;
    if (m & NG_FILL) rdp.fill_color = e->fill;
}

// ---- Recording ---------------------------------------------------------------------------------

static inline void nat_read(uint32_t groups) {
    sRec.read |= groups & ~sRec.written;
}

static inline void nat_emit(uintptr_t w) {
    if (sRec.nops < NAT_REC_WORDS) {
        sRec.ops[sRec.nops++] = w;
    } else {
        sRec.fail = 1;
    }
}

// Does the combiner read the primitive (want_prim) or environment colour?
static int nat_cc_reads(uint32_t w0, uint32_t w1, int want_prim) {
    const int c4 = want_prim ? 3 : 5, c5a = want_prim ? 10 : 12;
    int a = (w0 >> 20) & 0xF, b = (w1 >> 28) & 0xF, c = (w0 >> 15) & 0x1F, d = (w1 >> 15) & 0x7;
    int aa = (w0 >> 12) & 0x7, ab = (w1 >> 12) & 0x7, ac = (w0 >> 9) & 0x7, ad = (w1 >> 9) & 0x7;
    return a == c4 || b == c4 || c == c4 || c == c5a || d == c4 || aa == c4 || ab == c4 || ac == c4 || ad == c4;
}

// A new chunk starts at a vertex load that follows triangles.
static void nat_rec_chunk(void) {
    if (sRec.nchunks == 0 || sRec.chunk_has_tri) {
        if (sRec.nchunks >= NAT_REC_CHUNKS) {
            sRec.fail = 1;
            return;
        }
        sRec.chunk_op[sRec.nchunks] = sRec.nops;
        sRec.chunk_merge[sRec.nchunks] = (int16_t) sRec.nchunks;
        sRec.nchunks++;
        sRec.chunk_has_tri = 0;
    }
}

static void nat_rec_vtx(size_t n, size_t dest, const Vtx *v) {
    if (dest + n > MAX_VERTICES || sRec.nloads >= NAT_REC_LOADS) {
        sRec.fail = 1;
        return;
    }
    if (!nat_is_static(v)) sRec.dyn_vtx = 1;
    const uint32_t geom = rsp.geometry_mode;
    sRec.geom_read |= NAT_VTX_GEOM & ~sRec.geom_w;
    NatLoad *l = &sRec.loads[sRec.nloads];
    l->geom = geom;
    l->s_scale = rsp.s_scale;
    l->t_scale = rsp.t_scale;
    l->scale_set = (sRec.written & NG_SCALE) != 0;
    l->fog_mul = rsp.fog_mul;
    l->fog_offset = rsp.fog_offset;
    l->lights = -1;
    if (geom & G_FOG) nat_read(NG_FOG);
    if (geom & G_LIGHTING) {
        // The lights the vertices are lit with: all set by the list, or taken from its caller.
        uint8_t need = 0x80;
        for (int i = 0; i < rsp.current_num_lights && i <= MAX_LIGHTS; i++) need |= 1 << i;
        l->lights_in = need & ~sRec.light_w;
        NatLights *last = sRec.nlights ? &sRec.lights[sRec.nlights - 1] : NULL;
        if (last == NULL || last->num != rsp.current_num_lights ||
            memcmp(last->lights, rsp.current_lights, sizeof(last->lights)) != 0) {
            if (sRec.nlights >= NAT_REC_LIGHTS) {
                sRec.fail = 1;
                return;
            }
            last = &sRec.lights[sRec.nlights++];
            memcpy(last->lights, rsp.current_lights, sizeof(last->lights));
            last->num = rsp.current_num_lights;
        }
        l->lights = (int16_t) (last - sRec.lights);
        l->dirs = sRec.lc;
        if (sRec.lc) {
            if (!sRec.lc_touched) sRec.lc_in = 1;
            sRec.lc = 0;
            sRec.lc_touched = 1;
        } else if (!sRec.lc_touched) {
            sRec.soft = sRec.fail = 1;   // directions taken before the list was called
        }
    }
    nat_rec_chunk();
    for (size_t i = 0; i < n; i++) {
        sRec.slot_chunk[dest + i] = (int16_t) (sRec.nchunks - 1);
        sRec.slot_load[dest + i] = (int16_t) sRec.nloads;
    }
    sRec.nloads++;
    sRec.tri_op = ~0u;
    nat_emit(NOP_VTX | (uint32_t) n << 8 | (uint32_t) dest << 16);
    nat_emit((uintptr_t) v);
    nat_emit(sRec.nloads - 1);
}

// The segment the next triangle draws in: the current state, set up as the interpreter would.
static int nat_rec_seg(void) {
    if (sTri.epoch == sTriStateEpoch && sRec.cur_seg >= 0) return sRec.cur_seg;
    if (sTri.epoch != sTriStateEpoch) tri_state_setup();
    sRec.om_read_l |= ~sRec.om_w_l;
    sRec.om_read_h |= NAT_TRI_OMH & ~sRec.om_w_h;
    sRec.geom_read |= NAT_TRI_GEOM & ~sRec.geom_w;
    nat_read(NG_COMBINE);
    if (sTri.usetex && (sRec.tex_w & NTW_ALL) != NTW_ALL) sRec.read |= NG_TEX;

    NatSeg s;
    memset(&s, 0, sizeof(s));
    s.kind = pvr_cur_kind;
    gfx_pvr_header_for(s.kind, &s.hdr);
    memcpy(&s.tri, &sTri, sizeof(sTri));   // with its padding, for the comparison below
    s.tri.epoch = 0;
    s.geom = rsp.geometry_mode;
    s.w0 = rdp.combine_w0;
    s.w1 = rdp.combine_w1;
    s.uls = rdp.texture_tile.uls;
    s.ult = rdp.texture_tile.ult;
    s.env = rdp.env_color;
    s.prim = rdp.prim_color;
    if (!sTri.cc.replace) {
        if (!(sRec.written & NG_ENV) && nat_cc_reads(s.w0, s.w1, 0)) s.cc_live |= NG_ENV;
        if (!(sRec.written & NG_PRIM) && nat_cc_reads(s.w0, s.w1, 1)) s.cc_live |= NG_PRIM;
    }
    // The same state as the last segment (a sync or a repeated command between): keep that one.
    if (sRec.cur_seg >= 0 && memcmp(&sRec.segs[sRec.cur_seg], &s, sizeof(s)) == 0) {
        return sRec.cur_seg;
    }
    if (sRec.nsegs >= NAT_REC_SEGS) {
        sRec.fail = 1;
        return 0;
    }
    sRec.segs[sRec.nsegs] = s;
    sRec.cur_seg = (int) sRec.nsegs++;
    return sRec.cur_seg;
}

static void nat_rec_tri(uint8_t a, uint8_t b, uint8_t c) {
    const uint8_t idx[3] = { a, b, c };
    const int seg = nat_rec_seg();
    if (sRec.fail) return;
    const int cur = (int) sRec.nchunks - 1;
    for (int i = 0; i < 3; i++) {
        int16_t ch = idx[i] < MAX_VERTICES ? sRec.slot_chunk[idx[i]] : -1;
        if (ch < 0) {   // a vertex loaded before the list was called
            sRec.fail = 1;
            return;
        }
        if (ch < sRec.chunk_merge[cur]) sRec.chunk_merge[cur] = ch;
        const NatLoad *ld = &sRec.loads[sRec.slot_load[idx[i]]];
        if (sTri.usetex && !ld->scale_set) nat_read(NG_SCALE);
        // The caller's lights reach the image only through a combiner that reads the shade.
        if (ld->lights_in && !sTri.cc.all_const && !sTri.cc.replace) {
            sRec.read |= NG_LIGHTS;
            sRec.light_read |= ld->lights_in;
        }
    }
    sRec.chunk_has_tri = 1;
    if (sRec.tri_op == ~0u || (sRec.ops[sRec.tri_op] >> 16) != (uint32_t) seg ||
        ((sRec.ops[sRec.tri_op] >> 8) & 0xFF) == 0xFF) {
        sRec.tri_op = sRec.nops;
        nat_emit(NOP_TRI | (uint32_t) seg << 16);
    }
    sRec.ops[sRec.tri_op] += 1u << 8;
    nat_emit(a | (uint32_t) b << 8 | (uint32_t) c << 16);
}

// What a command does to the recording, before it runs.
static void nat_rec_cmd(const Gfx *cmd) {
    const uint32_t opcode = cmd->words.w0 >> 24;
    if (cmd->words.w0 == 0x424C4E44) {   // trilerp marker
        sRec.fail = 1;
        return;
    }
    switch (opcode) {
        case G_VTX:
            nat_rec_vtx(C0(12, 8), C0(1, 7) - C0(12, 8), (const Vtx *) seg_addr(cmd->words.w1));
            break;
        case (uint8_t) G_TRI1:
            nat_rec_tri(C0(17, 7), C0(9, 7), C0(1, 7));
            break;
        case (uint8_t) G_TRI2:
            nat_rec_tri(C0(17, 7), C0(9, 7), C0(1, 7));
            nat_rec_tri(C1(17, 7), C1(9, 7), C1(1, 7));
            break;
        case G_GEOMETRYMODE: {
            uint32_t bits = ~C0(0, 24) | cmd->words.w1;
            sRec.geom_w |= bits;
            sRec.tri_op = ~0u;
            break;
        }
        case (uint8_t) G_SETOTHERMODE_L:
        case (uint8_t) G_SETOTHERMODE_H: {
            uint32_t shift = (opcode == (uint8_t) G_SETOTHERMODE_L ? 31 : 63) - C0(8, 8) - C0(0, 8);
            uint32_t len = C0(0, 8) + 1;
            uint64_t mask = (((uint64_t) 1 << len) - 1) << shift;
            sRec.om_w_l |= (uint32_t) mask;
            sRec.om_w_h |= (uint32_t) (mask >> 32);
            sRec.written |= NG_OTHERMODE;
            break;
        }
        case G_SETCOMBINE:
            sRec.written |= NG_COMBINE;
            break;
        case G_SETTIMG:
            sRec.tex_w |= NTW_TIMG;
            sRec.written |= NG_TEX;
            break;
        case G_LOADBLOCK:
        case G_LOADTILE:
            if (!(sRec.tex_w & NTW_TIMG)) sRec.read |= NG_TEX;   // loads the caller's image
            sRec.tex_w |= NTW_LOAD | (opcode == G_LOADTILE ? NTW_SIZE : 0);
            sRec.written |= NG_TEX;
            break;
        case G_SETTILE:
            if (C1(24, 3) == G_TX_RENDERTILE) sRec.tex_w |= NTW_TILE;
            sRec.written |= NG_TEX;
            break;
        case G_SETTILESIZE:
            sRec.tex_w |= NTW_SIZE;
            sRec.written |= NG_TEX;
            break;
        case (uint8_t) G_TEXTURE:
            sRec.written |= NG_SCALE;
            break;
        case G_SETENVCOLOR:
            sRec.written |= NG_ENV;
            break;
        case G_SETPRIMCOLOR:
            sRec.written |= NG_PRIM;
            break;
        case G_SETFOGCOLOR:
            sRec.written |= NG_FOGCOL;
            if (sRec.nfogcols < 16) sRec.fogcols[sRec.nfogcols++] = cmd->words.w1;
            else sRec.fail = 1;
            break;
        case G_SETFILLCOLOR:
            sRec.written |= NG_FILL;
            break;
        case G_MOVEMEM:
            if (C0(0, 8) == G_MV_LIGHT) {
                int lightidx = (int) (C0(8, 8) * 8) / 24 - 2;
                if (lightidx >= 0 && lightidx <= MAX_LIGHTS) {
                    sRec.light_w |= 1 << lightidx;
                    sRec.written |= NG_LIGHTS;
                } else {
                    sRec.fail = 1;   // a lookat: always the caller's (geo_append_display_list)
                }
            } else {
                sRec.fail = 1;   // a viewport
            }
            break;
        case (uint8_t) G_MOVEWORD:
            if (C0(16, 8) == G_MW_NUMLIGHT) {
                sRec.light_w |= 0x80;
                sRec.written |= NG_LIGHTS;
                sRec.lc = 1;
                sRec.lc_touched = 1;
            } else if (C0(16, 8) == G_MW_FOG) {
                sRec.written |= NG_FOG;
            }
            break;
        case (uint8_t) G_NOOP:
            if (cmd->words.w1) sRec.fail = 1;   // a Dreamcast tag (gfx_dc_tags.h)
            break;
        case G_MTX:
        case (uint8_t) G_POPMTX:
        case G_TEXRECT:
        case G_TEXRECTFLIP:
        case G_FILLRECT:
        case G_SETSCISSOR:
        case G_SETZIMG:
        case G_SETCIMG:
            sRec.fail = 1;
            break;
        default:
            break;
    }
}

static void *nat_alloc(uint32_t bytes) {
    bytes = (bytes + 31) & ~31u;
    if (sNatArenaUsed + bytes > NAT_ARENA_BYTES) return NULL;
    void *p = sNatArena + sNatArenaUsed;
    sNatArenaUsed += bytes;
    if (sNatArenaUsed > sNatArenaHigh) sNatArenaHigh = sNatArenaUsed;
    return p;
}

// Bounds of the vertices of loads [first, last) of the stream.
static void nat_sphere(const uintptr_t *ops, uint32_t first, uint32_t last, float out[4]) {
    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t i = first; i < last;) {
        uint32_t op = ops[i] & 0xFF;
        if (op == NOP_VTX) {
            const Vtx *v = (const Vtx *) ops[i + 1];
            for (uint32_t k = 0, n = (ops[i] >> 8) & 0xFF; k < n; k++) {
                for (int j = 0; j < 3; j++) {
                    float p = (float) v[k].v.ob[j];
                    if (p < lo[j]) lo[j] = p;
                    if (p > hi[j]) hi[j] = p;
                }
            }
            i += 3;
        } else {
            i += 1 + ((ops[i] >> 8) & 0xFF);   // NOP_TRI
        }
    }
    float c[3], r2 = 0.0f;
    for (int j = 0; j < 3; j++) c[j] = (lo[j] + hi[j]) * 0.5f;
    for (uint32_t i = first; i < last;) {
        uint32_t op = ops[i] & 0xFF;
        if (op == NOP_VTX) {
            const Vtx *v = (const Vtx *) ops[i + 1];
            for (uint32_t k = 0, n = (ops[i] >> 8) & 0xFF; k < n; k++) {
                float dx = v[k].v.ob[0] - c[0], dy = v[k].v.ob[1] - c[1], dz = v[k].v.ob[2] - c[2];
                float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 > r2) r2 = d2;
            }
            i += 3;
        } else {
            i += 1 + ((ops[i] >> 8) & 0xFF);
        }
    }
    out[0] = c[0];
    out[1] = c[1];
    out[2] = c[2];
    out[3] = lo[0] > hi[0] ? -1.0f : sqrtf(r2) + 1.0f;
}

// The recording as a variant in the arena: fog colours first, then the chunks with their bounds.
static NatVariant *nat_finalize(void) {
    const int cull = !sRec.dyn_vtx;
    // Loads with a slot no later load of the list overwrites: the next list may draw with them.
    {
        uint64_t later = 0;
        static uint32_t vtx_at[NAT_REC_LOADS];
        uint32_t nv = 0;
        for (uint32_t i = 0; i < sRec.nops;) {
            if ((sRec.ops[i] & 0xFF) == NOP_VTX) {
                vtx_at[nv++] = i;
                i += 3;
            } else {
                i += 1 + ((sRec.ops[i] >> 8) & 0xFF);
            }
        }
        while (nv--) {
            uintptr_t *w = &sRec.ops[vtx_at[nv]];
            uint64_t slots = (((uint64_t) 1 << ((*w >> 8) & 0xFF)) - 1) << ((*w >> 16) & 0xFF);
            if (slots & ~later) *w |= NAT_LIVE;
            later |= slots;
        }
    }
    // Chunks whose triangles reach back into earlier chunks' loads are drawn (or skipped) together.
    static int16_t lead[NAT_REC_CHUNKS];
    for (uint32_t k = 0; k < sRec.nchunks; k++) {
        lead[k] = (int16_t) k;
        int m = sRec.chunk_merge[k];
        if (m < (int) k) {
            for (uint32_t j = (uint32_t) m; j <= k; j++) lead[j] = lead[m];
        }
    }
    uint32_t ngroups = 0;
    for (uint32_t k = 0; k < sRec.nchunks; k++) ngroups += lead[k] == (int16_t) k;

    const uint32_t words = 2 * sRec.nfogcols + sRec.nops + 2 * ngroups + 1;
    NatVariant *v = nat_alloc(sizeof(NatVariant));
    NatSeg *segs = nat_alloc(sRec.nsegs * sizeof(NatSeg));
    NatLoad *loads = nat_alloc(sRec.nloads * sizeof(NatLoad));
    NatLights *lights = nat_alloc(sRec.nlights * sizeof(NatLights) + 4);
    float (*spheres)[4] = nat_alloc(ngroups * sizeof(*spheres) + 4);
    uintptr_t *ops = nat_alloc(words * sizeof(uintptr_t));
    if (v == NULL || segs == NULL || loads == NULL || lights == NULL || spheres == NULL || ops == NULL) return NULL;

    memcpy(segs, sRec.segs, sRec.nsegs * sizeof(NatSeg));
    memcpy(loads, sRec.loads, sRec.nloads * sizeof(NatLoad));
    memcpy(lights, sRec.lights, sRec.nlights * sizeof(NatLights));
    uint32_t o = 0;
    for (uint32_t i = 0; i < sRec.nfogcols; i++) {
        ops[o++] = NOP_FOGCOL;
        ops[o++] = sRec.fogcols[i];
    }
    uint32_t nsph = 0;
    for (uint32_t k = 0; k < sRec.nchunks;) {
        uint32_t end = k + 1;
        while (end < sRec.nchunks && lead[end] == lead[k]) end++;
        uint32_t first = sRec.chunk_op[k], last = end < sRec.nchunks ? sRec.chunk_op[end] : sRec.nops;
        ops[o] = NOP_CHUNK | (uintptr_t) (last - first) << 8;
        ops[o + 1] = nsph;
        nat_sphere(sRec.ops, first, last, spheres[nsph]);
        if (!cull) spheres[nsph][3] = -1.0f;
        nsph++;
        o += 2;
        memcpy(&ops[o], &sRec.ops[first], (last - first) * sizeof(uintptr_t));
        for (uint32_t i = o; i < o + (last - first);) {
            const uint32_t w = (uint32_t) ops[i];
            if ((w & 0xFF) != NOP_VTX) {
                i += 1 + ((w >> 8) & 0xFF);
                continue;
            }
            const NatLoad *l = &sRec.loads[ops[i + 2]];
            const Vtx *src = (const Vtx *) ops[i + 1];
            const uint32_t n = (w >> 8) & 0xFF;
            NatVtx *cv = (l->geom & G_TEXTURE_GEN) || !nat_is_static(src) ? NULL : nat_alloc(n * sizeof(NatVtx));
            if (cv != NULL) {
                for (uint32_t k2 = 0; k2 < n; k2++) {
                    const Vtx_t *t = &src[k2].v;
                    cv[k2].x = (float) t->ob[0];
                    cv[k2].y = (float) t->ob[1];
                    cv[k2].z = (float) t->ob[2];
                    short U = t->tc[0] * l->s_scale >> 16;
                    short V = t->tc[1] * l->t_scale >> 16;
                    cv[k2].u = U;
                    cv[k2].v = V;
                    if (l->geom & G_LIGHTING) {
                        const Vtx_tn *tn = &src[k2].n;
                        cv[k2].ln.n[0] = tn->n[0];
                        cv[k2].ln.n[1] = tn->n[1];
                        cv[k2].ln.n[2] = tn->n[2];
                        cv[k2].ln.a = tn->a;
                    } else {
                        cv[k2].c.r = t->cn[0];
                        cv[k2].c.g = t->cn[1];
                        cv[k2].c.b = t->cn[2];
                        cv[k2].c.a = t->cn[3];
                    }
                }
                ops[i] |= NAT_CONV;
                ops[i + 1] = (uintptr_t) cv;
            }
            i += 3;
        }
        o += last - first;
        k = end;
    }
    ops[o++] = NOP_END;

    memset(v, 0, sizeof(*v));
    v->sig_mask = sRec.read & ~(NG_OTHERMODE | NG_GEOM | NG_ENV | NG_PRIM);
    v->sig_oml = sRec.om_read_l;
    v->sig_omh = sRec.om_read_h;
    v->sig_geom = sRec.geom_read;
    v->sig = sRec.entry;
    v->exit_mask = sRec.written & ~(NG_OTHERMODE | NG_GEOM);
    v->exit_oml = sRec.om_w_l;
    v->exit_omh = sRec.om_w_h;
    v->exit_geom = sRec.geom_w;
    v->exit_tex = sRec.tex_w;
    v->lc_in = sRec.lc_in;
    v->sig_lights = sRec.light_read;
    v->ortho = proj_is_ortho != 0;
    v->lc_out = sRec.lc_touched ? (int8_t) sRec.lc : -1;
    nat_state_get(&v->exit);
    nat_sphere(sRec.ops, 0, sRec.nops, v->sphere);
    if (!cull) v->sphere[3] = -1.0f;
    v->segs = segs;
    v->loads = loads;
    v->lights = lights;
    v->spheres = (const float (*)[4]) spheres;
    v->ops = ops;
    return v;
}

// Run dl through the interpreter, a command at a time, recording it. Returns its variant, or NULL
// when it cannot be compiled (it has still been drawn).
static NatVariant *nat_record(Gfx *dl) {
    Gfx *stack[GFX_DL_STACK_MAX];
    int sp = 0;
    memset(&sRec, 0, offsetof(__typeof__(sRec), ops));
    sRec.tri_op = ~0u;
    sRec.cur_seg = -1;
    sRec.lc = rsp.lights_changed;
    sRec.nops = sRec.nsegs = sRec.nloads = sRec.nlights = sRec.nfogcols = sRec.nchunks = 0;
    sRec.chunk_has_tri = 0;
    for (int i = 0; i < MAX_VERTICES + 4; i++) sRec.slot_chunk[i] = -1;
    nat_state_get(&sRec.entry);
    sRec.active = 1;

    for (Gfx *cmd = dl;;) {
        const uint32_t opcode = cmd->words.w0 >> 24;
        if (opcode == G_DL) {
            Gfx *to = (Gfx *) seg_addr(cmd->words.w1);
            if (!nat_is_static(to) && !nat_is_stable(to)) {
#ifdef TARGET_PC_ENDJINN
                if (!sRec.fail && getenv("SM64_NAT_FAIL") != NULL) {
                    printf("NATFAIL %s (%p) calls %p, built at run time\n", nat_name(dl), (void *) dl, (void *) to);
                }
#endif
                sRec.fail = 1;
            }
            if (C0(16, 1) == 0) {
                if (sp == GFX_DL_STACK_MAX) {
                    sRec.fail = 1;
                    break;
                }
                stack[sp++] = cmd + 1;
            }
            cmd = to;
            continue;
        }
        if (opcode == (uint8_t) G_ENDDL) {
            if (sp == 0) break;
            cmd = stack[--sp];
            continue;
        }
        // A rectangle is two or three words (gfx_run_dl), and never compiled.
        const int words = opcode == G_TEXRECT || opcode == G_TEXRECTFLIP ? 3 : opcode == G_FILLRECT ? 2 : 1;
        if (words > 1) sRec.fail = 1;
        if (!sRec.fail) {
            nat_rec_cmd(cmd);
#ifdef TARGET_PC_ENDJINN
            if (sRec.fail && getenv("SM64_NAT_FAIL") != NULL) {   // why a list does not compile
                printf("NATFAIL %s (%p) at %08x %08x\n", nat_name(dl), (void *) dl, cmd->words.w0, cmd->words.w1);
            }
#endif
        }
        for (int i = 0; i < words; i++) sNatOne[i] = cmd[i];
        sNatOne[words].words.w0 = (uint32_t) (uint8_t) G_ENDDL << 24;
        sNatOne[words].words.w1 = 0;
        gfx_run_dl(sNatOne);
        cmd += words;
    }
    sRec.active = 0;
    // Recording set up triangles the interpreter rejects unset, binding their textures.
    rdp.texture_changed = true;
    if (sRec.fail) return NULL;
    DC_PROF_COUNT(DCC_NAT_COMPILES, 1);
    NAT_STAT(1);
    return nat_finalize();
}

// ---- Drawing -----------------------------------------------------------------------------------

// The clip planes the interpreter rejects against (trivial_reject), as gradients in model space
// of x - w, -x - w, y - w, -y - w, z - w, -z - w, with their lengths.
static float sNatPlane[6][4];

static void nat_planes(void) {
    const float (*m)[4] = (const float (*)[4]) rsp.MP_matrix;
    static const int8_t axis[6] = { 0, 0, 1, 1, 2, 2 };
    for (int p = 0; p < 6; p++) {
        const float s = (p & 1) ? -1.0f : 1.0f;
        float g[3];
        for (int i = 0; i < 3; i++) g[i] = s * m[i][axis[p]] - m[i][3];
        sNatPlane[p][0] = g[0];
        sNatPlane[p][1] = g[1];
        sNatPlane[p][2] = g[2];
        sNatPlane[p][3] = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    }
}

// Where the sphere is against the clip planes, with a margin for rounding: 0 wholly outside one
// (every triangle in it would be rejected), 2 wholly inside all six, 1 across one.
#ifdef TARGET_PC_ENDJINN
static int sNatNoCull = -1;   // SM64_NAT_NOCULL=1: draw every chunk (host debugging)
#endif

static int __attribute__((noinline)) nat_classify(const float sp[4]) {
    if (sp[3] < 0.0f) return 1;
#ifdef TARGET_PC_ENDJINN
    if (sNatNoCull < 0) sNatNoCull = getenv("SM64_NAT_NOCULL") != NULL;
    if (sNatNoCull) return 1;
#endif
    const float (*m)[4] = (const float (*)[4]) rsp.MP_matrix;
    float c[4];
    for (int j = 0; j < 4; j++) c[j] = sp[0] * m[0][j] + sp[1] * m[1][j] + sp[2] * m[2][j] + m[3][j];
    const float e = 1e-3f * (fabsf(c[3]) + 1.0f);
    static const int8_t axis[6] = { 0, 0, 1, 1, 2, 2 };
    int inside = 1;
    for (int p = 0; p < 6; p++) {
        const float d = ((p & 1) ? -c[axis[p]] : c[axis[p]]) - c[3];   // > 0 outside
        const float rg = sp[3] * sNatPlane[p][3], margin = e * (1.0f + rg);
        if (d - rg > margin) return 0;
        if (d + rg > -margin) inside = 0;
    }
    return inside ? 2 : 1;
}

static void __attribute__((noinline)) nat_seg_begin(const NatSeg *s) {
    tri_state_bump();
    sTri = s->tri;
    sTri.epoch = sTriStateEpoch;
    sTri.cull_flip = (sm_xscale * sm_yscale) < 0.0f;
    if (s->cc_live) {
        // The caller's colours for what the list did not set, its own for the rest.
        struct RGBA env = rdp.env_color, prim = rdp.prim_color;
        if (!(s->cc_live & NG_ENV)) rdp.env_color = s->env;
        if (!(s->cc_live & NG_PRIM)) rdp.prim_color = s->prim;
        cc_compile_cached(&sTri.cc, s->w0, s->w1, s->tri.usetex, s->tri.texenv);
        rdp.env_color = env;
        rdp.prim_color = prim;
    }
    rsp.geometry_mode = s->geom;
    rdp.texture_tile.uls = s->uls;
    rdp.texture_tile.ult = s->ult;
    if (s->kind != pvr_cur_kind) {
        gfx_pvr_set_blend(s->kind);
        pvr_cur_kind = s->kind;
    }
    gfx_pvr_native_header(s->kind, &s->hdr);
}

static NatLights sNatLightsIn;   // the caller's lights while a draw lights with the list's

// gfx_sp_vertex_light and gfx_sp_vertex_no over converted vertices: the same results, without
// the conversions.
// The usual light set, one directional light and the ambient: the constants stay in registers.
// (The same arithmetic as nat_vertex_light, in the same order.)
static void SM_HOT __attribute__((noinline)) nat_vertex_light1(size_t n, size_t dest, const NatVtx *src) {
    const float amb_r = rsp.current_lights_colf[1][0];
    const float amb_g = rsp.current_lights_colf[1][1];
    const float amb_b = rsp.current_lights_colf[1][2];
    const float c0 = rsp.current_lights_coeffs[0][0], c1 = rsp.current_lights_coeffs[0][1];
    const float c2 = rsp.current_lights_coeffs[0][2];
    const float lr = rsp.current_lights_colf[0][0], lg = rsp.current_lights_colf[0][1];
    const float lb = rsp.current_lights_colf[0][2];
    const uint8_t fog = (rsp.geometry_mode & G_FOG) != 0;
    for (size_t i = 0; i < n; i++, dest++) {
        const NatVtx *v = &src[i];
        struct LoadedVertex *d = &rsp.loaded_vertices[dest];
        shz_vec4_t out = shz_xmtrx_transform_vec4(shz_vec4_init(v->x, v->y, v->z, 1.0f));
        __builtin_prefetch(v + 3);
        const float x = out.x, y = out.y, z = out.z, w = out.w;
        float r = amb_r, g = amb_g, b = amb_b;
        float intensity = shz_dot6f((float) v->ln.n[0], (float) v->ln.n[1], (float) v->ln.n[2], c0, c1, c2);
        if (intensity > 0.0f) {
            r += intensity * lr;
            g += intensity * lg;
            b += intensity * lb;
        }
        d->color.r = r >= 255.0f ? 255 : (uint8_t) r;
        d->color.g = g >= 255.0f ? 255 : (uint8_t) g;
        d->color.b = b >= 255.0f ? 255 : (uint8_t) b;
        d->color.a = v->ln.a;
        d->u = v->u;
        d->v = v->v;
        clip_rej[dest] = 128 | ((w < 0) ? 64 : 0x00) | trivial_reject(x, y, z, w);
        d->_x = x;
        d->_y = y;
        d->_z = z;
        d->_w = w;
        d->fog = fog ? gfx_calc_fog(z, w) : 0;
    }
}

static void __attribute__((noinline)) nat_vertex_light(size_t n, size_t dest, const NatVtx *src) {
    const int nl = rsp.current_num_lights - 1;
    float lcoef[MAX_LIGHTS][3], lcol[MAX_LIGHTS][3];
    const float amb_r = rsp.current_lights_colf[nl][0];
    const float amb_g = rsp.current_lights_colf[nl][1];
    const float amb_b = rsp.current_lights_colf[nl][2];
    for (int l = 0; l < nl; l++) {
        for (int c = 0; c < 3; c++) {
            lcoef[l][c] = rsp.current_lights_coeffs[l][c];
            lcol[l][c] = rsp.current_lights_colf[l][c];
        }
    }
    for (size_t i = 0; i < n; i++, dest++) {
        const NatVtx *v = &src[i];
        struct LoadedVertex *d = &rsp.loaded_vertices[dest];
        shz_vec4_t out = shz_xmtrx_transform_vec4(shz_vec4_init(v->x, v->y, v->z, 1.0f));
        __builtin_prefetch(v + 3);
        d->color.a = v->ln.a;
        const float x = out.x, y = out.y, z = out.z, w = out.w;
        float r = amb_r, g = amb_g, b = amb_b;
        const float nx = (float) v->ln.n[0], ny = (float) v->ln.n[1], nz = (float) v->ln.n[2];
        for (int l = 0; l < nl; l++) {
            float intensity = shz_dot6f(nx, ny, nz, lcoef[l][0], lcoef[l][1], lcoef[l][2]);
            if (intensity > 0.0f) {
                r += intensity * lcol[l][0];
                g += intensity * lcol[l][1];
                b += intensity * lcol[l][2];
            }
        }
        d->color.r = r >= 255.0f ? 255 : (uint8_t) r;
        d->color.g = g >= 255.0f ? 255 : (uint8_t) g;
        d->color.b = b >= 255.0f ? 255 : (uint8_t) b;
        d->u = v->u;
        d->v = v->v;
        clip_rej[dest] = 128 | ((w < 0) ? 64 : 0x00) | trivial_reject(x, y, z, w);
        d->_x = x;
        d->_y = y;
        d->_z = z;
        d->_w = w;
        d->fog = gfx_calc_fog(z, w);
    }
}

static void SM_HOT __attribute__((noinline)) nat_vertex_no(size_t n, size_t dest, const NatVtx *src) {
    for (size_t i = 0; i < n; i++, dest++) {
        const NatVtx *v = &src[i];
        struct LoadedVertex *d = &rsp.loaded_vertices[dest];
        shz_vec4_t out = shz_xmtrx_transform_vec4(shz_vec4_init(v->x, v->y, v->z, 1.0f));
        __builtin_prefetch(v + 3);
        const float x = out.x, y = out.y, z = out.z, w = out.w;
        d->color = v->c;
        d->u = v->u;
        d->v = v->v;
        clip_rej[dest] = ((w < 0) ? 64 : 0x00) | trivial_reject(x, y, z, w);
        d->_x = x;
        d->_y = y;
        d->_z = z;
        d->_w = w;
        d->fog = gfx_calc_fog(z, w);
    }
}

static void nat_vertex(size_t n, size_t dest, const NatVtx *src) {
    shz_xmtrx_load_4x4((const shz_mat4x4_t *) &rsp.MP_matrix);
    if (rsp.geometry_mode & G_LIGHTING) {
        gfx_vertex_lights_prep();
        if (rsp.current_num_lights == 2) {
            nat_vertex_light1(n, dest, src);
        } else {
            nat_vertex_light(n, dest, src);
        }
    } else {
        nat_vertex_no(n, dest, src);
    }
    DC_PROF_COUNT(DCC_VERTS, n);
    for (size_t i = 0; i < n; i++) {
        sVtxBakeEpoch[dest + i] = 0;
    }
}

// gfx_sp_vertex for a list that is interpreted: the vertices converted into a buffer as a compile
// would, then the same loops. (Texture-generating loads read the raw normal against the lookat:
// returns 0 for gfx_sp_vertex's own path.)
static int nat_vertex_from_vtx(size_t n, size_t dest, const Vtx *vertices) {
    static NatVtx __attribute__((aligned(32))) buf[MAX_VERTICES];
    if ((rsp.geometry_mode & G_TEXTURE_GEN) || n > MAX_VERTICES) return 0;
    const int lit = (rsp.geometry_mode & G_LIGHTING) != 0;
    for (size_t k = 0; k < n; k++) {
        const Vtx_t *t = &vertices[k].v;
        buf[k].x = (float) t->ob[0];
        buf[k].y = (float) t->ob[1];
        buf[k].z = (float) t->ob[2];
        short U = t->tc[0] * rsp.s_scale >> 16;
        short V = t->tc[1] * rsp.t_scale >> 16;
        buf[k].u = U;
        buf[k].v = V;
        if (lit) {
            const Vtx_tn *tn = &vertices[k].n;
            buf[k].ln.n[0] = tn->n[0];
            buf[k].ln.n[1] = tn->n[1];
            buf[k].ln.n[2] = tn->n[2];
            buf[k].ln.a = tn->a;
        } else {
            buf[k].c.r = t->cn[0];
            buf[k].c.g = t->cn[1];
            buf[k].c.b = t->cn[2];
            buf[k].c.a = t->cn[3];
        }
    }
    nat_vertex(n, dest, buf);
    return 1;
}

// Per draw: the load state last set, and whether the caller's lights were saved.
static struct {
    int load, lights, lights_saved;
} sNatCur;

// A load's state: its geometry mode, texture scale, fog and lights.
static void nat_load_state(const NatVariant *v, uint32_t li) {
    if ((int) li != sNatCur.load) {
        const NatLoad *l = &v->loads[li];
        sNatCur.load = (int) li;
        rsp.geometry_mode = l->geom;
        rsp.s_scale = l->s_scale;
        rsp.t_scale = l->t_scale;
        rsp.fog_mul = l->fog_mul;
        rsp.fog_offset = l->fog_offset;
        if (l->lights >= 0 && l->lights != sNatCur.lights) {
            const NatLights *nl = &v->lights[l->lights];
            if (!sNatCur.lights_saved) {
                memcpy(sNatLightsIn.lights, rsp.current_lights, sizeof(sNatLightsIn.lights));
                sNatLightsIn.num = rsp.current_num_lights;
                sNatCur.lights_saved = 1;
            }
            memcpy(rsp.current_lights, nl->lights, sizeof(nl->lights));
            rsp.current_num_lights = nl->num;
            rsp.light_cols_changed = 1;
            sNatCur.lights = l->lights;
        }
    }
    if (v->loads[li].lights >= 0) rsp.lights_changed = v->loads[li].dirs;
}

static void __attribute__((noinline)) nat_load(const NatVariant *v, const uintptr_t *op) {
    const uint32_t w = (uint32_t) op[0];
    nat_load_state(v, (uint32_t) op[2]);
    if (w & NAT_CONV) {
        nat_vertex((w >> 8) & 0xFF, (w >> 16) & 0xFF, (const NatVtx *) op[1]);
    } else {
        gfx_sp_vertex((w >> 8) & 0xFF, (w >> 16) & 0xFF, (const Vtx *) op[1]);
    }
}

// The stream's chunks, loads and triangles (nat_draw): the per-frame loop, in .sub0 with the
// triangle and vertex code it calls.
static void SM_HOT __attribute__((noinline)) nat_run(const NatVariant *v, const uintptr_t *op) {
    // A list wholly outside still loads the vertices the next list may draw with (a limb's
    // list draws triangles from its parent's vertices): every chunk counts as culled.
    const int list = nat_classify(v->sphere);
    const int all_culled = list == 0, all_in = list == 2;
    int cur_seg = -1;
    for (;;) {
        const uint32_t w = (uint32_t) *op;
        switch (w & 0xFF) {
            case NOP_CHUNK:
                if (all_culled || (!all_in && nat_classify(v->spheres[op[1]]) == 0)) {
                    DC_PROF_COUNT(DCC_NAT_CULLED, 1);
                    NAT_STAT(4);
                    // Skip the triangles; load what the next list may draw with.
                    const uintptr_t *end = op + 2 + (w >> 8);
                    for (op += 2; op < end;) {
                        const uint32_t x = (uint32_t) *op;
                        if ((x & 0xFF) == NOP_VTX) {
                            if (x & NAT_LIVE) {
                                nat_load(v, op);
                                if (cur_seg >= 0) rsp.geometry_mode = v->segs[cur_seg].geom;
                            } else if (v->loads[op[2]].lights >= 0) {
                                // Not loaded, but lit: the light directions the later loads keep.
                                nat_load_state(v, (uint32_t) op[2]);
                                gfx_vertex_lights_prep();
                                if (cur_seg >= 0) rsp.geometry_mode = v->segs[cur_seg].geom;
                            }
                            op += 3;
                        } else {
                            op += 1 + ((x >> 8) & 0xFF);
                        }
                    }
                } else {
                    DC_PROF_COUNT(DCC_NAT_CHUNKS, 1);
                    NAT_STAT(3);
                    op += 2;
                }
                continue;
            case NOP_VTX: {
                nat_load(v, op);
                if (cur_seg >= 0) rsp.geometry_mode = v->segs[cur_seg].geom;   // the triangles'
                op += 3;
                continue;
            }
            case NOP_TRI: {
                const int seg = (int) (w >> 16);
                if (seg != cur_seg) {
                    nat_seg_begin(&v->segs[seg]);
                    cur_seg = seg;
                }
                const uint32_t n = (w >> 8) & 0xFF;
                for (uint32_t i = 1; i <= n; i++) {
                    const uint32_t t = (uint32_t) op[i];
                    gfx_sp_tri1(t & 0xFF, (t >> 8) & 0xFF, (t >> 16) & 0xFF);
                }
                op += 1 + n;
                continue;
            }
            default:
                break;
        }
        break;
    }
}

static void __attribute__((noinline)) nat_draw(const NatVariant *v) {
    DC_PROF_COUNT(DCC_NAT_DRAWS, 1);
    NAT_STAT(0);
    if (rdp.viewport_or_scissor_changed) {
        // What tri_state_setup does first: the screen map the vertices bake with.
        if (memcmp(&rdp.viewport, &rendering_state.viewport, sizeof(rdp.viewport)) != 0) {
            n64_memcpy(&rendering_state.viewport, &rdp.viewport, sizeof(rdp.viewport));
            vpf_x = rdp.viewport.x; vpf_y = rdp.viewport.y;
            vpf_w = rdp.viewport.width; vpf_h = rdp.viewport.height;
            gfx_recompute_screen_map();
        }
        if (memcmp(&rdp.scissor, &rendering_state.scissor, sizeof(rdp.scissor)) != 0) {
            gfx_rapi->set_scissor(rdp.scissor.x, rdp.scissor.y, rdp.scissor.width, rdp.scissor.height);
            n64_memcpy(&rendering_state.scissor, &rdp.scissor, sizeof(rdp.scissor));
        }
        rdp.viewport_or_scissor_changed = false;
    }
    // What the draw sets that the list itself may not: put back before its exit state.
    const uint32_t geom_in = rsp.geometry_mode;
    const uint16_t s_in = rsp.s_scale, t_in = rsp.t_scale, uls_in = rdp.texture_tile.uls, ult_in = rdp.texture_tile.ult;
    const int16_t fog_mul_in = rsp.fog_mul, fog_offset_in = rsp.fog_offset;
    sNatCur.load = sNatCur.lights = -1;
    sNatCur.lights_saved = 0;
    const uintptr_t *op = v->ops;
    while ((*op & 0xFF) == NOP_FOGCOL) {
        uint32_t c = (uint32_t) op[1];
        gfx_dp_set_fog_color(c >> 24, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
        op += 2;
    }
    nat_planes();
    nat_run(v, op);
    gfx_pvr_native_end();
    rsp.geometry_mode = geom_in;
    rsp.s_scale = s_in;
    rsp.t_scale = t_in;
    rsp.fog_mul = fog_mul_in;
    rsp.fog_offset = fog_offset_in;
    rdp.texture_tile.uls = uls_in;
    rdp.texture_tile.ult = ult_in;
    if (sNatCur.lights_saved) {
        const int differ = rsp.current_num_lights != sNatLightsIn.num ||
                           memcmp(rsp.current_lights, sNatLightsIn.lights, sizeof(sNatLightsIn.lights)) != 0;
        memcpy(rsp.current_lights, sNatLightsIn.lights, sizeof(sNatLightsIn.lights));
        rsp.current_num_lights = sNatLightsIn.num;
        rsp.light_cols_changed = 1;
        if (differ && !(v->exit_mask & NG_LIGHTS) && v->lc_out == 0) {
            // The list lit with lights of its own compile, not the caller's (they did not show):
            // leave the directions the interpreter would have taken, from the caller's lights.
            rsp.lights_changed = 1;
            gfx_vertex_lights_prep();
        }
    }
    nat_state_exit(v);
    // The bound texture is the interpreter's last, not the list's: bind again on the next setup.
    rdp.texture_changed = true;
    if (v->exit_mask & NG_LIGHTS) rsp.light_cols_changed = 1;
    if (v->lc_out >= 0) rsp.lights_changed = (uint8_t) v->lc_out;
    tri_state_bump();
}

static int nat_init(void) {
    if (sNatOff) return 0;
    if (sNatArena == NULL) {
        nat_static_init();
#ifdef TARGET_PC_ENDJINN
        atexit(nat_stat_print);
#endif
        sNatArena = nat_memalign(32, NAT_ARENA_BYTES);
        sNatHash = nat_memalign(32, NAT_HASH * sizeof(NatEntry));
        if (sNatArena == NULL || sNatHash == NULL) {
            printf("NATIVE: no memory for the compiled display lists\n");
            sNatOff = 1;
            return 0;
        }
    }
    if (sNatCacheGen != sNatGen + 1) {
        memset(sNatHash, 0, NAT_HASH * sizeof(NatEntry));
        sNatArenaUsed = 0;
        sNatCacheGen = sNatGen + 1;
    }
    return 1;
}

// gfx_run_dl's G_DL: draw dl natively (compiling it first if need be). Returns 0 to interpret it.
static int __attribute__((noinline)) nat_try(Gfx *dl) {
    if (sRec.active || sCover || sSkybox || in_trilerp || !nat_init() ||
        !(nat_is_static(dl) || nat_is_stable(dl))) {
        return 0;
    }
    uint32_t h = (((uint32_t) (uintptr_t) dl >> 3) * 0x9E3779B1u) >> 20;
    NatEntry *e;
    for (;; h = (h + 1) & (NAT_HASH - 1)) {
        e = &sNatHash[h & (NAT_HASH - 1)];
        if (e->dl == dl) break;
        if (e->dl == NULL) {
            e->dl = dl;
            break;
        }
    }
    if (e->bad) return 0;
#ifdef TARGET_PC_ENDJINN
    {   // SM64_NAT_SKIP=name,name: interpret these lists (host debugging)
        static const char *skip;
        static int init;
        if (!init) {
            init = 1;
            skip = getenv("SM64_NAT_SKIP");
        }
        if (skip != NULL && strstr(skip, nat_name(dl)) != NULL) return 0;
    }
#endif
    for (int i = 0; i < e->nvar; i++) {
        if (nat_sig_match(e->var[i])) {
#ifdef TARGET_PC_ENDJINN
            if (nat_dbg()) {
                printf("NATDBG %s variant %d sig %x tex %p\n", nat_name(dl), i, e->var[i]->sig_mask,
                       (void *) rdp.loaded_texture.addr);
            }
#endif
            nat_draw(e->var[i]);
            return 1;
        }
    }
    if (e->nvar == NAT_MAX_VARIANTS) {
        NAT_STAT(5);
#ifdef TARGET_PC_ENDJINN
        if (getenv("SM64_NAT_FULL") != NULL) {   // which lists run out of variants, and on what
            const NatVariant *v0 = e->var[0];
            printf("NATFULL %s mask %x oml %08x/%08x omh %08x/%08x geom %08x/%08x lc %d\n", nat_name(dl), v0->sig_mask,
                   rdp.other_mode_l & v0->sig_oml, v0->sig.oml & v0->sig_oml, rdp.other_mode_h & v0->sig_omh,
                   v0->sig.omh & v0->sig_omh, rsp.geometry_mode & v0->sig_geom, v0->sig.geom & v0->sig_geom, v0->lc_in);
        }
#endif
        return 0;
    }
    NatVariant *v = nat_record(dl);
    if (v == NULL) {
        if (sRec.fail && !sRec.soft) {
            e->bad = 1;   // else the arena is full, or the state: maybe later
            NAT_STAT(2);
        }
        return 1;
    }
    e->var[e->nvar++] = v;
    return 1;
}
