/* The game's audio through AICAflow (make DC_AICAFLOW=1).
 *
 * src/audio/external.c keeps its music queue and request logic; where it would
 * start, fade or stop a sequence player, or queue a sound, it calls in here.
 * Nothing is sequenced on the SH4: a song or sound is one AFB (its samples)
 * and one AFX (its control flow), built offline from a console recording of
 * the original driver (tools/aica/sm64_afx.py). A song loops by itself
 * (AFX LOOP); a sound ends by itself.
 *
 * Assets are read from DC_AICAFLOW_ROOT when first needed: seqXX.afb/.afx for
 * sequence XX (hex), sfx_XXXXXXXX.afb/.afx for a sound's bank and id bits
 * (soundBits & 0xF0FF0000). Missing ones are remembered and stay silent.
 * Loading is synchronous; a planned per-area residency comes later.
 *
 * Fade timers are the sequencer's units: audio sub-updates, 240 per second,
 * so 8 per 30 Hz game tick (sm64_afx_tick). */
#ifdef DC_AICAFLOW

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kos.h>
#include <aicaflow/host.h>
#include <aicaflow/bank.h>

#include "sm64_aicaflow.h"
extern s16 gCurrLevelNum;
#include "pc/dc_prof.h"

#ifndef DC_AICAFLOW_ROOT
#define DC_AICAFLOW_ROOT "/pc/afx"
#endif
#define SUB_UPDATES_PER_TICK 8
#define MUSIC_PLAYERS 3
#define SFX_ASSETS 320
#define SONG_CACHE 16
#define SFX_VOICES 32
#define RETIRING 16

extern const unsigned char gAicaflowFirmware[];
extern const unsigned int gAicaflowFirmwareSize;

typedef struct {
    afx_bank_t bank;
    afx_asset_t flow;
    u32 id;
    u8 resident;   /* loaded from a scene pack: kept until its scene ends, not retired */
    u8 core;       /* from scene_core: kept for the whole run */
    afx_bank_t *shared;   /* a version 2 pack's scene bank, which it does not own; else bank */
} Asset;

/* Version 2 packs (tools/aica/sm64_scene_banks.py) bring one sample bank each that all their
 * flows bind to: scene_core's for the run, the level's until its last flow is freed. */
static afx_bank_t sCoreBank, sLevelBank;
static u8 sCoreBankUp, sLevelBankUp, sLevelBankDying;

/* The ended level's bank goes once no flow binds to it (retired flows may still, briefly). */
static void level_bank_release(void) {
    if (sLevelBankDying && afx_bank_release(&sLevelBank) == 0) {
        sLevelBankUp = sLevelBankDying = 0;
    }
}

typedef struct {
    Asset asset;             /* the loaded song; id = sequence + 1, 0 = none */
    afx_instance_t inst;
    f32 gain, target, step;  /* fade state, 0..1 */
    f32 scale;               /* lowered volume (func_8031FFB4), 0..1 */
    u8 sentGain;
    u8 stopAtZero;
} Music;

static Music sMusic[MUSIC_PLAYERS];
static Asset sSfx[SFX_ASSETS];
/* Songs from scene packs, kept loaded so a song that comes round again (the title's) is not
 * read from /pc again; id = sequence + 1. */
static Asset sSongs[SONG_CACHE];
static s16 sSceneLevel = -1;
static u8 sCorePackTried;
static u8 sPackAbsent[128];   /* per level: it has no pack (not looked for again) */
/* A playing sound: which asset, from which source (the game's position pointer), and for a
 * continuous sound (no 0x80 flag) the tick it was last requested: the game requests those every
 * frame and they stop two ticks after the requests do. */
typedef struct {
    afx_instance_t inst;
    u32 key, bits;
    f32 *pos;
    u32 last;
    u8 continuous;
    u8 gain;     /* last sent */
    s8 pan;      /* last sent lane pan offset */
} SfxVoice;
static SfxVoice sSfxVoice[SFX_VOICES];
static u32 sSfxNext, sTick;
static struct { Asset asset; afx_instance_t inst; } sRetiring[RETIRING];
/* Stops the firmware has not taken yet: a lifecycle command returns AFX_BUSY while an earlier one
 * is unacknowledged (or AFX_IPC_FULL with the queue full), and a stop lost that way leaves its
 * song playing with nothing holding it. Retried every tick until taken or the instance is gone. */
