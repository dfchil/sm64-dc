#ifdef DC_PROF

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arch/arch.h>
#include <arch/timer.h>
#include <dc/pvr.h>
#include <kos/thread.h>
#include <kos/irq.h>
#include <dc/perfctr.h>

#include "dc_prof.h"

#ifndef DC_AUTO_EXIT_SECONDS
#define DC_AUTO_EXIT_SECONDS 240
#endif
#ifndef DC_VBL_WAIT
#define DC_VBL_WAIT 2
#endif
#ifndef DC_FSAA
#define DC_FSAA 0
#endif

struct DemoInput;
extern int16_t gCurrLevelNum;
extern int16_t gCurrAreaIndex;
extern struct DemoInput *gCurrDemoInput;
extern volatile uint64_t vblticker;

// Components of one frame, in microseconds.
enum { C_TOTAL, C_LOGIC, C_GEO, C_DL, C_WAIT, C_FINISH, C_VSYNC, C_AUDIO, C_RENDER, C_COUNT };
static const char *const sCompNames[C_COUNT] = {
    "total", "logic", "geo", "dl", "wait", "finish", "vsync", "audio", "render"
};

typedef struct {
    uint16_t scene;   // level << 8 | area, bit 15 = demo playing
    uint8_t  fields;  // vblanks since the previous frame end
    uint8_t  vtx_kb;  // TA vertex buffer used by the last scene, KB (clamped)
    uint16_t us[C_COUNT];
} FrameRec;

// A sampling build gives the per-frame CSV's RAM to the samples: the image must stay under the
// 1 MB free-RAM guard (tools/dc_prof_run.zsh).
#ifdef DC_PROF_SAMPLE
#define MAX_FRAMES 8192
#else
#define MAX_FRAMES 16384
#endif
static FrameRec sFrames[MAX_FRAMES];
static uint32_t sFrameCount;
static uint32_t sFramesDropped;   // frames past MAX_FRAMES: in the scene stats, not the CSV

// Why a frame was late (DCPROF LATE lines, and the SCENE late_* counts). One field is 16.7 ms;
// the frame's own CPU work (logic + geo walk + translation) or the PVR (the wait for the previous
// render, or the render time itself) reaching a field's budget is what makes it late. A frame
// in a level load (a long level script, a translation that uploads every texture, or one of the
// first frames of a scene) is "load", whatever else it was.
enum { LATE_CPU, LATE_PVR, LATE_BOTH, LATE_LOAD, LATE_OTHER, LATE_CAUSES };
static const char *const sCauseNames[LATE_CAUSES] = { "cpu", "pvr", "both", "load", "other" };
#define FIELD_US 16667
#define LATE_BUDGET_US (DC_VBL_WAIT * FIELD_US - 1200)
#define LOAD_LOGIC_US 8000
#define LOAD_DL_US 30000
#define LOAD_SCENE_FRAMES 3

typedef struct {
    uint32_t frame, vbl;
    uint16_t scene, since_scene;
    uint8_t fields, cause;
    uint16_t logic, geo, dl, wait, render, audio, asset;
} LateRec;
#define MAX_LATE 2048
static LateRec sLate[MAX_LATE];
static uint32_t sLateCount, sLateDropped;
static uint16_t sPrevScene = 0xFFFF;

// Asset reads on the main thread (dc_prof_asset_load): each one, and their time in this frame.
typedef struct {
    uint32_t frame, us, bytes;
    uint16_t scene;
    uint8_t ok;
    char name[21];
} AssetRec;
#define MAX_ASSETS 512
static AssetRec sAssets[MAX_ASSETS];
static uint32_t sAssetCount, sAssetsDropped;
static uint32_t sFrameAssetUs;
static uint32_t sSinceScene;   // frames since the scene (level/area/demo) last changed

typedef struct {
    uint16_t scene;
    uint32_t frames, late;
    uint32_t late_by[LATE_CAUSES];
    uint32_t vram_min;   // least free texture VRAM seen in the scene, bytes
    uint64_t sum[C_COUNT];
    uint16_t max[C_COUNT];
} SceneStats;

#define MAX_SCENES 64
static SceneStats sScenes[MAX_SCENES];

