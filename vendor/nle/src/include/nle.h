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

/* Forward declarations for migrated NetHack structs (stage 4 — player).
 * Real definitions in include/you.h, which is too heavy to pull into nle.h
 * (would force monst.h, prop.h, skills.h cascade into util binaries).
 * Use heap pointers in nle_ctx_t and let nle.c (which includes hack.h)
 * allocate them. */
struct you;
struct flag;             /* include/flag.h */
struct instance_flags;   /* include/flag.h */
struct sysflag;          /* include/flag.h, only #ifdef SYSFLAGS */

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
    const char         *multi_reason;       /* was decl.c ('Speed', 'Slowness', ...) */
    int                 occtime;            /* was decl.c (occupation duration) */
    /* stage 3f — level-building + input replay state */
    int                 nroom;              /* was decl.c (rooms on current level) */
    int                 nsubroom;           /* was decl.c (subrooms in shop/temple) */
    int                 doorindex;          /* was decl.c (doors[] write idx) */
    boolean             in_mklev;           /* was decl.c (inside mklev()) */
    int                 in_doagain;         /* was decl.c (input replay state) */
    /* stage 3g — combat / inventory transient flags */
    boolean             stoned;             /* was decl.c (monster being stoned) */
    boolean             unweapon;           /* was decl.c (player unwielded) */
    boolean             mrg_to_wielded;     /* was decl.c (merge picked-up to wield) */
    boolean             defer_see_monsters; /* was decl.c (suppress see_monsters refresh) */
    /* stage 3h — small misc globals */
    schar               tbx;                /* was decl.c (throw target x) */
    schar               tby;                /* was decl.c (throw target y) */
    int                 otg_temp;           /* was decl.c (object_to_glyph scratch) */
    long                yn_number;          /* was decl.c (last numeric y/n response) */
    /* stage 3i — trivial-scope globals */
    int                 locknum;            /* was decl.c (UNIX simultaneous-user count) */
    long                done_money;         /* was decl.c (cash at death) */
    int                 warn_obj_cnt;       /* was decl.c (warn-mon counter) */
    /* bhitpos (coord) defer to later — needs coord.h include in nle.h */
    /* stage 3j — turn loop state (big migration, ~40 callsites) */
    int                 multi;              /* was decl.c (multi-step action counter) */
    /* stage 4 — player state (the big one — ~94 files, hundreds of refs) */
    struct you         *u_ptr;              /* was 'struct you u' in decl.c */
    /* stage 5 — game options & flags (another macro-storm: wizard,
     * discover, use_color etc. expand to flags.X / iflags.X) */
    struct flag                  *flags_ptr;  /* was 'struct flag flags' */
    struct instance_flags        *iflags_ptr; /* was 'struct instance_flags iflags' */
    struct sysflag               *sysflags_ptr; /* was 'struct sysflag sysflags' */
    /* stage 6 — dungeon topology (multiple structs/arrays, context-switched
     * around each nle_step). The exact layout is internal to nle.c which has
     * the full type definitions. Opaque blob here. */
    void                         *dungeon_save;
} nle_ctx_t;

/*
 * Would like to annotate this with __thread, but that causes
 * the MacOS dynamic linker to not unload the library on dlclose().
 *
 * Refactor stage 3: declared extern here, defined once in nle.c. Was a
 * tentative-definition (common symbol) — that broke under ASan ODR after
 * many TUs started including nle.h.
 */
extern nle_ctx_t *current_nle_ctx;

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