#define STOPS_PENDING 32
static afx_instance_t sStopPending[STOPS_PENDING];
/* DC_AICAFLOW_DEBUG: log every music call and the music instances' status every two seconds. */
#ifdef DC_AICAFLOW_DEBUG
static u32 sTicks;
#define DBG(...) printf(__VA_ARGS__)
#else
#define DBG(...) ((void) 0)
#endif
static u8 sMissingSeq[128];
static u32 sMissingSfx[256];
static u32 sMissingSfxCount;
static int sReady;

static int read_file(const char *path, void **out, u32 *bytes) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *data = size > 0 ? memalign(32, (size_t) size) : NULL;
    if (data == NULL || fread(data, 1, (size_t) size, f) != (size_t) size) {
        fclose(f);
        free(data);
        return -1;
    }
    fclose(f);
    *out = data;
    *bytes = (u32) size;
    return 0;
}

static int asset_load_files(Asset *a, const char *stem, u32 id, u32 *bytes) {
    char path[128];
    void *flow = NULL;
    memset(a, 0, sizeof(*a));
    snprintf(path, sizeof(path), "%s/%s.afb", DC_AICAFLOW_ROOT, stem);
    int result = afx_bank_load_file(&a->bank, path);
    if (result) return result;
    snprintf(path, sizeof(path), "%s/%s.afx", DC_AICAFLOW_ROOT, stem);
    result = read_file(path, &flow, bytes) ? -1 : afx_bank_flow_upload(&a->bank, flow, *bytes, &a->flow);
    free(flow);
    if (result) {
        afx_bank_release(&a->bank);
        memset(a, 0, sizeof(*a));
        printf("AICAFLOW: %s: flow not loaded (%d)\n", stem, result);
        return result ? result : -1;
    }
    a->id = id;
    return 0;
}

/* Every load reads /pc on the game thread, a missing asset included (a failed open is a dcload
 * round trip too); DC_PROF builds log each one with its time. */
static int asset_load(Asset *a, const char *stem, u32 id) {
#ifdef DC_PROF
    u64 start = dc_prof_now_us();
#endif
    u32 bytes = 0;
    int result = asset_load_files(a, stem, id, &bytes);
#ifdef DC_PROF
    dc_prof_asset_load(stem, (unsigned int) (dc_prof_now_us() - start), bytes, result == 0);
#endif
    return result;
}

static int gone(afx_instance_t inst);

static void stop_inst(afx_instance_t inst) {
    if (!inst) return;
    int result = afx_instance_stop(inst);
    if (result != -AFX_BUSY && result != -AFX_IPC_FULL) return;
    for (int i = 0; i < STOPS_PENDING; i++) {
        if (sStopPending[i] == inst) return;
    }
    for (int i = 0; i < STOPS_PENDING; i++) {
        if (!sStopPending[i]) {
            sStopPending[i] = inst;
            return;
        }
    }
    printf("AICAFLOW: no room to retry a stop; instance %lx may keep playing\n", (unsigned long) inst);
}

static void retry_stops(void) {
    for (int i = 0; i < STOPS_PENDING; i++) {
        afx_instance_t inst = sStopPending[i];
        if (!inst) continue;
        int result = gone(inst) ? 0 : afx_instance_stop(inst);
        if (result != -AFX_BUSY && result != -AFX_IPC_FULL) sStopPending[i] = 0;
    }
}

/* Hand an asset (and its last instance) over to be freed once the instance is gone. */
static void retire(Asset *a, afx_instance_t inst) {
    if (!a->id) return;
    stop_inst(inst);
    if (a->resident) {   /* a copy of a scene-pack song: the cache keeps it */
        memset(a, 0, sizeof(*a));
        return;
    }
    for (int i = 0; i < RETIRING; i++) {
        if (!sRetiring[i].asset.id) {
            sRetiring[i].asset = *a;
            sRetiring[i].inst = inst;
            memset(a, 0, sizeof(*a));
            return;
        }
    }
    printf("AICAFLOW: no room to retire asset %lx; it stays loaded\n", (unsigned long) a->id);
    memset(a, 0, sizeof(*a));
}

static int gone(afx_instance_t inst) {
    afx_instance_status_t status;
    return !inst || afx_instance_status(inst, &status) == -AFX_STALE_GENERATION;
}

