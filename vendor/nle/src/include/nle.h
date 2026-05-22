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
struct nle_dlevel;       /* include/rm.h     (stage 7' completion) */
struct cmd;              /* include/flag.h   (Cmd migration) */
struct context_info;     /* include/context.h (context migration) */
struct nle_rndmonst_state; /* makemon.c (rndmonst_state migration) */
struct artifact;         /* include/artifact.h (artilist migration) */
struct objclass;         /* include/objclass.h (objects migration) */
struct objdescr;         /* include/objclass.h (obj_descr migration) */
struct fruit;            /* include/youprop.h via hack.h — Cluster AU group 1 (restore.c oldfruit) */

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
    int                 doorindex_v;        /* macro: doorindex (cluster V) */
    boolean             in_mklev_v;         /* macro: in_mklev (cluster V) */
    int                 in_doagain_v;       /* macro: in_doagain (cluster V) */
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
    /* bhitpos — per-env throw/zap impact point. Stored as a pointer so
     * we can keep coord.h out of nle.h. Allocated in init_nle. */
    struct nhcoord      *bhitpos_p;
    /* cluster W — per-env player identity buffers. PL_NSIZ=32, PL_CSIZ=20,
     * PL_FSIZ=32. Allocated inline (small enough). */
    char                 plname_v[32];        /* PL_NSIZ */
    char                 pl_character_v[32];  /* PL_CSIZ */
    char                 pl_race_v;
    char                 pl_fruit_v[32];      /* PL_FSIZ */
    char                 tune_v[6];
    /* cluster X — pet name buffers and a couple of pointers. */
    char                 dogname_v[63];       /* PL_PSIZ */
    char                 catname_v[63];
    char                 horsename_v[63];
    struct fruit        *ffruit_v;
    char                *save_cm_v;
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
    /* stage 7' completion — `dlevel_t level` (40 KB, the largest single
     * global) heap-allocated per-env. The macro pattern works now that
     * `struct dig_info.level` was renamed to `.dlvl` in context.h.
     * Forward-declared `struct nle_dlevel` (tag added in rm.h). */
    struct nle_dlevel   *s7_level_p;          /* macro: level */
    /* Cmd — command bindings (struct cmd in flag.h). */
    struct cmd          *s5_cmd_p;            /* macro: Cmd */
    /* small file-local-static migrations (each defined as a macro
     * inside the .c file that owns the global). */
    short               *s_disco_p;           /* o_init.c disco[NUM_OBJECTS] */
    char                *s_obufs_p;           /* objnam.c obufs[NUMOBUF][BUFSZ] */
    char                 s_prevmsg[256];      /* pline.c prevmsg[BUFSZ=256] */
    void                *s_tty_status_p;      /* wintty.c tty_status[2][MAXBLSTATS] */
    unsigned long       *s_tty_colormasks;    /* wintty.c tty_colormasks */
    long                 s_tty_condition_bits; /* wintty.c tty_condition_bits */
    int                  s_hpbar_percent;     /* wintty.c hpbar_percent */
    int                  s_hpbar_color;       /* wintty.c hpbar_color */
    struct context_info *s_context_p;         /* macro: context */
    struct nle_rndmonst_state *s_rndmonst_state_p; /* makemon.c rndmonst_state */
    struct artifact     *s_artilist_p;        /* artifact.c artilist[] */
    struct objclass     *s9o_objects_p;       /* macro: objects (NUM_OBJECTS entries) */
    struct objdescr     *s9o_obj_descr_p;     /* macro: obj_descr (NUM_OBJECTS entries) */
    /* per-env status-line state (windows.c / wintty.c). MAXBLSTATS=23. */
    const char          *s_status_fieldnm[23];
    const char          *s_status_fieldfmt[23];
    char                *s_status_vals[23];
    boolean              s_status_activefields[23];
    /* per-env "name buffers" pool used by do_name.c nextmbuf().
     * NUMMBUF=5, BUFSZ=256 → 1280 bytes flat. */
    char                *s_mbufs_p;
    int                  s_mbuf_idx;
    /* per-env room-equivalence work array (decl.c smeq[]). */
    int                 *s_smeq_p;            /* size MAXNROFROOMS+1 */
    /* per-env object-class base-index table (decl.c bases[MAXOCLASSES]). */
    int                 *s_bases_p;
    /* per-env terminal color escapes (tty/termcap.c hilites[CLR_MAX]). */
    char               **s_hilites_p;
    /* display buffer (display.c gbuf_entry[ROWNO][COLNO]) + bookkeeping. */
    void                *s_gbuf_p;            /* malloc'd ROWNO*COLNO*sizeof(gbuf_entry) */
    char                 s_gbuf_start[21];    /* ROWNO=21 */
    char                 s_gbuf_stop[21];
    /* bottom-line stats (botl.c blstats[2][MAXBLSTATS]) + flags. */
    void                *s_blstats_p;
    boolean              s_blinit;
    boolean              s_update_all;
    /* cluster Z: per-env once-per-game init flags (formerly file-scope
     * static booleans that tripped in shared-libnethack vecenv when env 2
     * inherited env 1's TRUE state). */
    char                 s_blstats_initalready; /* botl.c init_blstats */
    /* cluster AA: vision.c transient computation state. These are set at
     * the top of view_from() and used by left_side/right_side recursively.
     * If a vecenv env yields mid-vision_recalc, another env will clobber
     * the statics, breaking the recursion → infinite loop. Moved per-env.
     * (genericptr_t typed as void* via opaque cast.) */
    int                  s_vis_start_row;
    int                  s_vis_start_col;
    int                  s_vis_step;
    char               **s_vis_cs_rows;
    char                *s_vis_cs_left;
    char                *s_vis_cs_right;
    void               (*s_vis_func)();
    void                *s_vis_varg;
    /* cluster AB: timeout.c timer queue — was __thread, broken under vecenv
     * because all envs share one thread; env A's timers fire while env B
     * holds the globals → "extract_nexthere: object lost" panic. */
    void                *s_timer_base;            /* timer_element * */
    unsigned long        s_timer_id;
    /* cluster AG: light source list head (light.c light_base). Was
     * __thread; under vecenv env A's lights leaked into env B's
     * vision_recalc → impossible objects on the wrong levels →
     * eventual cascade in left_ptrs causing infinite recursion. */
    void                *s_light_base;            /* light_source * */
    /* cluster AH: deferred-goto messages (do.c). Were __thread;
     * env A schedules level change with messages, env B's deferred_goto
     * sees A's leftover strings (now potentially dangling). */
    char                *s_dfr_pre_msg;
    char                *s_dfr_post_msg;
    /* cluster AI: region.c per-env region table (gas clouds, force-fields).
     * Was process-global (regions) + __thread (n/max). Cross-env contamination
     * was severe — env A's gas cloud could be applied to env B's monsters. */
    void                *s_regions;       /* NhRegion ** */
    int                  s_n_regions;
    int                  s_max_regions;
    /* cluster AJ: assorted small __thread to per-env. */
    unsigned             s_pline_flags;
    int                  s_polearm_range_min;
    int                  s_polearm_range_max;
    int                  s_lastinvnr;       /* invent.c menu nrf */
    int                  s_bcrestriction;   /* ball/chain */
    int                  s_mkot_trap_warn_count;
    /* cluster AK: function-local static recursion guards (pline.c, hack.c). */
    int                  s_pline_in_pline;
    int                  s_inspoteffects;
    int                  s_artifact_nesting;
    /* cluster AL: vision recursion depth guard. */
    int                  s_vision_recur_depth;
    /* cluster AD: vision.c viz_rmin/viz_rmax. Set during vision_recalc;
     * if env A yields mid-recalc, env B overwrites these. (viz_array
     * itself already moved to nle_ctx_t->vision_array in stage 8'.) */
    char                *s_viz_rmin;
    char                *s_viz_rmax;
    boolean              s_valset[23];        /* MAXBLSTATS */
    void                *s_status_hilites_p;
    /* vision work buffers (vision.c). */
    void                *s_could_see_p;
    void                *s_viz_clear_p;
    void                *s_left_ptrs_p;
    void                *s_right_ptrs_p;
    char                *s_cs_rows0[21];
    char                *s_cs_rows1[21];
    char                 s_cs_rmin0[21];
    char                 s_cs_rmax0[21];
    char                 s_cs_rmin1[21];
    char                 s_cs_rmax1[21];
    char                *s_viz_clear_rows[21];
    /* special-level position map (sp_lev.c). */
    void                *s_SpLev_Map_p;
    /* file-path scratch buffers (files.c fqn_filename_buffer[FQN_NUMBUF][FQN_MAX_FILENAME]). */
    void                *s_fqn_fname_p;       /* 2048 bytes */
    /* per-env worm tables (worm.c). MAX_NUM_WORMS=32. */
    void                *s_wheads_p;
    void                *s_wtails_p;
    void                *s_wgrowtime_p;
    /* options.c boolopt[] — per-env copy of the boolean-options
     * table. Baseline is const in options.c; init_nle calloc's a
     * mutable slot, options.c seeds it from baseline at game init. */
    void                *s_boolopt_p;
    void                *s_compopt_p;
    /* per-env role/race description (role.c urole/urace). */
    void                *s_urole_p;
    void                *s_urace_p;
    /* Cluster AM: per-env NetHackRL singleton (winrl.cc).
     * Was `static thread_local std::unique_ptr<NetHackRL> instance`. Under
     * PufferLib's OMP-parallel cpu_vec_step, worker threads have a null
     * thread_local instance and segfault in rl_nhgetch. Owned by this
     * pointer; nle_end deletes it via NetHackRL::destroy_for_ctx(). */
    void                *s_netHackRL_instance;
    /* Cluster AM: per-env win-procedure trace deque (winrl.cc).
     * Was `thread_local std::deque<std::string> win_proc_calls`. Same OMP
     * coroutine-resume hazard as s_netHackRL_instance: push happens on
     * init thread, pop on worker thread → empty-deque pop_back UB. Owned
     * by this pointer; nle_end frees it via NetHackRL::destroy_for_ctx(). */
    void                *s_win_proc_calls;
    /* Cluster AN: per-env tty backend state (win/tty/*.c).
     * Each file owns its own struct; void* here so nle.h doesn't have
     * to pull in MAX_PER_ROW, BUFSIZ, enum statusfields. Owned by the
     * respective .c file's accessor; nle_end frees via nle_tty_destroy_for_ctx().
     *
     * wintty.c:  obuf / clipping / clipx,y / vt_tile_current_window /
     *            fieldorder / finalx / windowdata_init / cond_shrinklvl /
     *            enclev,enc_shrinklvl / dlvl_shrinklvl / truncation_expected /
     *            do_field_opt
     * topl.c:    snapshot_mesgs
     * termcap.c: KS, KE
     *
     * Was `static __thread X foo`. Cross-thread coroutine resume saw the
     * worker pthread's TLS slots empty after the env was init'd on the
     * main thread, triggering jump_fcontext+103 SIGSEGV. Now per-env so
     * resume on any thread reads the same env state. */
    void                *s_wintty_state;
    void                *s_topl_state;
    void                *s_termcap_state;
    /* Cluster AO: per-env src-file local state. Each `void*` is owned by
     * the corresponding .c file; lazy-alloced through a file-local
     * accessor that resolves via current_nle_ctx. Same pattern as the
     * tty group above. Frees in nle_end. */
    void                *s_pline_state;       /* pline.c: you_buf / you_buf_siz */
    void                *s_save_state;        /* save.c: bw_fd / buffering */
    void                *s_files_state;       /* files.c: nesting / lockfd / config_error_data / symset_* */
    void                *s_objnam_state;      /* objnam.c: obufidx / distantname */
    void                *s_uhitm_state;       /* uhitm.c: override_confirmation */
    void                *s_shk_state;         /* shk.c: auto_credit */
    void                *s_end_state;         /* end.c: Schroedingers_cat */
    void                *s_sounds_state;      /* sounds.c: soundmap */
    void                *s_fast_reset_state;  /* nle_fast_reset.c: nle_arena_base */
    /* Stage 9' batch D — body-slot pointers.  Were TLS NEARDATA in decl.c,
     * pinned there by worn[] referencing &uarm etc. at static-init time.
     * Now live per-env on nle_ctx_t; worn[] uses byte-offset resolution.
     * Prefixed s9_ to match the batch naming used for other stage-9' work. */
    struct obj          *s9_uarm;
    struct obj          *s9_uarmc;
    struct obj          *s9_uarmh;
    struct obj          *s9_uarms;
    struct obj          *s9_uarmg;
    struct obj          *s9_uarmf;
    struct obj          *s9_uarmu;
    struct obj          *s9_uleft;
    struct obj          *s9_uright;
    struct obj          *s9_uwep;
    struct obj          *s9_uswapwep;
    struct obj          *s9_uquiver;
    struct obj          *s9_uamul;
    struct obj          *s9_ublindf;
    struct obj          *s9_uball;
    struct obj          *s9_uchain;
    /* Cluster AP: botl.c per-env status state.
     * cond_hilites[] was a plain static (process-global) unsigned long array;
     * it holds condition highlight masks computed per-env during render_status.
     * bl_hilite_moves and now_or_before_idx were __thread; broken under OMP
     * vecenv for the same coroutine-resume reason as the Cluster AN group.
     * status_hilite_str / status_hilite_str_id were __thread linked-list
     * head+id; thread-local values are zero on worker threads after env was
     * init'd on main thread, so the list is lost and allocs leak. */
    unsigned long        s_cond_hilites[21]; /* BL_ATTCLR_MAX = CLR_MAX(16)+5 */
    long                 s_bl_hilite_moves;  /* botl.c bl_hilite_moves */
    int                  s_now_or_before_idx; /* botl.c now_or_before_idx */
    void                *s_status_hilite_str_p; /* botl.c status_hilite_str */
    int                  s_status_hilite_str_id; /* botl.c status_hilite_str_id */
    /* Cluster AP: cmd.c per-env key-input queues.
     * pushq/saveq/phead/ptail/shead/stail were plain statics (process-global);
     * concurrent OMP envs sharing one thread could interleave reads/writes
     * from different envs' input replay sequences. */
    char                 s_pushq[20];       /* cmd.c pushq[BSIZE], BSIZE=20 */
    char                 s_saveq[20];       /* cmd.c saveq[BSIZE] */
    int                  s_phead;           /* cmd.c phead */
    int                  s_ptail;           /* cmd.c ptail */
    int                  s_shead;           /* cmd.c shead */
    int                  s_stail;           /* cmd.c stail */
    /* Cluster AP: wintty.c/getline.c per-env scratch.
     * compress_str() cbuf was a function-local static used by tty_putstr
     * on every message output — a hot per-env buffer shared across envs.
     * tty_nhgetch nesting was __thread; marks re-entrant getc under UNIX.
     * suppress_history in getline.c was a plain STATIC_VAR (process-global). */
    char                 s_compress_cbuf[256]; /* wintty.c compress_str cbuf, BUFSZ=256 */
    int                  s_tty_nhgetch_nesting; /* wintty.c tty_nhgetch nesting */
    boolean              s_suppress_history; /* getline.c suppress_history */
    /* Cluster AP: cmd.c enlightenment-window state.
     * en_win was a plain static (process-global winid); concurrent envs
     * both running enlightenment (e.g. at game-over) would race on it.
     * en_via_menu was __thread; OMP cross-thread resume hazard. */
    short                s_en_win;           /* cmd.c en_win (winid=short) */
    boolean              s_en_via_menu;      /* cmd.c en_via_menu */
    /* Cluster AP Part 2: remaining functional __thread variables.
     * Each was __thread (broken under OMP coroutine-resume) or a plain
     * process-global static (racy under concurrent envs). */
    /* rumors.c oracle state — __thread (flg/loc) or plain static (cnt).
     * oracle_cnt is decremented as oracles are used, so it must be per-env. */
    int                  s_oracle_flg;       /* rumors.c oracle_flg */
    unsigned long       *s_oracle_loc;       /* rumors.c oracle_loc (heap ptr) */
    unsigned             s_oracle_cnt;       /* rumors.c oracle_cnt */
    /* do_wear.c initial_don — __thread; per-env flag for startup auto-wear */
    boolean              s_initial_don;
    /* sp_lev.c special-level generation state — __thread; each env's level
     * gen is independent. container_obj[] is already a plain static (not TLS),
     * so container_idx (index into it) must be per-env to avoid aliasing. */
    boolean              s_splev_init_present;
    boolean              s_icedpools;
    int                  s_container_idx;
    /* spell.c sort state — __thread; sort mode and index array per-env. */
    int                  s_spl_sortmode;
    int                 *s_spl_orderindx;    /* heap ptr, NULL=not alloced */
    /* restore.c ID-mapping state — __thread; used during savefile restore. */
    int                  s_n_ids_mapped;
    void                *s_id_map;           /* struct bucket *, heap */
    /* eat.c eatmbuf — __thread; allocated string for mimic-eating feedback. */
    char                *s_eatmbuf;          /* heap ptr, NULL=none */
    /* options.c n_menu_mapped — __thread; count of mapped menu cmds per env. */
    short                s_n_menu_mapped;
    /* windows.c last_winchoice — __thread; window-system choice during init;
     * used only at startup, but must be per-env if envs init concurrently. */
    void                *s_last_winchoice;   /* struct win_choices * */
    /* Cluster AQ: makemon.c align_shift() per-env cache.
     * oldmoves and lev were `static NEARDATA` (plain process-global) inside
     * align_shift(). Two OMP threads in makemon() simultaneously race on
     * the oldmoves/lev update, corrupting lev and causing a SIGSEGV when
     * one thread dereferences the other env's stale s_level pointer.
     * NOTE: `s_level` is not declared in nle.h; use void* + cast in .c. */
    long                 s_align_shift_oldmoves; /* makemon.c align_shift oldmoves */
    void                *s_align_shift_lev;      /* makemon.c align_shift lev (s_level*) */

    /* Cluster AT-C: per-env dungeon graph + level-builder + key-cmd state.
     * Was: process-global mutable in dungeon.c/mklev.c/do.c/decl.c. With
     * N envs in one process, env A's dungeon graph would be walked by env
     * B's level transition code, causing the save_room(r=NULL) crash. */
    void                         *s_branches;          /* branch * (dungeon.c) */
    int                           s_branch_id_ctr;     /* dungeon.c add_branch */
    void                         *s_mapseenchn;        /* mapseen * (dungeon.c) */
    int                           s_n_dgns;            /* dungeon.c */
    signed char                   s_vault_x;           /* xchar (mklev.c) */
    signed char                   s_vault_y;           /* xchar (mklev.c) */
    char                          s_made_branch;       /* boolean (mklev.c) */
    char                          s_at_ladder;         /* boolean (do.c) */
    void                         *s_save_cm;           /* struct ext_func_tab * (decl.c) */
    /* Cluster AT-B: per-env level/save filename buffers and prefix table.
     * Was: process-global `char lock[PL_NSIZ+14]`, `char SAVEF[SAVESIZE]`,
     * `char bones[]`, `char *fqn_prefix[PREFIX_COUNT]` in files.c/decl.c.
     * With N envs in one process, all envs collided on the same buffer:
     * getlock() builds lock="<uid><plname>" (same for every env's wizard
     * default), and fqn_prefix[] was set once per env at mainloop entry
     * then overwritten by the next env. Result: env j writes its level
     * file over env i's; env i later mreads a torn level and panics.
     * Sizes match compile-time arrays in files.c (PL_NSIZ=32). */
    char                          s_lock[46];      /* PL_NSIZ+14 */
    char                          s_SAVEF[45];     /* SAVESIZE = PL_NSIZ+13 (UNIX) */
    char                          s_bones[16];    /* "bonesnn.xxx" + slack */
    char                         *s_fqn_prefix[10]; /* PREFIX_COUNT */

    /* Cluster AU group 1 — save/restore session state (save.c + restore.c).
     * Eleven file-statics that race across envs during c_reset save paths.
     * Direct fields on nle_ctx_t (no swap struct); macros at the top of
     * each .c file rewrite accesses to current_nle_ctx->s_<name>.
     *
     * Note: s_outbuf is sized 256 here because nle.h is included by util
     * binaries that don't pull in hack.h (where BUFSZ=256 is defined).
     * A _Static_assert in save.c enforces BUFSZ == 256 / ZEROCOMP_BUFSIZ
     * to catch any future config drift. */
    int                  s_count_only;            /* save.c (MFLOPPY-gated; harmless on UNIX) */
    unsigned             s_ustuck_id;             /* save.c (preserve monster id across save) */
    unsigned             s_usteed_id;             /* save.c (preserve steed id across save) */
    FILE                *s_bw_FILE;               /* save.c (def_bufon fdopen'd save stream) */
    unsigned char        s_outbuf[256];           /* save.c (BUFSZ == ZEROCOMP_BUFSIZ on UNIX) */
    unsigned short       s_outbufp;               /* save.c (zerocomp output cursor) */
    short                s_outrunlength;          /* save.c (RLE run len; -1 == no run) */
    int                  s_bwritefd;              /* save.c (zerocomp active fd) */
    boolean              s_compressing;           /* save.c (zerocomp mode flag) */
    struct fruit        *s_oldfruit;              /* restore.c (ghost-level fruit chain) */
    long                 s_omoves;                /* restore.c (ghost-level monstermoves) */
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
