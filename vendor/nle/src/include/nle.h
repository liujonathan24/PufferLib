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

/* `struct sinfo` was defined inline at the variable declaration in
 * decl.h. Moved here for the refactor (stage 3b) so nle_ctx_t can host
 * a per-instance copy. The original macro storm in decl.h is replaced
 * by direct current_nle_ctx->program_state.X access at each callsite. */
struct sinfo {
    int gameover;  /* self explanatory? */
    int stopprint; /* inhibit further end of game disclosure */
#ifdef HANGUPHANDLING
    volatile int done_hup; /* SIGHUP or moral equivalent received
                            * -- no more screen output */
    int preserve_locks;    /* don't remove level files prior to exit */
#endif
    int something_worth_saving; /* in case of panic */
    int panicking;              /* `panic' is in progress */
    int exiting;                /* an exit handler is executing */
    int in_moveloop;
    int in_impossible;
#ifdef PANICLOG
    int in_paniclog;
#endif
    int wizkit_wishing;
};

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

    /* nle_state refactor — RNG subsystem (stage 1, was static rnglist
     * in rnd.c). Accessed via nle_rng_state(idx) / nle_rng_init_flag(idx). */
    isaac64_ctx rng_state[2];
    int rng_init[2]; /* boolean flag per rng; 0 = uninitialized */

    /* nle_state refactor — NLE wrapper layer (stage 2). Moved out of
     * file-scope statics in nle.c (`settings`, `nle_seeds_init`) and
     * hacklib.c (`nle_seeds`). seeds[] tracks the last seed set via
     * set_random() for inspection via nle_get_seed(). */
    nle_settings        settings;
    nle_seeds_init_t   *seeds_init;
    unsigned long       seeds[2];
    boolean             has_strong_rngseed; /* was NEARDATA in decl.c */
    struct sinfo        program_state;      /* was NEARDATA in decl.c */
    int                 hackpid;            /* was 'int hackpid' in decl.c */
    boolean             restoring;          /* was decl.c flag (savefile) */
    boolean             ransacked;          /* was decl.c flag (mkmaze) */
    boolean             in_steed_dismounting; /* was decl.c flag (steed) */
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
