/* NetHack 3.6	alloc.c	$NHDT-Date: 1454376505 2016/02/02 01:28:25 $  $NHDT-Branch: NetHack-3.6.0 $:$NHDT-Revision: 1.16 $ */
/* Copyright (c) Stichting Mathematisch Centrum, Amsterdam, 1985. */
/*-Copyright (c) Robert Patrick Rankin, 2012. */
/* NetHack may be freely redistributed.  See license for details. */

/*
 * NLE fast-reset variant: NetHack's allocator is replaced with a bump-pointer
 * arena allocator. All allocations live in a single contiguous region, so a
 * memcpy of that region trivially captures the entire NetHack heap state.
 *
 * free() is redirected (via global.h macro) to nle_arena_free(), which is a
 * no-op for arena pointers. Memory is reclaimed only at snapshot-restore
 * time, when the bump pointer rewinds to its saved position. Within a single
 * "episode" the arena grows monotonically; on restore it shrinks back to the
 * snapshot watermark.
 *
 * Non-arena pointers (e.g. from libc strdup() called in a save-recovery
 * path) are forwarded to libc free().
 */

#define ALLOC_C /* comment line for pre-compiled headers */
#define EXTERN_H /* comment line for pre-compiled headers */
#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

char *FDECL(fmt_ptr, (const genericptr));

long *FDECL(alloc, (unsigned int));
extern void VDECL(panic, (const char *, ...)) PRINTF_F(1, 2);

#ifdef NLE_USE_ARENA_FREE
#include <sys/mman.h>

/* Arena: 512 MB of address space, lazily backed by physical pages on first
 * touch. Bump-allocated. Aligned 16 bytes per allocation. */
#define NLE_ARENA_SIZE ((size_t) 64 * 1024 * 1024)
#define NLE_ARENA_ALIGN 16

/* Exported so nle_fast_reset.c can snapshot the live portion. */
char  *nle_arena_base = NULL;
size_t nle_arena_used = 0;
size_t nle_arena_cap  = 0;

static void
nle_arena_init(void)
{
    if (nle_arena_base)
        return;
    void *p = mmap(NULL, NLE_ARENA_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "nle_arena: mmap(%zu) failed\n", NLE_ARENA_SIZE);
        abort();
    }
    nle_arena_base = (char *) p;
    nle_arena_used = 0;
    nle_arena_cap  = NLE_ARENA_SIZE;
}

long *
alloc(lth)
register unsigned int lth;
{
    if (!nle_arena_base)
        nle_arena_init();
    size_t need = (lth + NLE_ARENA_ALIGN - 1) & ~(size_t)(NLE_ARENA_ALIGN - 1);
    if (need == 0)
        need = NLE_ARENA_ALIGN;
    if (nle_arena_used + need > nle_arena_cap) {
        panic("nle_arena: out of memory (used=%zu + req=%zu > cap=%zu)",
              nle_arena_used, need, nle_arena_cap);
    }
    void *ptr = nle_arena_base + nle_arena_used;
    nle_arena_used += need;
    return (long *) ptr;
}

/* Called by NetHack code via the `free` macro in global.h (non-MONITOR_HEAP
 * branch). Pointers inside the arena are leaked-until-restore; anything else
 * (rare, e.g. libc strdup in save recovery) is forwarded to libc free. */
void
nle_arena_free(void *ptr)
{
    if (!ptr)
        return;
    if (nle_arena_base
        && (char *) ptr >= nle_arena_base
        && (char *) ptr <  nle_arena_base + nle_arena_cap) {
        /* arena pointer: no-op. Reclaimed at snapshot restore. */
        return;
    }
    /* Non-arena pointer: forward to libc free. Use __libc_free to bypass
     * the `free` macro from global.h. */
    extern void __libc_free(void *);
    __libc_free(ptr);
}

#else /* !NLE_USE_ARENA_FREE */

/* Util binaries (makedefs, dgn_comp, lev_comp, dlb) reuse this file but link
 * with libc free. Provide the original libc-malloc-based alloc(). */
long *
alloc(lth)
register unsigned int lth;
{
#ifdef LINT
    long dummy = ftell(stderr);
    if (lth)
        dummy = 0;
    return &dummy;
#else
    register genericptr_t ptr;
    ptr = malloc(lth);
    if (!ptr)
        panic("Memory allocation failure; cannot get %u bytes", lth);
    return (long *) ptr;
#endif
}

#endif /* NLE_USE_ARENA_FREE */

#ifdef HAS_PTR_FMT
#define PTR_FMT "%p"
#define PTR_TYP genericptr_t
#else
#define PTR_FMT "%06lx"
#define PTR_TYP unsigned long
#endif

#define PTRBUFCNT 4
#define PTRBUFSIZ 32
static char ptrbuf[PTRBUFCNT][PTRBUFSIZ];
static __thread int ptrbufidx = 0;

char *
fmt_ptr(ptr)
const genericptr ptr;
{
    char *buf;

    buf = ptrbuf[ptrbufidx];
    if (++ptrbufidx >= PTRBUFCNT)
        ptrbufidx = 0;

    Sprintf(buf, PTR_FMT, (PTR_TYP) ptr);
    return buf;
}

/* strdup() which uses our alloc() rather than libc's malloc(); */
char *
dupstr(string)
const char *string;
{
    return strcpy((char *) alloc(strlen(string) + 1), string);
}

/*alloc.c*/