static void free_retired(void) {
    for (int i = 0; i < RETIRING; i++) {
        if (sRetiring[i].asset.id && gone(sRetiring[i].inst)) {
            afx_asset_free(sRetiring[i].asset.flow);
            if (!sRetiring[i].asset.shared) afx_bank_release(&sRetiring[i].asset.bank);
            memset(&sRetiring[i], 0, sizeof(sRetiring[i]));
        }
    }
    level_bank_release();
}

/* Scene packs (tools/aica/sm64_scene_packs.py): the songs and sounds a level was recorded to
 * use, read in one go when the level loads instead of file by file when each first plays.
 * Read from /pc over dcload, every file is a round trip: a 0.5 KB sound took ~35 ms, so one
 * pack per scene. "SMPK", version, count, then per entry a 24-byte stem and the offsets and
 * sizes of its .afb and .afx in the file. */
#define PACK_MAGIC 0x4B504D53u   /* "SMPK" */
typedef struct {
    char stem[24];
    u32 afb_off, afb_size, afx_off, afx_size;
} PackEntry;

typedef struct {
    char stem[24];
    u32 bank, afx_off, afx_size, reserved;
} PackEntry2;

static int pack_entry_load2(Asset *a, const u8 *base, const PackEntry2 *e, afx_bank_t *bank, u32 id, u8 core) {
    memset(a, 0, sizeof(*a));
    int result = afx_bank_flow_upload(bank, base + e->afx_off, e->afx_size, &a->flow);
    if (result) {
        printf("AICAFLOW: %.24s: flow not loaded (%d)\n", e->stem, result);
        memset(a, 0, sizeof(*a));
        return result;
    }
    a->shared = bank;
    a->id = id;
    a->resident = 1;
    a->core = core;
    return 0;
}

static int pack_entry_load(Asset *a, const u8 *base, const PackEntry *e, u32 id, u8 core) {
    memset(a, 0, sizeof(*a));
    int result = afx_bank_load_memory(&a->bank, base + e->afb_off, e->afb_size);
    if (result) return result;
    result = afx_bank_flow_upload(&a->bank, base + e->afx_off, e->afx_size, &a->flow);
    if (result) {
        afx_bank_release(&a->bank);
        memset(a, 0, sizeof(*a));
        return result;
    }
    a->id = id;
    a->resident = 1;
    a->core = core;
    return 0;
}

/* The slot a pack entry's asset goes to, with its id; NULL when it is loaded already or no
 * slot is free. */
static Asset *pack_slot(const char *stem, u32 *id) {
    Asset *table = sSfx, *slot = NULL;
    int count = SFX_ASSETS;
    if (strncmp(stem, "seq", 3) == 0) {
        *id = (u32) strtoul(stem + 3, NULL, 16) + 1;
        table = sSongs;
        count = SONG_CACHE;
    } else if (strncmp(stem, "sfx_", 4) == 0) {
        *id = (u32) strtoul(stem + 4, NULL, 16) + 1;
    } else {
        return NULL;
    }
    for (int i = 0; i < count; i++)
        if (table[i].id == *id) return NULL;
    for (int i = 0; i < count && !slot; i++)
        if (!table[i].id) slot = &table[i];
    return slot;
}

/* A version 2 pack read piece by piece: the header and entry table, then the bank, then each
 * flow, so SH4 RAM holds the largest piece instead of the whole pack (a course's is ~2 MB).
 * Returns 0 when the file is not a version 2 pack. */
