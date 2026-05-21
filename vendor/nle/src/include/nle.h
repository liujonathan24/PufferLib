#ifndef NLE_H
#define NLE_H

#define NLE_BZ2_TTYRECS

#include <stdio.h>
#include <time.h>  /* time_t for stage 9' ubirthday */

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
struct WinDesc;          /* include/wintty.h (stage 10') */
struct DisplayDesc;      /* include/wintty.h (stage 10') */
struct obj;              /* include/obj.h (stage 9') */
struct monst;            /* include/monst.h (stage 9') */
struct autopickup_exception; /* include/decl.h (stage 9') */
struct dgn_topology;     /* include/decl.h (stage 6') */
struct dungeon;          /* include/dungeon.h (stage 6') */
struct s_level;          /* include/dungeon.h (stage 6') */
struct stairway;         /* include/dungeon.h (stage 6') */
struct dest_area;        /* include/dungeon.h (stage 6') */
struct nhcoord;          /* include/coord.h (stage 6'); typedef'd as `coord` */
struct multishot;        /* include/decl.h (stage 9' batch C) */
struct u_realtime;       /* include/you.h (stage 9' batch C) */
struct q_score;          /* include/quest.h (stage 9' batch C) */
struct spell;            /* include/spell.h (stage 9' batch C) */
struct nle_mvitals_t;    /* include/decl.h (stage 9' batch C) */
struct kinfo;            /* include/decl.h (stage 9' batch C) */
struct nle_tcap_t;       /* include/decl.h (stage 8' completion) */
struct mkroom;           /* include/mkroom.h (stage 7' partial) */
struct linfo;            /* include/dungeon.h (stage 7' partial) */
struct trap;             /* include/trap.h   (stage 7' partial) */

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
    /* stage 10' — TTY window port state (was wintty.c/getline.c globals).
     * Field names are distinct from the public macros (see wintty.h) so
     * that those macros don't accidentally expand inside this struct
     * declaration — hack.h includes wintty.h before any TU includes nle.h.
     * Same trick as `u_ptr` field vs `u` macro (stage 4). */
    int                  base_window;     /* macro: BASE_WINDOW */
    struct WinDesc      *tty_wins[20];    /* macro: wins; MAXWIN=20 */
    struct DisplayDesc  *tty_display;     /* macro: ttyDisplay */
    char                 tty_morc;        /* macro: morc */
    /* stage 8' — display / message state (was decl.c globals).
     * Field names distinct from macros for the same hack.h-pulls-decl.h
     * -before-nle.h reason as stage 10'. tc_gbl_data deferred (struct-tag
     * self-reference; addressed in the collision batch). */
    int                  win_message;     /* macro: WIN_MESSAGE */
    int                  win_status;      /* macro: WIN_STATUS */
    int                  win_map;         /* macro: WIN_MAP */
    int                  win_inven;       /* macro: WIN_INVEN */
    char                 vision_recalc;   /* macro: vision_full_recalc */
    char               **vision_array;    /* macro: viz_array */
    char                 top_lines[300];  /* macro: toplines; TBUFSZ=300 */
    /* stage 9' batch A — scalars (no struct cascade). */
    time_t               nle_ubirthday;       /* macro: ubirthday */
    long                 nle_moves;           /* macro: moves */
    long                 nle_monstermoves;    /* macro: monstermoves */
    long                 nle_wailmsg;         /* macro: wailmsg */
    long                 nle_domove_attempting; /* macro: domove_attempting */
    long                 nle_domove_succeeded;  /* macro: domove_succeeded */
    /* stage 9' batch B — already-pointers; just relocate. */
    struct obj          *invent_p;            /* macro: invent */
    struct obj          *uskin_p;             /* macro: uskin */
    struct obj          *current_wand_p;      /* macro: current_wand */
    struct obj          *thrownobj_p;         /* macro: thrownobj */
    struct obj          *kickedobj_p;         /* macro: kickedobj */
    struct obj          *migrating_objs_p;    /* macro: migrating_objs */
    struct obj          *billobjs_p;          /* macro: billobjs */
    struct monst        *mydogs_p;            /* macro: mydogs */
    struct monst        *migrating_mons_p;    /* macro: migrating_mons */
    struct autopickup_exception *apelist_p;   /* macro: apelist */
    /* stage 6' — dungeon topology (heap-allocated to avoid dungeon.h
     * cascade in nle.h). All allocated in init_nle. */
    struct dgn_topology *s6_topology_p;       /* macro: dungeon_topology */
    struct dungeon      *s6_dungeons_p;       /* macro: dungeons (array head) */
    struct s_level      *s6_sp_levchn;        /* macro: sp_levchn */
    struct stairway     *s6_upstair_p;        /* macro: upstair */
    struct stairway     *s6_dnstair_p;        /* macro: dnstair */
    struct stairway     *s6_upladder_p;       /* macro: upladder */
    struct stairway     *s6_dnladder_p;       /* macro: dnladder */
    struct stairway     *s6_sstairs_p;        /* macro: sstairs */
    struct dest_area    *s6_updest_p;         /* macro: updest */
    struct dest_area    *s6_dndest_p;         /* macro: dndest */
    struct nhcoord      *s6_inv_pos_p;        /* macro: inv_pos */
    /* stage 9' batch C — heap-allocated per-env. Each ctor calloc's in
     * init_nle; nle_end frees. Replaces the dungeon_save round-trip. */
    struct multishot    *s9c_m_shot_p;        /* macro: m_shot */
    struct u_realtime   *s9c_urealtime_p;     /* macro: urealtime */
    struct q_score      *s9c_quest_status_p;  /* macro: quest_status */
    struct spell        *s9c_spl_book_p;      /* macro: spl_book (array head) */
    struct monst        *s9c_youmonst_p;      /* macro: youmonst */
    struct nle_mvitals_t *s9c_mvitals_p;      /* macro: mvitals (array head) */
    struct kinfo        *s9c_killer_p;        /* macro: killer */
    struct nle_tcap_t   *s8_tcap_p;           /* macro: tc_gbl_data */
    /* stage 7' partial — easy items from the current-level swap bundle
     * that don't have the `level` token-collision problem. All heap-
     * allocated in init_nle. */
    struct mkroom       *s7_rooms_p;          /* macro: rooms; size (MAXNROFROOMS+1)*2 */
    struct nhcoord      *s7_doors_p;          /* macro: doors; size DOORMAX */
    struct linfo        *s7_level_info_p;     /* macro: level_info; size MAXLINFO */
    schar               *s7_lastseentyp_p;    /* macro: lastseentyp; size COLNO*ROWNO */
    struct mkroom       *s7_subrooms;         /* macro: subrooms (just a pointer) */
    struct mkroom       *s7_upstairs_room;    /* macro: upstairs_room */
    struct mkroom       *s7_dnstairs_room;    /* macro: dnstairs_room */
    struct mkroom       *s7_sstairs_room;     /* macro: sstairs_room */
    struct trap         *s7_ftrap;            /* macro: ftrap */
} nle_ctx_t;

/*
 * Refactor stage 3: declared extern here, defined once in nle.c. Was a
 * tentative-definition (common symbol) — that broke under ASan ODR after
 * many TUs started including nle.h.
 *
 * Stage 10'+: __thread enables OMP-parallel stepping. Each thread chases
 * its own nle_ctx_t through this pointer; since all per-env state lives
 * in nle_ctx_t (after stages 1-10 + 5'-10' migrations), threads are
 * naturally isolated — no shared mutable globals to race on.
 *
 * macOS caveat: __thread on a dynamically-loaded library prevents
 * dlclose() from unloading. Not applicable for our Linux/HPC target;
 * if we ever need macOS dynamic-unload, gate this with #ifndef __APPLE__.
 */
extern __thread nle_ctx_t *current_nle_ctx;

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