unsigned int gDcProfCount[DCC_COUNT];
enum { PMU_CYCLES, PMU_DSTALL, PMU_ISTALL, PMU_INSNS, PMU_COUNT };
static uint64_t sSceneCounts[MAX_SCENES][DCC_COUNT];
static uint64_t sScenePmu[MAX_SCENES][PMU_COUNT];
static uint32_t sScenePmuFrames[MAX_SCENES][PMU_COUNT];
static const char *const sCountNames[DCC_COUNT] = { "tris", "fast", "bakes", "epochs", "setups", "verts",
    "op_tris", "op_px", "pt_tris", "pt_px", "pt_opq_px", "tr_tris", "tr_px", "tr_opq_tris", "tr_opq_px", "held", "dropped" };
static int sSceneCount;

#ifdef DC_PROF_LISTS
// What fills the PT and TR lists, by texture: each frame's batches are summed by (list, N64 texture
// address) and merged into a per-scene top table. A full table replaces its smallest entry, so the
// tail is approximate; the large entries are what matter.
typedef struct { uint32_t src; uint8_t kind; uint64_t px; uint32_t tris; } ListSrc;
#define LIST_SRC_FRAME 96
#define LIST_SRC_SCENE 24
static ListSrc sFrameSrc[LIST_SRC_FRAME];
static int sFrameSrcCount;
static ListSrc sSceneSrc[MAX_SCENES][LIST_SRC_SCENE];

void dc_prof_list_src(int kind, unsigned int src, unsigned int px, unsigned int tris) {
    for (int i = 0; i < sFrameSrcCount; i++) {
        if (sFrameSrc[i].src == src && sFrameSrc[i].kind == kind) {
            sFrameSrc[i].px += px;
            sFrameSrc[i].tris += tris;
            return;
        }
    }
    if (sFrameSrcCount < LIST_SRC_FRAME) {
        ListSrc *f = &sFrameSrc[sFrameSrcCount++];
        f->src = src;
        f->kind = (uint8_t) kind;
        f->px = px;
        f->tris = tris;
    }
}

static void list_src_merge(int scene) {
    ListSrc *t = sSceneSrc[scene];
    for (int i = 0; i < sFrameSrcCount; i++) {
        const ListSrc *f = &sFrameSrc[i];
        int slot = -1, min = 0;
        for (int j = 0; j < LIST_SRC_SCENE; j++) {
            if (t[j].tris && t[j].src == f->src && t[j].kind == f->kind) {
                slot = j;
                break;
            }
            if (t[j].px < t[min].px) {
                min = j;
            }
        }
        if (slot < 0) {
            if (t[min].tris && t[min].px > f->px) {
                continue;
            }
            slot = min;
            t[slot].src = f->src;
            t[slot].kind = f->kind;
            t[slot].px = 0;
            t[slot].tris = 0;
        }
        t[slot].px += f->px;
        t[slot].tris += f->tris;
    }
}
#endif

static uint64_t sMark[DCP_COUNT];
static uint64_t sFrameStart, sRunStart;
static uint64_t sLastVbl;
static volatile uint32_t sAudioUsTotal;   // written only by the audio thread
static volatile uint32_t sProgress;       // frames completed, read by the hang watchdog
static uint32_t sAudioUsSeen;

uint64_t dc_prof_now_us(void) {
    return timer_us_gettime64();
}

// Which part of the frame the main thread is in: 0 = logic (frame start .. DCP_LOGIC),
// m + 1 = after mark m. Tags every PC sample.
static volatile uint8_t sStage;

// SH4 performance counter 1 over the display-list translation (DCP_WAIT_END .. DCP_DL), one event per
// frame in rotation, so per scene we get cycles, data/instruction cache stall cycles and
// instructions. (Counter 0 is KOS's timer.)
static const perf_cntr_event_t sPmuEvent[PMU_COUNT] = {
    PMCR_ELAPSED_TIME_MODE, PMCR_PIPELINE_FREEZE_BY_DCACHE_MISS_MODE,
    PMCR_PIPELINE_FREEZE_BY_ICACHE_MISS_MODE, PMCR_INSTRUCTION_ISSUED_MODE,
};
static const char *const sPmuNames[PMU_COUNT] = { "cycles", "dstall", "istall", "insns" };
static uint32_t sPmuFrame;
static uint64_t sPmuValue;
static uint8_t sPmuValid;

