#ifndef EB_ALLOC_OVERRIDE_H
#define EB_ALLOC_OVERRIDE_H

/*
 * Force-included by every EarthBound translation unit (via -include in
 * C_DEFS_EARTHBOUND). Routes lakesnes's malloc/free calls (apu.c, spc.c,
 * dsp.c, statehandler.c — ~70 KB total at audio_init) through the RAM_EMU
 * bump allocator instead of newlib's 85 KB DTCM heap, which the launcher,
 * FatFs, and stdio already share.
 *
 * EB src/ outside lakesnes does not call malloc/free directly (verified),
 * so the override is effectively scoped to those four files.
 *
 * stdlib.h / string.h / stdio.h are pulled in BEFORE the macros so their
 * own malloc/free function declarations aren't textually mangled by the
 * substitution; the include guards make subsequent #includes no-ops, so
 * the macros stay active for all user code.
 *
 * Bump allocator semantics: free() is a no-op because RAM_EMU is torn
 * down wholesale when the launcher returns to the menu — there is no
 * inter-game persistence to leak into.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "gw_malloc.h"

static inline void eb_free_noop(void *p) { (void)p; }

#define malloc(n) ram_malloc(n)
#define free(p)   eb_free_noop(p)

#endif /* EB_ALLOC_OVERRIDE_H */