static int pack_load_v2(const char *path, const char *stem, u8 core, u32 *loaded, u32 *bytes) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return 0;
    u32 head[5];
    if (fread(head, 1, sizeof(head), f) != sizeof(head) || head[0] != PACK_MAGIC || head[1] != 2 || head[2] > 1024) {
        fclose(f);
        return 0;
    }
    PackEntry2 *entries = malloc(head[2] * sizeof(PackEntry2));
    void *piece = memalign(32, head[4]);
    afx_bank_t *own = core ? &sCoreBank : &sLevelBank;
    u8 *up = core ? &sCoreBankUp : &sLevelBankUp;
    int result = -1;
    if (entries && piece && fread(entries, sizeof(PackEntry2), head[2], f) == head[2] &&
        fseek(f, (long) head[3], SEEK_SET) == 0 && fread(piece, 1, head[4], f) == head[4]) {
        if (!core) level_bank_release();   /* the previous level's, if its flows are gone by now */
        result = *up ? -AFX_BUSY : afx_bank_load_memory(own, piece, head[4]);
    }
    free(piece);
    piece = NULL;
    *bytes = head[4];
    if (result) {
        printf("AICAFLOW: %s: bank not loaded (%d)\n", stem, result);
    } else {
        *up = 1;
        u32 capacity = 0;
        for (u32 i = 0; i < head[2]; i++) {
            const PackEntry2 *e = &entries[i];
            afx_bank_t *bank = e->bank == 1 ? &sCoreBank : own;
            u32 id;
            if (e->bank == 1 && !sCoreBankUp) continue;
            Asset *slot = pack_slot(e->stem, &id);
            if (!slot) continue;
            if (e->afx_size > capacity) {
                free(piece);
                piece = memalign(32, e->afx_size);
                capacity = piece ? e->afx_size : 0;
            }
            if (!piece || fseek(f, (long) e->afx_off, SEEK_SET) || fread(piece, 1, e->afx_size, f) != e->afx_size) continue;
            *bytes += e->afx_size;
            const PackEntry2 local = { .bank = 0, .afx_off = 0, .afx_size = e->afx_size };
            PackEntry2 named = local;
            memcpy(named.stem, e->stem, sizeof(named.stem));
            if (!pack_entry_load2(slot, piece, &named, bank, id, core)) (*loaded)++;
        }
    }
    free(piece);
    free(entries);
    fclose(f);
    return 1;
}

static int pack_load(const char *stem, u8 core) {
    char path[128];
    void *data = NULL;
    u32 bytes = 0, loaded = 0, id;
#ifdef DC_PROF
    u64 start = dc_prof_now_us();
#endif
    snprintf(path, sizeof(path), "%s/%s.smp", DC_AICAFLOW_ROOT, stem);
    int found = 1;
    if (pack_load_v2(path, stem, core, &loaded, &bytes)) {
        goto done;
    }
    int ok = read_file(path, &data, &bytes) == 0 && bytes >= 20 && ((const u32 *) data)[0] == PACK_MAGIC;
    const u32 *head = data;
    if (ok && head[1] == 1 && 12 + head[2] * sizeof(PackEntry) <= bytes) {
        const u8 *base = data;
        const PackEntry *e = (const PackEntry *) (base + 12);
        for (u32 n = head[2]; n--; e++) {
            if (e->afb_off + e->afb_size > bytes || e->afx_off + e->afx_size > bytes) break;
            Asset *slot = pack_slot(e->stem, &id);
            if (slot && !pack_entry_load(slot, base, e, id, core)) loaded++;
        }
    } else if (ok && head[1] == 2 && 20 + head[2] * sizeof(PackEntry2) <= bytes && head[3] + head[4] <= bytes) {
        /* One bank for the pack: scene_core's for the run, a level's until the scene ends. */
        const u8 *base = data;
        afx_bank_t *own = core ? &sCoreBank : &sLevelBank;
        u8 *up = core ? &sCoreBankUp : &sLevelBankUp;
        if (!core) level_bank_release();   /* the previous level's, if its flows are gone by now */
        int result = *up ? -AFX_BUSY : afx_bank_load_memory(own, base + head[3], head[4]);
        if (result) {
            printf("AICAFLOW: %s: bank not loaded (%d)\n", stem, result);
        } else {
            *up = 1;
            const PackEntry2 *e = (const PackEntry2 *) (base + 20);
            for (u32 n = head[2]; n--; e++) {
                if (e->afx_off + e->afx_size > bytes) break;
                afx_bank_t *bank = e->bank == 1 ? &sCoreBank : own;
                if (e->bank == 1 && !sCoreBankUp) continue;
                Asset *slot = pack_slot(e->stem, &id);
                if (slot && !pack_entry_load2(slot, base, e, bank, id, core)) loaded++;
            }
        }
    } else {
        ok = 0;
    }
    found = ok;
    free(data);
done:;
    int ok_all = found;   /* the pack exists (whether or not everything in it fit) */
#ifdef DC_PROF
    dc_prof_asset_load(stem, (unsigned int) (dc_prof_now_us() - start), bytes, ok_all);
#endif
    afx_mem_stats_t mem;
    if (afx_mem_stats(&mem) == 0) {
        printf("AICAFLOW: pack %s: %lu assets; AICA RAM used %lu KB, free %lu KB (largest %lu KB)\n", stem,
               (unsigned long) loaded, (unsigned long) (mem.used_bytes / 1024), (unsigned long) (mem.free_bytes / 1024),
               (unsigned long) (mem.largest_free_block / 1024));
    }
    return ok_all;
}