void dc_prof_mark(int mark) {
    if (mark == DCP_DL) {
        perf_cntr_stop(PRFC1);
        sPmuValue = perf_cntr_count(PRFC1);
        sPmuValid = 1;
    }
    sMark[mark] = timer_us_gettime64();
    sStage = (uint8_t) (mark + 1);
    if (mark == DCP_WAIT_END) {   // translation starts; set in both frames of a 60 fps pair
        perf_cntr_clear(PRFC1);
        perf_cntr_start(PRFC1, sPmuEvent[sPmuFrame % PMU_COUNT], PMCR_COUNT_CPU_CYCLES);
    }
}

// DC_PROF_SKIP: the part of the frame gfx_pvr.c leaves out, to price it by subtraction (the codes
// are listed there). DC_PROF_SKIP=999 cycles through every code, a block of frames each, with
// blocks that leave nothing out between them, so one run prices every part on the same content.
// The render time read at a frame's end is that of a frame or two before, so the first frames of
// a block are not attributed.
#ifdef DC_PROF_SKIP
int gDcProfSkip = DC_PROF_SKIP == 999 ? 0 : DC_PROF_SKIP;
#if DC_PROF_SKIP == 999
static const uint8_t sSkipCodes[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
#define SKIP_CODES ((int) sizeof(sSkipCodes))
#define SKIP_BLOCK 10
#define SKIP_SETTLE 3
static uint32_t sSkipFrame;
typedef struct {
    uint64_t sum;
    uint32_t frames, over;
    uint16_t max;
} SkipStats;
static SkipStats sSceneSkip[MAX_SCENES][SKIP_CODES];
#endif
#endif

#ifdef DC_PROF_SAMPLE
// PC sampler (make DC_PROF_SAMPLE=1): TMU1 interrupts at DC_SAMPLE_HZ and stores the interrupted
// PC (offset from 0x8c000000, low 24 bits) with the frame stage in the top byte. Written to
// /pc/sm64_samples.bin at exit; tools/dc_prof_samples.py resolves it against the ELF.
// DC_PROF_SAMPLE_LATE=1 keeps only the samples of frames that turn out late: a frame's samples
// wait in a staging buffer until its end shows how many fields it took.
#ifdef DC_PROF_SAMPLE_LATE
#define DC_SAMPLE_HZ 2000
#else
#define DC_SAMPLE_HZ 500
#endif
#define DC_SAMPLE_CAP (32 * 1024)   // 128 KB (65 s at 500 Hz): the game leaves only ~1.4 MB of the 16 MB free
static uint32_t sSamples[DC_SAMPLE_CAP];
static volatile uint32_t sSampleCount;
#ifdef DC_PROF_SAMPLE_LATE
#define DC_STAGING_CAP 1024
static uint32_t sStaging[DC_STAGING_CAP];
static volatile uint32_t sStagingCount;
#endif

static void sample_irq(irq_t code, irq_context_t *ctx, void *data) {
    (void) code;
    (void) data;
    timer_clear(TMU1);
    uint32_t s = ((ctx->pc - 0x8c000000u) & 0x00FFFFFFu) | ((uint32_t) sStage << 24);
#ifdef DC_PROF_SAMPLE_LATE
    uint32_t n = sStagingCount;
    if (n < DC_STAGING_CAP) {
        sStaging[n] = s;
        sStagingCount = n + 1;
    }
#else
    uint32_t n = sSampleCount;
    if (n < DC_SAMPLE_CAP) {
        sSamples[n] = s;
        sSampleCount = n + 1;
    }
#endif
}

// Frame end: keep or drop the staged samples of the frame that just finished.
static void sample_frame_end(int late) {
#ifdef DC_PROF_SAMPLE_LATE
    uint32_t n = sStagingCount;
    if (late) {
        uint32_t at = sSampleCount;
        for (uint32_t i = 0; i < n && at < DC_SAMPLE_CAP; i++) {
            sSamples[at++] = sStaging[i];
        }
        sSampleCount = at;
    }
    sStagingCount = 0;
#else
    (void) late;
#endif
}

static void sampler_start(void) {
    irq_set_handler(EXC_TMU1_TUNI1, sample_irq, NULL);
    timer_prime(TMU1, DC_SAMPLE_HZ, 1);
    timer_clear(TMU1);
    timer_start(TMU1);
}

static void sampler_finish(void) {
    timer_stop(TMU1);
    irq_set_handler(EXC_TMU1_TUNI1, NULL, NULL);
    FILE *f = fopen("/pc/sm64_samples.bin", "wb");
    if (f == NULL) {
        printf("DCPROF no /pc mount, samples not written\n");
        return;
    }
    for (uint32_t at = 0; at < sSampleCount; at += 4096) {
        uint32_t chunk = sSampleCount - at < 4096 ? sSampleCount - at : 4096;
        fwrite(&sSamples[at], sizeof(sSamples[0]), chunk, f);
    }
    fclose(f);
    printf("DCPROF wrote /pc/sm64_samples.bin (%u samples at %u Hz)\n", (unsigned) sSampleCount, DC_SAMPLE_HZ);
}
#else
static void sample_frame_end(int late) {
    (void) late;
}
static void sampler_start(void) {
}
static void sampler_finish(void) {
}
#endif

void dc_prof_audio_add(unsigned int us) {
    sAudioUsTotal += us;
}

void dc_prof_asset_load(const char *name, unsigned int us, unsigned int bytes, int ok) {
    sFrameAssetUs += us;
    if (sAssetCount >= MAX_ASSETS) {
        sAssetsDropped++;
        return;
    }
    AssetRec *a = &sAssets[sAssetCount++];
    a->frame = sFrameCount + sFramesDropped;   // the frame in progress, as in LateRec
    a->us = us;
    a->bytes = bytes;
    a->scene = (gCurrDemoInput != NULL ? 0x8000 : 0) | ((gCurrLevelNum & 0x7F) << 8) | (gCurrAreaIndex & 0xFF);
    a->ok = (uint8_t) (ok != 0);
    strncpy(a->name, name, sizeof(a->name) - 1);
    a->name[sizeof(a->name) - 1] = '\0';
}

static uint16_t clamp16(uint64_t v) {
    return v > 0xFFFF ? 0xFFFF : (uint16_t) v;
}

// A mark that didn't fire this frame (e.g. reset-bar frames skip the level script)
// collapses onto the previous one, so its component reads as zero.
static uint64_t mark_or(int mark, uint64_t fallback) {
    return sMark[mark] >= sFrameStart ? sMark[mark] : fallback;
}

static SceneStats *scene_stats(uint16_t scene) {
    for (int i = 0; i < sSceneCount; i++) {
        if (sScenes[i].scene == scene) {
            return &sScenes[i];
        }
    }
    if (sSceneCount == MAX_SCENES) {
        return NULL;
    }
    SceneStats *s = &sScenes[sSceneCount++];
    memset(s, 0, sizeof(*s));
    s->scene = scene;
    s->vram_min = 0xFFFFFFFFu;
    return s;
}

static char sOut[65536];
static size_t sOutLen;

static void out_flush(FILE *f) {
    if (f != NULL && sOutLen) {
        fwrite(sOut, 1, sOutLen, f);
    }
    sOutLen = 0;
}

static void write_csv(void) {
    FILE *f = fopen("/pc/sm64_prof.csv", "w");
    if (f == NULL) {
        printf("DCPROF no /pc mount, per-frame CSV skipped\n");
        return;
    }
    sOutLen = snprintf(sOut, sizeof(sOut), "frame,level,area,demo,fields,vtx_kb");
    for (int c = 0; c < C_COUNT; c++) {
        sOutLen += snprintf(sOut + sOutLen, sizeof(sOut) - sOutLen, ",%s_us", sCompNames[c]);
    }
    sOutLen += snprintf(sOut + sOutLen, sizeof(sOut) - sOutLen, "\n");
    for (uint32_t i = 0; i < sFrameCount; i++) {
        const FrameRec *r = &sFrames[i];
        if (sizeof(sOut) - sOutLen < 128) {
            out_flush(f);
        }
        sOutLen += snprintf(sOut + sOutLen, sizeof(sOut) - sOutLen, "%u,%u,%u,%u,%u,%u", (unsigned) i,
                            (unsigned) ((r->scene >> 8) & 0x7F), (unsigned) (r->scene & 0xFF),
                            (unsigned) (r->scene >> 15), (unsigned) r->fields, (unsigned) r->vtx_kb);
        for (int c = 0; c < C_COUNT; c++) {
            sOutLen += snprintf(sOut + sOutLen, sizeof(sOut) - sOutLen, ",%u", (unsigned) r->us[c]);
        }
        sOutLen += snprintf(sOut + sOutLen, sizeof(sOut) - sOutLen, "\n");
    }
    out_flush(f);
    fclose(f);
    printf("DCPROF wrote /pc/sm64_prof.csv (%u frames)\n", (unsigned) sFrameCount);
}

static void print_scene_id(const char *kind, uint16_t scene) {
    printf("DCPROF %s level=%u area=%u demo=%u", kind, (unsigned) ((scene >> 8) & 0x7F),
           (unsigned) (scene & 0xFF), (unsigned) (scene >> 15));
}

static void print_report(void) {
    uint32_t late = 0;
    for (int i = 0; i < sSceneCount; i++) {
        late += sScenes[i].late;
    }
    printf("DCPROF RUN frames=%u late=%u vbl_wait=%d seconds=%u fsaa=%d\n", (unsigned) (sFrameCount + sFramesDropped),
           (unsigned) late, DC_VBL_WAIT, (unsigned) ((sFrameStart - sRunStart) / 1000000), DC_FSAA);

    // One line per scene: average and max of every component, in microseconds.
    for (int i = 0; i < sSceneCount; i++) {
        const SceneStats *s = &sScenes[i];
        print_scene_id("SCENE", s->scene);
        printf(" frames=%u late=%u", (unsigned) s->frames, (unsigned) s->late);
        for (int c = 0; c < C_COUNT; c++) {
            printf(" %s=%u/%u", sCompNames[c], (unsigned) (s->sum[c] / (s->frames ? s->frames : 1)),
                   (unsigned) s->max[c]);
        }
        for (int c = 0; c < LATE_CAUSES; c++) {
            printf(" late_%s=%u", sCauseNames[c], (unsigned) s->late_by[c]);
        }
        printf(" vram_free=%u\n", (unsigned) (s->vram_min == 0xFFFFFFFFu ? 0 : s->vram_min));
    }

    // Average per-frame event counts per scene.
    for (int i = 0; i < sSceneCount; i++) {
        const SceneStats *s = &sScenes[i];
        print_scene_id("COUNTS", s->scene);
        for (int c = 0; c < DCC_COUNT; c++) {
            printf(" %s=%u", sCountNames[c], (unsigned) (sSceneCounts[i][c] / (s->frames ? s->frames : 1)));
        }
        for (int c = 0; c < PMU_COUNT; c++) {
            uint32_t n = sScenePmuFrames[i][c];
            printf(" %s=%u", sPmuNames[c], (unsigned) (n ? sScenePmu[i][c] / n : 0));
        }
        printf("\n");
    }
    // Totals of the 60 fps frame skips (per-frame averages round them to 0): held = identical
    // second draws not drawn, dropped = exact frames dropped after a late one (DC_FRAME_MODE_60_DROP).
    for (int i = 0; i < sSceneCount; i++) {
        print_scene_id("STEPS", sScenes[i].scene);
        printf(" frames=%u held=%u dropped=%u\n", (unsigned) sScenes[i].frames,
               (unsigned) sSceneCounts[i][DCC_HELD], (unsigned) sSceneCounts[i][DCC_DROPPED]);
    }

#ifdef DC_PROF_LISTS
    // The largest PT/TR fill sources per scene, per-frame averages, largest first.
    for (int i = 0; i < sSceneCount; i++) {
        const SceneStats *s = &sScenes[i];
        uint32_t frames = s->frames ? s->frames : 1;
        uint8_t done[LIST_SRC_SCENE] = { 0 };
        for (int n = 0; n < 12; n++) {
            int best = -1;
            for (int j = 0; j < LIST_SRC_SCENE; j++) {
                if (!done[j] && sSceneSrc[i][j].tris && (best < 0 || sSceneSrc[i][j].px > sSceneSrc[i][best].px)) {
                    best = j;
                }
            }
            if (best < 0) {
                break;
            }
            done[best] = 1;
            const ListSrc *e = &sSceneSrc[i][best];
            print_scene_id("LISTSRC", s->scene);
            printf(" list=%s src=0x%08x px=%u tris=%u\n", e->kind == 2 ? "TR" : "PT", (unsigned) e->src,
                   (unsigned) (e->px / frames), (unsigned) (e->tris / frames));
        }
    }
#endif

#if defined(DC_PROF_SKIP) && DC_PROF_SKIP == 999
    // PVR render time per scene by what was left out of the frame.
    for (int i = 0; i < sSceneCount; i++) {
        for (int k = 0; k < SKIP_CODES; k++) {
            const SkipStats *ss = &sSceneSkip[i][k];
            if (!ss->frames) {
                continue;
            }
            print_scene_id("SKIP", sScenes[i].scene);
            printf(" code=%u frames=%u render=%u/%u over=%u\n", (unsigned) sSkipCodes[k], (unsigned) ss->frames,
                   (unsigned) (ss->sum / ss->frames), (unsigned) ss->max, (unsigned) ss->over);
        }
    }
#endif

    // Every late frame: where it was, how many fields it took and what it cost.
    for (uint32_t i = 0; i < sLateCount; i++) {
        const LateRec *r = &sLate[i];
        print_scene_id("LATE", r->scene);
        printf(" frame=%u vbl=%u since=%u fields=%u cause=%s logic=%u geo=%u dl=%u wait=%u render=%u audio=%u asset=%u\n",
               (unsigned) r->frame, (unsigned) r->vbl, (unsigned) r->since_scene, (unsigned) r->fields,
               sCauseNames[r->cause], (unsigned) r->logic, (unsigned) r->geo, (unsigned) r->dl, (unsigned) r->wait,
               (unsigned) r->render, (unsigned) r->audio, (unsigned) r->asset);
    }
    if (sLateDropped) {
        printf("DCPROF LATE_DROPPED %u\n", (unsigned) sLateDropped);
    }

    // Every asset read on the main thread, in order.
    for (uint32_t i = 0; i < sAssetCount; i++) {
        const AssetRec *a = &sAssets[i];
        print_scene_id("ASSET", a->scene);
        printf(" frame=%u us=%u bytes=%u ok=%u name=%s\n", (unsigned) a->frame, (unsigned) a->us,
               (unsigned) a->bytes, (unsigned) a->ok, a->name);
    }
    if (sAssetsDropped) {
        printf("DCPROF ASSET_DROPPED %u\n", (unsigned) sAssetsDropped);
    }

    // The worst frames by total time, with their breakdown.
    enum { WORST = 24 };
    uint32_t worst[WORST];
    int nworst = 0;
    for (uint32_t i = 0; i < sFrameCount; i++) {
        int pos = nworst;
        while (pos > 0 && sFrames[worst[pos - 1]].us[C_TOTAL] < sFrames[i].us[C_TOTAL]) {
            pos--;
        }
        if (pos >= WORST) {
            continue;
        }
        if (nworst < WORST) {
            nworst++;
        }
        memmove(&worst[pos + 1], &worst[pos], (nworst - 1 - pos) * sizeof(worst[0]));
        worst[pos] = i;
    }
    for (int w = 0; w < nworst; w++) {
        const FrameRec *r = &sFrames[worst[w]];
        printf("DCPROF WORST frame=%u level=%u area=%u fields=%u", (unsigned) worst[w],
               (unsigned) ((r->scene >> 8) & 0x7F), (unsigned) (r->scene & 0xFF), (unsigned) r->fields);
        for (int c = 0; c < C_COUNT; c++) {
            printf(" %s=%u", sCompNames[c], (unsigned) r->us[c]);
        }
        printf("\n");
    }
}

// Hang watchdog: if no frame completes for DC_HANG_SECONDS, print where the main thread is
// (PC, PR and code addresses found on its stack, for addr2line on the host) and exit to
// dcload, so a hung build never leaves the shared console needing a restart.
#ifndef DC_HANG_SECONDS
#define DC_HANG_SECONDS 10
#endif
extern char start[];   // KOS entry point, the start of .text (asm symbol _start)
static kthread_t *sMainThread;

static void *hang_watchdog(void *arg) {
    (void) arg;
    uint32_t last = sProgress;
    int idle = 0;

    for (;;) {
        thd_sleep(1000);
        if (sProgress != last) {
            last = sProgress;
            idle = 0;
            continue;
        }
        if (++idle < DC_HANG_SECONDS) {
            continue;
        }
        const irq_context_t *ctx = &sMainThread->context;
        int stage = -1;   // last frame mark reached in the stuck frame
        for (int m = 0; m < DCP_COUNT; m++) {
            if (sMark[m] >= sFrameStart) {
                stage = m;
            }
        }
        printf("DCPROF HANG frames=%u stage=%d pc=%08lx pr=%08lx sp=%08lx\n", (unsigned) sProgress, stage,
               (unsigned long) ctx->pc, (unsigned long) ctx->pr, (unsigned long) ctx->r[15]);
        const uint32_t *sp = (const uint32_t *) ctx->r[15];
        for (int i = 0; i < 256; i++) {
            if (sp[i] >= (uint32_t) (uintptr_t) start && sp[i] < (uint32_t) (uintptr_t) sFrames) {
                printf("DCPROF STACK %08lx\n", (unsigned long) sp[i]);
            }
        }
        dc_prof_stop_audio();
        sampler_finish();
        print_report();
        printf("DCPROF exit (hang)\n");
        arch_exit();
    }
    return NULL;
}

void dc_prof_init(void) {
    sMainThread = thd_get_current();
    // Above the main and audio threads, so it still runs if the main thread spins.
    kthread_attr_t attr = { 0 };
    attr.create_detached = 1;
    attr.prio = PRIO_DEFAULT - 2;
    attr.label = "DcProfWatchdog";
    thd_create_ex(&attr, hang_watchdog, NULL);
    sampler_start();
}

void dc_prof_frame_end(void) {
    uint64_t end = timer_us_gettime64();
    uint64_t vbl = vblticker;

    if (sRunStart == 0) {
        // First call: start the clock; there is no complete frame to record yet.
        sRunStart = sFrameStart = end;
        sLastVbl = vbl;
        sAudioUsSeen = sAudioUsTotal;
        sProgress++;
        return;
    }

    uint64_t logic  = mark_or(DCP_LOGIC, sFrameStart);
    uint64_t geo    = mark_or(DCP_GEO, logic);
    uint64_t w0     = mark_or(DCP_WAIT_BEGIN, geo);
    uint64_t w1     = mark_or(DCP_WAIT_END, w0);
    uint64_t dl     = mark_or(DCP_DL, w1);
    uint64_t finish = mark_or(DCP_FINISH, dl);

    pvr_stats_t st;
    pvr_get_stats(&st);
    uint32_t audio = sAudioUsTotal;

    {
        // Per-frame records stop at MAX_FRAMES (the CSV); the per-scene stats keep going.
        FrameRec overflow;
        FrameRec *r = sFrameCount < MAX_FRAMES ? &sFrames[sFrameCount] : &overflow;
        uint16_t demo = gCurrDemoInput != NULL ? 0x8000 : 0;
        r->scene = demo | ((gCurrLevelNum & 0x7F) << 8) | (gCurrAreaIndex & 0xFF);
        r->fields = (uint8_t) (vbl - sLastVbl > 255 ? 255 : vbl - sLastVbl);
        r->vtx_kb = (uint8_t) (st.vtx_buffer_used / 1024 > 255 ? 255 : st.vtx_buffer_used / 1024);
        r->us[C_TOTAL] = clamp16(end - sFrameStart);
        r->us[C_LOGIC] = clamp16(logic - sFrameStart);
        r->us[C_GEO] = clamp16(geo - logic);
        r->us[C_WAIT] = clamp16(w1 - w0);
        r->us[C_DL] = clamp16((dl - geo) - (w1 - w0));
        r->us[C_FINISH] = clamp16(finish - dl);
        r->us[C_VSYNC] = clamp16(end - finish);
        r->us[C_AUDIO] = clamp16(audio - sAudioUsSeen);
        r->us[C_RENDER] = clamp16(st.rnd_last_time / 1000);

        if (r->scene != sPrevScene) {
            sPrevScene = r->scene;
            sSinceScene = 0;
        }
        int is_late = r->fields > DC_VBL_WAIT;
        sample_frame_end(is_late);

        SceneStats *s = scene_stats(r->scene);
#ifdef DC_PROF_LISTS
        if (s != NULL) {
            list_src_merge((int) (s - sScenes));
        }
        sFrameSrcCount = 0;
#endif
        if (s != NULL) {
            for (int c = 0; c < DCC_COUNT; c++) {
                sSceneCounts[s - sScenes][c] += gDcProfCount[c];
            }
            if (sPmuValid) {
                sScenePmu[s - sScenes][sPmuFrame % PMU_COUNT] += sPmuValue;
                sScenePmuFrames[s - sScenes][sPmuFrame % PMU_COUNT]++;
            }
            s->frames++;
            if (is_late) {
                uint32_t cpu = (uint32_t) r->us[C_LOGIC] + r->us[C_GEO] + r->us[C_DL] + r->us[C_FINISH];
                int load = r->us[C_LOGIC] >= LOAD_LOGIC_US || r->us[C_DL] >= LOAD_DL_US ||
                           sSinceScene < LOAD_SCENE_FRAMES || sFrameAssetUs != 0;
                int cpu_bound = cpu >= LATE_BUDGET_US;
                int pvr_bound = r->us[C_WAIT] >= 500 || r->us[C_RENDER] >= LATE_BUDGET_US;
                uint8_t cause = load ? LATE_LOAD
                              : cpu_bound && pvr_bound ? LATE_BOTH
                              : cpu_bound ? LATE_CPU
                              : pvr_bound ? LATE_PVR
                              : LATE_OTHER;
                s->late++;
                s->late_by[cause]++;
                if (sLateCount < MAX_LATE) {
                    LateRec *l = &sLate[sLateCount++];
                    l->frame = sFrameCount + sFramesDropped;
                    l->vbl = (uint32_t) vbl;
                    l->scene = r->scene;
                    l->since_scene = (uint16_t) (sSinceScene > 0xFFFF ? 0xFFFF : sSinceScene);
                    l->fields = r->fields;
                    l->cause = cause;
                    l->logic = r->us[C_LOGIC];
                    l->geo = r->us[C_GEO];
                    l->dl = r->us[C_DL];
                    l->wait = r->us[C_WAIT];
                    l->render = r->us[C_RENDER];
                    l->audio = r->us[C_AUDIO];
                    l->asset = clamp16(sFrameAssetUs);
                } else {
                    sLateDropped++;
                }
            }
            for (int c = 0; c < C_COUNT; c++) {
                s->sum[c] += r->us[c];
                if (r->us[c] > s->max[c]) {
                    s->max[c] = r->us[c];
                }
            }
            // Free texture VRAM, sampled now and then (the allocator walk is not free).
            if ((s->frames & 31) == 1) {
                uint32_t avail = (uint32_t) pvr_mem_available();
                if (avail < s->vram_min) {
                    s->vram_min = avail;
                }
            }
#if defined(DC_PROF_SKIP) && DC_PROF_SKIP == 999
            if (sSkipFrame % SKIP_BLOCK >= SKIP_SETTLE) {
                SkipStats *ss = &sSceneSkip[s - sScenes][(sSkipFrame / SKIP_BLOCK) % SKIP_CODES];
                ss->sum += r->us[C_RENDER];
                ss->frames++;
                if (r->us[C_RENDER] > ss->max) {
                    ss->max = r->us[C_RENDER];
                }
                if (r->us[C_RENDER] > FIELD_US) {
                    ss->over++;
                }
            }
#endif
        }
        if (sFrameCount < MAX_FRAMES) {
            sFrameCount++;
        } else {
            sFramesDropped++;
        }
    }

#if defined(DC_PROF_SKIP) && DC_PROF_SKIP == 999
    sSkipFrame++;
    gDcProfSkip = sSkipCodes[(sSkipFrame / SKIP_BLOCK) % SKIP_CODES];
#endif

    sSinceScene++;
    memset(gDcProfCount, 0, sizeof(gDcProfCount));
    if (sPmuValid) {
        sPmuValid = 0;
        sPmuFrame++;
    }
    sAudioUsSeen = audio;
    sFrameAssetUs = 0;
    sLastVbl = vbl;
    sFrameStart = end;
    sProgress++;
    sStage = 0;

    if (end - sRunStart >= (uint64_t) DC_AUTO_EXIT_SECONDS * 1000000) {
        dc_prof_stop_audio();
        sampler_finish();
        print_report();
        write_csv();
        printf("DCPROF exit\n");
        arch_exit();
    }
}

#endif
