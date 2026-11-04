#ifndef INTERP_STATS_H
#define INTERP_STATS_H

// pc-endjinn host build only (interp_stats.c): what the display-list interpreter still draws.
extern int gInterpStatsOn;
void interp_stats_alloc(const void *ptr, unsigned size, const void *caller);
void interp_stats_pool_reset(void);
void interp_stats_frame(void);
void interp_stats_call(const void *dl, int push);
void interp_stats_return(void);
void interp_stats_cmd(unsigned opcode);
void interp_stats_vtx(unsigned n);
void interp_stats_tri(unsigned n);
void interp_stats_rect(void);
void interp_stats_any_tri(void);

#endif