static void asset_free(Asset *a) {
    if (!a->id) return;
    if (afx_asset_free(a->flow) == 0 && (a->shared || afx_bank_release(&a->bank) == 0)) {
        memset(a, 0, sizeof(*a));
        return;
    }
    a->resident = 0;   /* still referenced: freed once its instance is gone */
    retire(a, 0);
}

/* The previous scene ends: everything it played stops, and everything but scene_core leaves AICA
 * RAM, so the new scene starts with the room it needs. The stops are waited for (a level load
 * has time to spare) before the assets are freed. */
static void scene_end(void) {
    afx_instance_t stopped[MUSIC_PLAYERS + SFX_VOICES];
    int count = 0;
    for (int p = 0; p < MUSIC_PLAYERS; p++) {
        if (sMusic[p].inst) stopped[count++] = sMusic[p].inst;
        stop_inst(sMusic[p].inst);
        sMusic[p].inst = 0;
        sMusic[p].stopAtZero = 0;
    }
    for (int i = 0; i < SFX_VOICES; i++) {
        if (sSfxVoice[i].inst) stopped[count++] = sSfxVoice[i].inst;
        stop_inst(sSfxVoice[i].inst);
    }
    for (int wait = 0; wait < 200; wait++) {
        int busy = 0;
        retry_stops();
        for (int i = 0; i < count; i++) busy |= !gone(stopped[i]);
        if (!busy) break;
        thd_sleep(1);
    }
    for (int i = 0; i < SFX_VOICES; i++) sSfxVoice[i].inst = 0;
    for (int p = 0; p < MUSIC_PLAYERS; p++) {
        if (sMusic[p].asset.id && !sMusic[p].asset.resident) asset_free(&sMusic[p].asset);
        memset(&sMusic[p].asset, 0, sizeof(sMusic[p].asset));
    }
    for (int i = 0; i < SONG_CACHE; i++) {
        if (sSongs[i].id && !sSongs[i].core) asset_free(&sSongs[i]);
    }
    for (int i = 0; i < SFX_ASSETS; i++) {
        if (sSfx[i].id && !sSfx[i].core) asset_free(&sSfx[i]);
    }
    free_retired();
    if (sLevelBankUp) sLevelBankDying = 1;
    level_bank_release();
    /* A song or sound that failed for want of AICA RAM gets another chance. */
    memset(sMissingSeq, 0, sizeof(sMissingSeq));
    sMissingSfxCount = 0;
}

/* Called from init_level, and from the first song or sound of a level that does not run it (the
 * title). The level's pack is read each time the level starts. */
void sm64_afx_scene_prepare(s16 level) {
    if (!sReady || level == sSceneLevel) return;
    if (sSceneLevel >= 0) scene_end();
    sSceneLevel = level;
    if (!sCorePackTried) {
        sCorePackTried = 1;
        pack_load("scene_core", 1);
    }
    if (level < 0 || level >= (s16) sizeof(sPackAbsent) || sPackAbsent[level]) return;
    char stem[16];
    snprintf(stem, sizeof(stem), "scene_%02d", (int) level);
    if (!pack_load(stem, 0)) sPackAbsent[level] = 1;
}

int sm64_afx_init(void) {
    int result = afx_init(gAicaflowFirmware, gAicaflowFirmwareSize);
    printf("AICAFLOW: init %d (firmware %u bytes, assets from %s)\n", result, gAicaflowFirmwareSize,
           DC_AICAFLOW_ROOT);
    for (int p = 0; p < MUSIC_PLAYERS; p++) {
        sMusic[p].scale = 1.0f;
    }
    sReady = result == 0;
    return result;
}

static void music_apply_gain(Music *m) {
    f32 g = m->gain * m->scale;
    u8 v = g <= 0.0f ? 0 : g >= 1.0f ? 255 : (u8) (g * 255.0f + 0.5f);
    if (m->inst && v != m->sentGain) {
        afx_instance_gain(m->inst, v);
        m->sentGain = v;
    }
}

