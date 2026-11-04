// pc-endjinn host build only: SM64_INTERP_STATS=1 counts what the display-list interpreter still
// draws, by the list it is in, and prints it at exit: the work left before everything is native.
// A list in the program's data is named by its symbol; one built at run time (alloc_display_list)
// by the function that allocated it; any other by the symbol it lies in ("?" for the heap).
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "interp_stats.h"

int gInterpStatsOn = -1;   // -1: SM64_INTERP_STATS not read yet

#define OWNERS 1024
#define ALLOCS 8192
#define DEPTH 64

typedef struct {
    const char *name;
    unsigned long lists, cmds, loads, verts, tris, rects, frames;
    unsigned last_frame;
} Owner;

static Owner sOwners[OWNERS];
static int sNumOwners;
static struct { uintptr_t lo, hi; int owner; } sAllocs[ALLOCS];
static int sNumAllocs;
static int sStack[DEPTH];
static int sDepth;
static int sCur;
static unsigned sFrame;
static unsigned long sAllTris;   // every triangle that reaches gfx_sp_tri1, interpreted or native

static int owner_named(const char *name) {
    for (int i = 0; i < sNumOwners; i++) {
        if (strcmp(sOwners[i].name, name) == 0) return i;
    }
    if (sNumOwners == OWNERS) return OWNERS - 1;
    sOwners[sNumOwners].name = strdup(name);
    return sNumOwners++;
}

static void print_stats(void) {
    unsigned long tris = 0;
    for (int i = 0; i < sNumOwners; i++) tris += sOwners[i].tris;
    // By triangles, then commands.
    for (int i = 0; i < sNumOwners; i++) {
        for (int j = i + 1; j < sNumOwners; j++) {
            Owner *a = &sOwners[i], *b = &sOwners[j];
            if (b->tris > a->tris || (b->tris == a->tris && b->cmds > a->cmds)) {
                Owner t = *a;
                *a = *b;
                *b = t;
            }
        }
    }
    printf("INTERP: %u frames; triangles %lu interpreted of %lu (%.1f%%)\n", sFrame, tris, sAllTris,
           sAllTris ? 100.0 * tris / sAllTris : 0.0);
    printf("INTERP: %-40s %7s %9s %9s %9s %9s %7s %6s\n", "list (owner)", "frames", "lists", "cmds", "verts",
           "tris", "rects", "tris%");
    for (int i = 0; i < sNumOwners; i++) {
        const Owner *o = &sOwners[i];
        if (o->cmds == 0) continue;
        printf("INTERP: %-40.40s %7lu %9lu %9lu %9lu %9lu %7lu %6.1f\n", o->name, o->frames, o->lists, o->cmds,
               o->verts, o->tris, o->rects, tris ? 100.0 * o->tris / tris : 0.0);
    }
}

static int on(void) {
    if (gInterpStatsOn < 0) {
        gInterpStatsOn = getenv("SM64_INTERP_STATS") != NULL;
        if (gInterpStatsOn) {
            sCur = owner_named("(frame)");
            atexit(print_stats);
        }
    }
    return gInterpStatsOn;
}

void interp_stats_alloc(const void *ptr, unsigned size, const void *caller) {
    if (!on() || ptr == NULL || sNumAllocs == ALLOCS) return;
    Dl_info di;
    const char *name = dladdr(caller, &di) && di.dli_sname != NULL ? di.dli_sname : "? (alloc)";
    sAllocs[sNumAllocs].lo = (uintptr_t) ptr;
    sAllocs[sNumAllocs].hi = (uintptr_t) ptr + size;
    sAllocs[sNumAllocs].owner = owner_named(name);
    sNumAllocs++;
}

void interp_stats_pool_reset(void) {
    sNumAllocs = 0;
}

void interp_stats_frame(void) {
    if (!on()) return;
    sFrame++;
    sDepth = 0;
    sCur = owner_named("(frame)");
}

static int owner_of(const void *dl) {
    const uintptr_t a = (uintptr_t) dl;
    for (int i = sNumAllocs - 1; i >= 0; i--) {
        if (a >= sAllocs[i].lo && a < sAllocs[i].hi) return sAllocs[i].owner;
    }
    Dl_info di;
    if (dladdr(dl, &di) && di.dli_sname != NULL) return owner_named(di.dli_sname);
    return owner_named("? (heap)");
}

static void count_list(void) {
    Owner *o = &sOwners[sCur];
    o->lists++;
    if (o->last_frame != sFrame) {
        o->last_frame = sFrame;
        o->frames++;
    }
}

void interp_stats_call(const void *dl, int push) {
    if (push && sDepth < DEPTH) sStack[sDepth++] = sCur;
    sCur = owner_of(dl);
    count_list();
}

void interp_stats_return(void) {
    if (sDepth > 0) sCur = sStack[--sDepth];
}

void interp_stats_cmd(unsigned opcode) {
    Owner *o = &sOwners[sCur];
    o->cmds++;
    (void) opcode;
}

void interp_stats_vtx(unsigned n) {
    sOwners[sCur].loads++;
    sOwners[sCur].verts += n;
}

void interp_stats_tri(unsigned n) {
    sOwners[sCur].tris += n;
}

void interp_stats_rect(void) {
    sOwners[sCur].rects++;
}

void interp_stats_any_tri(void) {
    sAllTris++;
}
