/* pc-endjinn host build only (pc_endjinn.mk puts this directory first on the include path):
 * pc-endjinn's KOS shim <kos.h>, plus the few KOS calls SM64's shared platform code uses that the
 * shim does not provide. */
#ifndef SM64_PC_ENDJINN_KOS_COMPAT_H
#define SM64_PC_ENDJINN_KOS_COMPAT_H

#include_next <kos.h>
#include <arch/timer.h>

#ifndef MAPLE_FUNC_MEMCARD
#define MAPLE_FUNC_MEMCARD 0x02000000u
#endif
#ifndef O_META
#define O_META 0
#endif
#ifndef FILEHND_INVALID
#define FILEHND_INVALID (-1)
#endif

static inline uint64_t timer_us_gettime64(void) {
    return timer_ns_gettime64() / 1000u;
}
static inline uint64_t timer_ms_gettime64(void) {
    return timer_ns_gettime64() / 1000000u;
}
static inline ssize_t fs_total(file_t f) {
    off_t here = fs_tell(f), end = fs_seek(f, 0, SEEK_END);
    fs_seek(f, here, SEEK_SET);
    return (ssize_t) end;
}

#endif
