#ifndef NLE_H
#define NLE_H

#define NLE_BZ2_TTYRECS

#include <stdio.h>

#include <fcontext/fcontext.h>

#include "nleobs.h"
#include "isaac64.h"

/* TODO: Fix this. */
#undef SIG_RET_TYPE
#define SIG_RET_TYPE void (*)(int)

typedef struct TMT TMT;

typedef struct nle_globals {
    fcontext_stack_t stack;
    fcontext_t returncontext;
    fcontext_t generatorcontext;

    FILE *ttyrec;
    TMT *vterminal;
    char outbuf[BUFSIZ];
    char *outbuf_write_ptr;
    char *outbuf_write_end;

#ifdef NLE_BZ2_TTYRECS
    void *ttyrec_bz2;
#endif

    boolean done;
    nle_obs *observation;

    /* nle_state refactor — RNG subsystem (first migration from rnd.c).
     * Storage was `static struct rnglist_t rnglist[2]` in rnd.c.
     * Accessed via nle_rng_state(idx) / nle_rng_init(idx) helpers. */
    isaac64_ctx rng_state[2];
    int rng_init[2]; /* boolean flag per rng; 0 = uninitialized */
} nle_ctx_t;

/*
 * Would like to annotate this with __thread, but that causes
 * the MacOS dynamic linker to not unload the library on dlclose().
 */
nle_ctx_t *current_nle_ctx;

nle_ctx_t *nle_start(nle_obs *, FILE *, nle_seeds_init_t *, nle_settings *);
nle_ctx_t *nle_step(nle_ctx_t *, nle_obs *);
void nle_end(nle_ctx_t *);

void nle_set_seed(nle_ctx_t *, unsigned long, unsigned long, boolean);
void nle_get_seed(nle_ctx_t *, unsigned long *, unsigned long *, boolean *);

/* nle_state refactor — per-instance accessors. Called from rnd.c (and
 * other subsystems as they migrate). Each returns a pointer into the
 * current nle_ctx_t. CORE = 0 (gameplay RNG), DISP = 1 (display RNG). */
isaac64_ctx *nle_rng_state(int idx);
int          *nle_rng_init_flag(int idx);

#endif /* NLE_H */