void sm64_afx_play_sequence(u8 player, u8 seqId, u16 fadeTimer) {
    if (!sReady || player >= MUSIC_PLAYERS || player == 2) return;   /* player 2 runs the SFX scripts */
    sm64_afx_scene_prepare(gCurrLevelNum);
    Music *m = &sMusic[player];
    u8 seq = seqId & 0x7f;
    DBG("AFXDBG t=%lu play player=%u seq=%02X fade=%u loaded=%lx\n", (unsigned long) sTicks, player, seqId, fadeTimer,
        (unsigned long) m->asset.id);
    afx_instance_t old = m->inst;
    m->inst = 0;
    if (m->asset.id != (u32) seq + 1) {
        retire(&m->asset, old);   /* stops it; freed once it is gone */
        old = 0;
    }
    if (old) stop_inst(old);
    for (int i = 0; i < SONG_CACHE && m->asset.id != (u32) seq + 1; i++)
        if (sSongs[i].id == (u32) seq + 1) m->asset = sSongs[i];
    if (m->asset.id != (u32) seq + 1) {
        if (sMissingSeq[seq]) return;
        char stem[16];
        snprintf(stem, sizeof(stem), "seq%02X", seq);
        if (asset_load(&m->asset, stem, (u32) seq + 1)) {
            sMissingSeq[seq] = 1;
            printf("AICAFLOW: no asset for sequence %02X; silent\n", seq);
            return;
        }
    }
    int result = afx_instance_activate(m->asset.flow, &m->inst);
    if (result) {
        printf("AICAFLOW: sequence %02X not started (%d)\n", seq, result);
        m->inst = 0;
        return;
    }
    m->stopAtZero = 0;
    m->target = 1.0f;
    if (fadeTimer) {
        m->gain = 0.0f;
        m->step = (f32) SUB_UPDATES_PER_TICK / (f32) fadeTimer;
    } else {
        m->gain = 1.0f;
        m->step = 1.0f;
    }
    m->sentGain = 255;
    music_apply_gain(m);
}

void sm64_afx_fade_out(u8 player, u16 fadeTimer) {
    DBG("AFXDBG t=%lu fade_out player=%u fade=%u\n", (unsigned long) sTicks, player, fadeTimer);
    if (!sReady || player >= MUSIC_PLAYERS) return;
    Music *m = &sMusic[player];
    if (!m->inst) return;
    if (!fadeTimer) {
        stop_inst(m->inst);
        m->inst = 0;
        return;
    }
    m->target = 0.0f;
    m->step = (f32) SUB_UPDATES_PER_TICK / (f32) fadeTimer;
    m->stopAtZero = 1;
}

void sm64_afx_set_volume(u8 player, f32 scale, u16 fadeTimer) {
    /* The lowered volume is applied at once; the fade timer is the original's
     * ramp, short enough not to matter for now. */
    (void) fadeTimer;
    DBG("AFXDBG t=%lu volume player=%u scale=%d%%\n", (unsigned long) sTicks, player, (int) (scale * 100));
    if (player >= MUSIC_PLAYERS) return;
    sMusic[player].scale = scale;
    music_apply_gain(&sMusic[player]);
}

/* Volume by the source's distance and pan by its direction, as the game computes them, sent
 * when they change: the instance's gain, and the pan offset of lane 0 (sounds are built with a
 * lane map holding every channel in lane 0; AICA pan steps are about 3 dB, 15 per side). */
static void sfx_place(SfxVoice *v) {
    f32 volume, pan;
    sm64_sound_dynamics(v->bits, v->pos, &volume, &pan);
    u8 gain = volume <= 0.0f ? 0 : volume >= 1.0f ? 255 : (u8) (volume * 255.0f + 0.5f);
    s8 offset = (s8) (pan <= 0.0f ? -15 : pan >= 1.0f ? 15 : (s32) ((pan - 0.5f) * 30.0f + (pan >= 0.5f ? 0.5f : -0.5f)));
    if (gain != v->gain) {
        afx_instance_gain(v->inst, gain);
        v->gain = gain;
    }
    if (offset != v->pan) {
        u8 values[32] = { (u8) offset };
        afx_instance_lanes_set(v->inst, AFX_LANE_PAN, 0, 1u, values);
        v->pan = offset;
    }
}

