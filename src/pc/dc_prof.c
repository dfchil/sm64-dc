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

#define MAX_FRAMES 16384
static FrameRec sFrames[MAX_FRAMES];
static uint32_t sFrameCount;
static uint32_t sFramesDropped;   // frames past MAX_FRAMES: in the scene stats, not the CSV

typedef struct {
    uint16_t scene;
    uint32_t frames, late;
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
    "op_tris", "op_px", "pt_tris", "pt_px", "pt_opq_px", "tr_tris", "tr_px", "tr_opq_tris", "tr_opq_px" };
static int sSceneCount;

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

#ifdef DC_PROF_SAMPLE
// PC sampler (make DC_PROF_SAMPLE=1): TMU1 interrupts at DC_SAMPLE_HZ and stores the interrupted
// PC (offset from 0x8c000000, low 24 bits) with the frame stage in the top byte. Written to
// /pc/sm64_samples.bin at exit; tools/dc_prof_samples.py resolves it against the ELF.
#define DC_SAMPLE_HZ 500
#define DC_SAMPLE_CAP (32 * 1024)   // 128 KB (65 s at 500 Hz): the game leaves only ~1.4 MB of the 16 MB free
static uint32_t sSamples[DC_SAMPLE_CAP];
static volatile uint32_t sSampleCount;

static void sample_irq(irq_t code, irq_context_t *ctx, void *data) {
    (void) code;
    (void) data;
    timer_clear(TMU1);
    uint32_t n = sSampleCount;
    if (n < DC_SAMPLE_CAP) {
        sSamples[n] = ((ctx->pc - 0x8c000000u) & 0x00FFFFFFu) | ((uint32_t) sStage << 24);
        sSampleCount = n + 1;
    }
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
static void sampler_start(void) {
}
static void sampler_finish(void) {
}
#endif

void dc_prof_audio_add(unsigned int us) {
    sAudioUsTotal += us;
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

static void print_report(void) {
    uint32_t late = 0;
    for (int i = 0; i < sSceneCount; i++) {
        late += sScenes[i].late;
    }
    printf("DCPROF RUN frames=%u late=%u vbl_wait=%d seconds=%u\n", (unsigned) (sFrameCount + sFramesDropped),
           (unsigned) late, DC_VBL_WAIT, (unsigned) ((sFrameStart - sRunStart) / 1000000));

    // One line per scene: average and max of every component, in microseconds.
    for (int i = 0; i < sSceneCount; i++) {
        const SceneStats *s = &sScenes[i];
        printf("DCPROF SCENE level=%u area=%u demo=%u frames=%u late=%u",
               (unsigned) ((s->scene >> 8) & 0x7F), (unsigned) (s->scene & 0xFF),
               (unsigned) (s->scene >> 15), (unsigned) s->frames, (unsigned) s->late);
        for (int c = 0; c < C_COUNT; c++) {
            printf(" %s=%u/%u", sCompNames[c], (unsigned) (s->sum[c] / (s->frames ? s->frames : 1)),
                   (unsigned) s->max[c]);
        }
        printf("\n");
    }

    // Average per-frame event counts per scene.
    for (int i = 0; i < sSceneCount; i++) {
        const SceneStats *s = &sScenes[i];
        printf("DCPROF COUNTS level=%u area=%u demo=%u", (unsigned) ((s->scene >> 8) & 0x7F),
               (unsigned) (s->scene & 0xFF), (unsigned) (s->scene >> 15));
        for (int c = 0; c < DCC_COUNT; c++) {
            printf(" %s=%u", sCountNames[c], (unsigned) (sSceneCounts[i][c] / (s->frames ? s->frames : 1)));
        }
        for (int c = 0; c < PMU_COUNT; c++) {
            uint32_t n = sScenePmuFrames[i][c];
            printf(" %s=%u", sPmuNames[c], (unsigned) (n ? sScenePmu[i][c] / n : 0));
        }
        printf("\n");
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

        SceneStats *s = scene_stats(r->scene);
        if (s != NULL) {
            for (int c = 0; c < DCC_COUNT; c++) {
                sSceneCounts[s - sScenes][c] += gDcProfCount[c];
            }
            if (sPmuValid) {
                sScenePmu[s - sScenes][sPmuFrame % PMU_COUNT] += sPmuValue;
                sScenePmuFrames[s - sScenes][sPmuFrame % PMU_COUNT]++;
            }
            s->frames++;
            if (r->fields > DC_VBL_WAIT) {
                s->late++;
            }
            for (int c = 0; c < C_COUNT; c++) {
                s->sum[c] += r->us[c];
                if (r->us[c] > s->max[c]) {
                    s->max[c] = r->us[c];
                }
            }
        }
        if (sFrameCount < MAX_FRAMES) {
            sFrameCount++;
        } else {
            sFramesDropped++;
        }
    }

    memset(gDcProfCount, 0, sizeof(gDcProfCount));
    if (sPmuValid) {
        sPmuValid = 0;
        sPmuFrame++;
    }
    sAudioUsSeen = audio;
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