void sm64_afx_play_sound(s32 soundBits, f32 *pos) {
    if (!sReady) return;
    sm64_afx_scene_prepare(gCurrLevelNum);
    u32 key = (u32) soundBits & 0xF0FF0000u;
    Asset *a = NULL;
    for (int i = 0; i < SFX_ASSETS && !a; i++)
        if (sSfx[i].id == key + 1) a = &sSfx[i];
    if (a == NULL) {
        for (u32 i = 0; i < sMissingSfxCount; i++)
            if (sMissingSfx[i] == key) return;
        for (int i = 0; i < SFX_ASSETS && !a; i++)
            if (!sSfx[i].id) a = &sSfx[i];
        if (a == NULL) return;
        char stem[24];
        snprintf(stem, sizeof(stem), "sfx_%08lX", (unsigned long) key);
        if (asset_load(a, stem, key + 1)) {
            if (sMissingSfxCount < sizeof(sMissingSfx) / sizeof(sMissingSfx[0])) sMissingSfx[sMissingSfxCount++] = key;
            printf("AICAFLOW: no asset for sound %08lX; silent\n", (unsigned long) soundBits);
            return;
        }
    }
    u8 continuous = !(soundBits & 0x80);
    /* The same sound from the same source: a continuous one keeps playing, a 0x80 one restarts
     * (process_sound_request replaces it). A continuous one that ended by itself starts again. */
    for (int i = 0; i < SFX_VOICES; i++) {
        SfxVoice *v = &sSfxVoice[i];
        if (v->inst && v->key == key && v->pos == pos) {
            if (continuous && !gone(v->inst)) {
                v->last = sTick;
                return;
            }
            if (!gone(v->inst)) stop_inst(v->inst);
            v->inst = 0;
        }
    }
    SfxVoice *slot = NULL;
    for (int i = 0; i < SFX_VOICES && !slot; i++)
        if (!sSfxVoice[i].inst || gone(sSfxVoice[i].inst)) slot = &sSfxVoice[i];
    if (!slot) {
        slot = &sSfxVoice[sSfxNext++ % SFX_VOICES];   /* all busy: one makes way */
        stop_inst(slot->inst);
    }
    slot->key = key;
    slot->bits = (u32) soundBits;
    slot->pos = pos;
    slot->last = sTick;
    slot->continuous = continuous;
    if (afx_instance_activate(a->flow, &slot->inst)) {
        slot->inst = 0;
        return;
    }
    slot->gain = 255;
    slot->pan = 0;
    sfx_place(slot);
}

void sm64_afx_tick(void) {
    if (!sReady) return;
    sTick++;
    for (int i = 0; i < SFX_VOICES; i++) {
        SfxVoice *v = &sSfxVoice[i];
        if (!v->inst) continue;
        if (gone(v->inst) || (v->continuous && sTick - v->last > 2)) {
            if (!gone(v->inst)) stop_inst(v->inst);
            v->inst = 0;
            continue;
        }
        sfx_place(v);   /* the source moves */
    }
#ifdef DC_AICAFLOW_DEBUG
    if (++sTicks % 60 == 0) {
        for (int p = 0; p < MUSIC_PLAYERS; p++) {
            afx_instance_status_t st;
            if (!sMusic[p].inst) continue;
            int r = afx_instance_status(sMusic[p].inst, &st);
            DBG("AFXDBG t=%lu player=%d inst=%lx status=%d state=%lu result=%lu pos=%lu loops=%lu gain=%u\n",
                (unsigned long) sTicks, p, (unsigned long) sMusic[p].inst, r, (unsigned long) st.state,
                (unsigned long) st.result, (unsigned long) st.position, (unsigned long) st.loops, sMusic[p].sentGain);
        }
    }
#endif
    for (int p = 0; p < MUSIC_PLAYERS; p++) {
        Music *m = &sMusic[p];
        if (!m->inst) continue;
        if (m->gain < m->target) m->gain = m->gain + m->step > m->target ? m->target : m->gain + m->step;
        else if (m->gain > m->target) m->gain = m->gain - m->step < m->target ? m->target : m->gain - m->step;
        music_apply_gain(m);
        if (m->stopAtZero && m->gain <= 0.0f) {
            stop_inst(m->inst);
            m->inst = 0;
            m->stopAtZero = 0;
        }
    }
    retry_stops();
    free_retired();
}

#endif
