
#include <assert.h>
#include <string.h>
#include <sys/time.h>

#include <tmt.h>

#define NEED_VARARGS
#ifdef MONITOR_HEAP
#undef MONITOR_HEAP
#endif
#include "hack.h"

#include "dlb.h"

#include "nle.h"

/* Single definition of current_nle_ctx; declared extern in nle.h. */
nle_ctx_t *current_nle_ctx;

#ifdef NLE_BZ2_TTYRECS
#include <bzlib.h>
#endif

#define STACK_SIZE (1 << 16) /* 64KiB */

#ifndef __has_feature
#define __has_feature(x) 0 /* Compatibility with non-clang compilers. */
#endif

#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#endif

extern int unixmain(int, char **);

signed char
vt_char_color_extract(TMTCHAR *c)
{
    /* We pick out the colors in the enum tmt_color_t. These match the order
     * found standard in IBM color graphics, and are the same order as those
     * found in src/color.h.  */

    /* TODO: We no longer need *signed* chars. Let's change the dtype of
     * tty_chars when we change the API next. */

    signed char color;

    if (c->a.fg == TMT_COLOR_DEFAULT) {
        /* Need to make a choice for default color. To stay compatible with
           NetHack, choose black for the "null glyph", gray otherwise. */
        color = (c->c == ' ') ? CLR_BLACK : CLR_GRAY; /* 0 or 7 */
    } else if (c->a.fg < TMT_COLOR_MAX) {
        color = c->a.fg - TMT_COLOR_BLACK + CLR_BLACK; /* TMT color offset. */
        if (c->a.bold) {
            color |= BRIGHT;
        }
    } else {
        fprintf(stderr, "Illegal color %d\n", (int) c->a.fg);
        color = CLR_GRAY;
    }

    /* The above is 0..15. For "reverse" colors (bg/fg swap), let's
     * use 16..31. */
    if (c->a.reverse) {
        color += CLR_MAX;
    }
    return color;
}

void
nle_vt_callback(tmt_msg_t m, TMT *vt, const void *a, void *p)
{
    const TMTSCREEN *s = tmt_screen(vt);
    const TMTPOINT *cur = tmt_cursor(vt);

    nle_ctx_t *nle = (nle_ctx_t *) p;
    if (!nle || !nle->observation) {
        return;
    }

    switch (m) {
    case TMT_MSG_BELL:
        break;

    case TMT_MSG_UPDATE:
        for (size_t r = 0; r < s->nline; r++) {
            if (s->lines[r]->dirty) {
                for (size_t c = 0; c < s->ncol; c++) {
                    size_t offset = (r * NLE_TERM_CO) + c;
                    TMTCHAR *tmt_c = &(s->lines[r]->chars[c]);

                    if (nle->observation->tty_chars) {
                        nle->observation->tty_chars[offset] = tmt_c->c;
                    }

                    if (nle->observation->tty_colors) {
                        nle->observation->tty_colors[offset] =
                            vt_char_color_extract(tmt_c);
                    }
                }
            }
        }
        tmt_clean(vt);
        break;

    case TMT_MSG_ANSWER:
        break;

    case TMT_MSG_MOVED:
        if (nle->observation->tty_cursor) {
            /* cast from size_t is safe from overflow, since r,c < 256 */
            nle->observation->tty_cursor[0] = (unsigned char) cur->r;
            nle->observation->tty_cursor[1] = (unsigned char) cur->c;
        }
        break;

    case TMT_MSG_CURSOR:
        break;
    }
}

/* nle_state refactor: helpers for moving subsystems out of process-global
 * storage into nle_ctx_t. Stage 1 ports the RNG state (was static rnglist
 * in rnd.c). Call from non-NLE TUs via the prototypes declared in nle.h. */
isaac64_ctx *
nle_rng_state(int idx)
{
    return &current_nle_ctx->rng_state[idx];
}

int *
nle_rng_init_flag(int idx)
{
    return &current_nle_ctx->rng_init[idx];
}

nle_ctx_t *
init_nle(FILE *ttyrec, nle_obs *obs)
{
    nle_ctx_t *nle = calloc(1, sizeof(nle_ctx_t));

    nle->ttyrec = ttyrec;

#ifdef NLE_BZ2_TTYRECS
    if (nle->ttyrec) {
        int bzerror;
        nle->ttyrec_bz2 = BZ2_bzWriteOpen(&bzerror, ttyrec, 9, 0, 0);
        assert(bzerror == BZ_OK);
    }
#endif

    nle->observation = obs;

    TMT *vterminal = tmt_open(LI, CO, nle_vt_callback, nle, NULL, true);
    assert(vterminal);
    nle->vterminal = vterminal;

    nle->outbuf_write_ptr = nle->outbuf;
    nle->outbuf_write_end = nle->outbuf + sizeof(nle->outbuf);

    /* RNG state cleared by calloc; init_isaac64 will populate it via the
     * set_random() / init_random() chain during NetHack's early setup. */

    /* Stage 4 player state: allocate the struct you on the heap so the
     * `u` macro in decl.h can resolve to (*current_nle_ctx->u_ptr). */
    nle->u_ptr = (struct you *) calloc(1, sizeof(struct you));
    if (!nle->u_ptr) {
        fprintf(stderr, "init_nle: failed to allocate struct you\n");
        abort();
    }

    /* Stage 5 flags / iflags / sysflags */
    nle->flags_ptr = (struct flag *) calloc(1, sizeof(struct flag));
    nle->iflags_ptr = (struct instance_flags *) calloc(1, sizeof(struct instance_flags));
#ifdef SYSFLAGS
    nle->sysflags_ptr = (struct sysflag *) calloc(1, sizeof(struct sysflag));
#endif
    if (!nle->flags_ptr || !nle->iflags_ptr) {
        fprintf(stderr, "init_nle: failed to allocate flags/iflags\n");
        abort();
    }

    return nle;
}

/* `settings` moved into nle_ctx_t (refactor stage 2). Below uses
 * `current_nle_ctx->settings` since mainloop and friends always run
 * with current_nle_ctx anchored to the active env. */

/* TODO: Consider copying the relevant parts of main() in unixmain.c. */
void
mainloop(fcontext_transfer_t ctx_transfer)
{
    current_nle_ctx->returncontext = ctx_transfer.ctx;
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
    /* ASan isn't happy with fcontext's assembly.
     * See: https://bugs.llvm.org/show_bug.cgi?id=27627 and
     * https://github.com/boostorg/coroutine/issues/30#issuecomment-325578344
     * TODO: I don't understand why __sanitizer_(start/finish)_switch_fiber
     * doesn't work here.
     */
    fcontext_stack_t *stack = &current_nle_ctx->stack;
    ASAN_UNPOISON_MEMORY_REGION((char *) stack->sptr - stack->ssize,
                                stack->ssize);
#endif

    nle_settings *s = &current_nle_ctx->settings;
    int len = strnlen(s->hackdir, sizeof(s->hackdir));

    if (len >= sizeof(s->hackdir) - 1) {
        error("HACKDIR too long");
        return;
    }
    if (s->hackdir[len - 1] != '/') {
        s->hackdir[len] = '/';
        s->hackdir[len + 1] = '\0';
    } else {
        s->hackdir[len] = '\0';
    }

    char *scoreprefix = (s->scoreprefix[0] != '\0')
                            ? s->scoreprefix
                            : s->hackdir;
    fqn_prefix[SYSCONFPREFIX] = s->hackdir;
    fqn_prefix[CONFIGPREFIX] = s->hackdir;
    fqn_prefix[HACKPREFIX] = s->hackdir;
    fqn_prefix[SAVEPREFIX] = s->hackdir;
    fqn_prefix[LEVELPREFIX] = s->hackdir;
    fqn_prefix[BONESPREFIX] = s->hackdir;
    fqn_prefix[SCOREPREFIX] = scoreprefix;
    fqn_prefix[LOCKPREFIX] = s->hackdir;
    fqn_prefix[TROUBLEPREFIX] = s->hackdir;
    fqn_prefix[DATAPREFIX] = s->hackdir;

    char *argv[1] = { "nethack" };

    unixmain(1, argv);
}

boolean
write_ttyrec_data(void *buf, int length)
{
    nle_ctx_t *nle = current_nle_ctx;
#ifdef NLE_BZ2_TTYRECS
    int bzerror;
    BZ2_bzWrite(&bzerror, nle->ttyrec_bz2, buf, length);
    assert(bzerror == BZ_OK);
#else
    assert(fwrite(buf, 1, length, nle->ttyrec) == length);
#endif
    return TRUE;
}

boolean
write_ttyrec_header(int length, unsigned char channel)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    int buffer[3];
    buffer[0] = tv.tv_sec;
    buffer[1] = tv.tv_usec;
    buffer[2] = length;

    /* Assumes little endianness */
    write_ttyrec_data(buffer, 3 * sizeof(int));
    write_ttyrec_data(&channel, 1);

    return TRUE;
}

/* win/tty only calls fflush(stdout). */
int
nle_fflush(FILE *stream)
{
    /* Only act on fflush(stdout). */
    if (stream != stdout) {
        fprintf(stderr,
                "Warning: nle_flush called with unexpected FILE pointer %p ",
                stream);
        return fflush(stream);
    }
    nle_ctx_t *nle = current_nle_ctx;

    ssize_t length = nle->outbuf_write_ptr - nle->outbuf;
    if (length == 0)
        return 0;

    if (nle->ttyrec) {
        write_ttyrec_header(length, 0);
        write_ttyrec_data(nle->outbuf, length);
    }

    nle_obs *obs = nle->observation;
    if (obs->tty_chars || obs->tty_colors || obs->tty_cursor) {
        tmt_write(nle->vterminal, nle->outbuf, length);
    }
    nle->outbuf_write_ptr = nle->outbuf;

#ifdef NLE_BZ2_TTYRECS
    return 0;
#else
    return nle->ttyrec ? fflush(nle->ttyrec) : 0;
#endif
}

/*
 * NetHack prints most of its output via putchar. We do our
 * own buffering.
 */
int
nle_putchar(int c)
{
    nle_ctx_t *nle = current_nle_ctx;
    if (nle->outbuf_write_ptr >= nle->outbuf_write_end) {
        nle_fflush(stdout);
    }
    *nle->outbuf_write_ptr++ = c;
    return c;
}

/*
 * Used in place of xputs from termcap.c. Not using
 * the tputs padding logic from tclib.c.
 */
void
nle_xputs(const char *str)
{
    int c;
    const char *p = str;

    if (!p || !*p)
        return;

    while ((c = *p++) != '\0') {
        nle_putchar(c);
    }
}

/*
 * puts seems to be called only by tty_raw_print and tty_raw_print_bold.
 * We could probably override this in winrl instead.
 */
int
nle_puts(const char *str)
{
    if (!*str) /* At exit, an empty string gets printed in tty_raw_print. */
        return 0;

    int val = fputs(str, stdout);
    putc('\n', stdout); /* puts includes a newline, fputs doesn't */
    return val;
}

/* Necessary for initial observation struct. */
nle_obs *
nle_get_obs()
{
    return current_nle_ctx->observation;
}

void *
nle_yield(void *notdone)
{
    nle_fflush(stdout);
    fcontext_transfer_t t =
        jump_fcontext(current_nle_ctx->returncontext, notdone);
#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
    fcontext_stack_t *stack = &current_nle_ctx->stack;
    ASAN_UNPOISON_MEMORY_REGION((char *) stack->sptr - stack->ssize,
                                stack->ssize);
#endif

    if (notdone)
        current_nle_ctx->returncontext = t.ctx;

    return t.data;
}

void
nethack_exit(int status)
{
    if (status) {
        fprintf(stderr, "NetHack exit with status %i\n", status);
    }
    nle_yield(NULL);
}

/* Called in really_done() in end.c to get "how". */
void
nle_done(int how)
{
    nle_ctx_t *nle = current_nle_ctx;
    nle->observation->how_done = how;
}

char *
nle_ttyrecname()
{
    return current_nle_ctx->settings.ttyrecname;
}

int
nle_spawn_monsters()
{
    return current_nle_ctx->settings.spawn_monsters;
}

/* `nle_seeds_init` moved into nle_ctx_t (refactor stage 2). Below uses
 * `current_nle_ctx->seeds_init`. */

/* See rng.c. */
extern int FDECL(whichrng, (int FDECL((*fn), (int) )));

/* See hacklib.c. */
extern int FDECL(set_random, (unsigned long, int FDECL((*fn), (int) )));
/* An appropriate version of this must always be provided in
   port-specific code somewhere. It returns a number suitable
   as seed for the random number generator */
extern unsigned long NDECL(sys_random_seed);

char *
nle_getenv(const char *name)
{
    if (strcmp(name, "TERM") == 0) {
        return "ansi";
    }
    if (strcmp(name, "NETHACKOPTIONS") == 0) {
        return current_nle_ctx->settings.options;
    }
    /* Don't return anything for "SHOPTYPE" or "SPLEVTYPE". */
    return (char *) 0;
}

FILE *
nle_fopen_wizkit_file()
{
    nle_settings *s = &current_nle_ctx->settings;
    size_t len = strnlen(s->wizkit, sizeof(s->wizkit));
    if (!len) {
        return (FILE *) 0;
    }
    return fmemopen(s->wizkit, len, "r");
}

/*
 * Initializes the random number generator.
 * Originally in hacklib.c.
 */
void
init_random(int FDECL((*fn), (int) ))
{
#ifdef NLE_ALLOW_SEEDING
    nle_seeds_init_t *si = current_nle_ctx->seeds_init;
    if (si) {
        set_random(si->seeds[whichrng(fn)], fn);
        current_nle_ctx->has_strong_rngseed = si->reseed;
        return;
    }
#endif
    set_random(sys_random_seed(), fn);
}

static void nle_swap_in(nle_ctx_t *nle);
static void nle_swap_out(nle_ctx_t *nle);

nle_ctx_t *
nle_start(nle_obs *obs, FILE *ttyrec, nle_seeds_init_t *seed_init,
          nle_settings *settings_p)
{
    /* Set CO and LI to control ttyrec output size. */
    CO = NLE_TERM_CO;
    LI = NLE_TERM_LI;

    nle_ctx_t *nle = init_nle(ttyrec, obs);
    nle->settings = *settings_p;
    nle->seeds_init = seed_init;

    nle->stack = create_fcontext_stack(STACK_SIZE);
    nle->generatorcontext =
        make_fcontext(nle->stack.sptr, nle->stack.ssize, mainloop);

    current_nle_ctx = nle;
    nle_swap_in(nle);
    fcontext_transfer_t t = jump_fcontext(nle->generatorcontext, NULL);
    nle->generatorcontext = t.ctx;
    nle->done = (t.data == NULL);
    obs->done = nle->done;
    nle->seeds_init =
        NULL; /* Don't set to *these* seeds on subsequent reseeds, if any. */
    nle_swap_out(nle);

    if (nle->ttyrec) {
        if (obs->blstats) {
            /* See comment in `nle_step`. We record the score in line with
             * the state to ensure s,r -> a -> s', r'. These lines ensure
             * we don't skip the first reward. */
            write_ttyrec_header(4, 2);
            write_ttyrec_data(&obs->blstats[9], 4);
        }
    }

    return nle;
}

/* Stage 6 dungeon topology save bundle. Holds copies of all the
 * dungeon-graph globals from decl.c so each env has its own. Allocated
 * lazily in nle_swap_in on first call (after init_dungeon has populated
 * the globals). */
struct nle_dungeon_save {
    /* stage 6 — dungeon graph */
    struct dgn_topology topology;
    dungeon             dungeons[MAXDUNGEON];
    s_level            *sp_levchn;
    stairway            upstair, dnstair, upladder, dnladder, sstairs;
    dest_area           updest, dndest;
    coord               inv_pos;
    /* stage 7 — current level (biggest single struct, ~40 KB) and
     * related per-level scratch */
    dlevel_t            level;
    struct linfo        level_info[MAXLINFO];
    schar               lastseentyp[COLNO][ROWNO];
    coord               doors[DOORMAX];
    struct mkroom       rooms[(MAXNROFROOMS + 1) * 2];
    struct mkroom      *subrooms;
    struct mkroom      *upstairs_room;
    struct mkroom      *dnstairs_room;
    struct mkroom      *sstairs_room;
    struct trap        *ftrap;
};

static void
nle_dungeon_save_to(struct nle_dungeon_save *s)
{
    s->topology = dungeon_topology;
    memcpy(s->dungeons, dungeons, sizeof(s->dungeons));
    s->sp_levchn = sp_levchn;
    s->upstair = upstair; s->dnstair = dnstair;
    s->upladder = upladder; s->dnladder = dnladder;
    s->sstairs = sstairs;
    s->updest = updest; s->dndest = dndest;
    s->inv_pos = inv_pos;
    /* stage 7 */
    s->level = level;
    memcpy(s->level_info, level_info, sizeof(s->level_info));
    memcpy(s->lastseentyp, lastseentyp, sizeof(s->lastseentyp));
    memcpy(s->doors, doors, sizeof(s->doors));
    memcpy(s->rooms, rooms, sizeof(s->rooms));
    s->subrooms = subrooms;
    s->upstairs_room = upstairs_room;
    s->dnstairs_room = dnstairs_room;
    s->sstairs_room = sstairs_room;
    s->ftrap = ftrap;
}

static void
nle_dungeon_load_from(const struct nle_dungeon_save *s)
{
    dungeon_topology = s->topology;
    memcpy(dungeons, s->dungeons, sizeof(s->dungeons));
    sp_levchn = s->sp_levchn;
    upstair = s->upstair; dnstair = s->dnstair;
    upladder = s->upladder; dnladder = s->dnladder;
    sstairs = s->sstairs;
    updest = s->updest; dndest = s->dndest;
    inv_pos = s->inv_pos;
    /* stage 7 */
    level = s->level;
    memcpy(level_info, s->level_info, sizeof(s->level_info));
    memcpy(lastseentyp, s->lastseentyp, sizeof(s->lastseentyp));
    memcpy(doors, s->doors, sizeof(s->doors));
    memcpy(rooms, s->rooms, sizeof(s->rooms));
    subrooms = s->subrooms;
    upstairs_room = s->upstairs_room;
    dnstairs_room = s->dnstairs_room;
    sstairs_room = s->sstairs_room;
    ftrap = s->ftrap;
}

/* Stage 5 context-switch: copy per-env flags/iflags/sysflags state in
 * from nle_ctx_t before resuming NetHack, then back out after.
 *
 * Why memcpy and not macros: `flags` (and `iflags`) are also used as
 * STRUCT FIELD NAMES in dungeon.h/lev.h/rm.h/sp_lev.h/func_tab.h.
 * A `#define flags (*ptr)` clobbers `someobj.flags` everywhere. Keeping
 * them as process-globals and swapping the contents around each step is
 * less elegant but keeps NetHack's source unchanged. Cost: 2 memcpys of
 * sizeof(struct flag)+sizeof(struct instance_flags) per c_step ≈
 * sub-microsecond on modern CPUs.
 *
 * Caveat: this serializes within-process multi-env stepping — all envs
 * share one global. For parallel scaling we still need either multi-
 * process OR thread-local storage on `flags` (a future refinement). */
static void
nle_swap_in(nle_ctx_t *nle)
{
    if (nle->flags_ptr)
        memcpy(&flags, nle->flags_ptr, sizeof(flags));
    if (nle->iflags_ptr)
        memcpy(&iflags, nle->iflags_ptr, sizeof(iflags));
#ifdef SYSFLAGS
    if (nle->sysflags_ptr)
        memcpy(&sysflags, nle->sysflags_ptr, sizeof(sysflags));
#endif
    if (nle->dungeon_save)
        nle_dungeon_load_from((struct nle_dungeon_save *) nle->dungeon_save);
}

static void
nle_swap_out(nle_ctx_t *nle)
{
    if (nle->flags_ptr)
        memcpy(nle->flags_ptr, &flags, sizeof(flags));
    if (nle->iflags_ptr)
        memcpy(nle->iflags_ptr, &iflags, sizeof(iflags));
#ifdef SYSFLAGS
    if (nle->sysflags_ptr)
        memcpy(nle->sysflags_ptr, &sysflags, sizeof(sysflags));
#endif
    if (!nle->dungeon_save)
        nle->dungeon_save = calloc(1, sizeof(struct nle_dungeon_save));
    if (nle->dungeon_save)
        nle_dungeon_save_to((struct nle_dungeon_save *) nle->dungeon_save);
}

nle_ctx_t *
nle_step(nle_ctx_t *nle, nle_obs *obs)
{
    current_nle_ctx = nle;
    nle_swap_in(nle);
    nle->observation = obs;
    if (nle->ttyrec) {
        write_ttyrec_header(1, 1);
        write_ttyrec_data(&obs->action, 1);
    }
    fcontext_transfer_t t = jump_fcontext(nle->generatorcontext, obs);
    nle->generatorcontext = t.ctx;
    nle->done = (t.data == NULL);
    obs->done = nle->done;
    nle_swap_out(nle);

    if (nle->ttyrec) {
        /* NLE ttyrec version 3 stores the action and in-game score in
         * different channels of the ttyrec. These channels are:
         *  - 0: the terminal instructions (classic ttyrec)
         *  - 1: the keypress/action (1 byte)
         *  - 2: the in-game score (4 bytes)
         *
         * We could either the note the in-game score every time we flush the
         * terminal instructions to screen, (eg writing [ 0 2 0 2 <step> 1 0 2
         * <step> 1 ]) or we can note it _just_ before resuming the game,
         * assuming no chicanery has happened to the score after it is written
         * to the array `blstats`, (eg writing [ 0 2 <step> 1 0 2 <step> 1 0 2
         * <step> ]). We chose the latter for compression & simplicity
         * reasons.
         *
         * Note: blstats[9] == botl_score which is used for score/reward fns.
         * see winrl.cc
         */
        if (obs->blstats) {
            write_ttyrec_header(4, 2);
            write_ttyrec_data(&obs->blstats[9], 4);
        }
    }

    return nle;
}

void
nle_end(nle_ctx_t *nle)
{
    if (!nle->done) {
        /* Reset without closing nethack. Need free memory, etc.
         * this is what nh_terminate in end.c does. I hope it's enough. */
        if (!current_nle_ctx->program_state.panicking) {
            freedynamicdata();
            dlb_cleanup();
        }
    }
    nle_fflush(stdout);

#ifdef NLE_BZ2_TTYRECS
    if (nle->ttyrec) {
        int bzerror;
        BZ2_bzWriteClose(&bzerror, nle->ttyrec_bz2, 0, NULL, NULL);
        assert(bzerror == BZ_OK);
    }
#endif

    tmt_close(nle->vterminal);

    destroy_fcontext_stack(&nle->stack);
    free(nle);
}

#ifdef NLE_ALLOW_SEEDING
void
nle_set_seed(nle_ctx_t *nle, unsigned long core, unsigned long disp,
             boolean reseed)
{
    /* Keep up to date with rnglist[] in rnd.c. */
    set_random(core, rn2);
    set_random(disp, rn2_on_display_rng);

    /* Determines logic in reseed_random() in hacklib.c. */
    current_nle_ctx->has_strong_rngseed = reseed;
};

/* nle_seeds[] moved into nle_ctx_t (refactor stage 2). Below uses
 * current_nle_ctx->seeds. */

void
nle_get_seed(nle_ctx_t *nle, unsigned long *core, unsigned long *disp,
             boolean *reseed)
{
    *core = current_nle_ctx->seeds[0];
    *disp = current_nle_ctx->seeds[1];
    *reseed = current_nle_ctx->has_strong_rngseed;
}
#endif

/* From unixtty.c */
/* fatal error */
/*VARARGS1*/
void error
VA_DECL(const char *, s)
{
    VA_START(s);
    VA_INIT(s, const char *);

    if (iflags.window_inited)
        exit_nhwindows((char *) 0); /* for tty, will call settty() */

    fprintf(stderr, s, VA_ARGS);
    fprintf(stderr, "\n");
    VA_END();
    nethack_exit(EXIT_FAILURE);
}

/* From unixtty.c */
char erase_char, intr_char, kill_char;

void
gettty()
{
    /* Should set erase_char, intr_char, kill_char */
}

void
settty(const char *s)
{
    end_screen();
    if (s)
        raw_print(s);
}

void
setftty()
{
    start_screen();

    iflags.cbreak = ON;
    iflags.echo = OFF;
}

void
intron()
{
}

void
introff()
{
}

#ifdef __linux__ /* via Jesse Thilo and Ben Gertzfield */
#include <sys/ioctl.h>
#include <sys/vt.h>

int linux_flag_console = 0;

void NDECL(linux_mapon);
void NDECL(linux_mapoff);
void NDECL(check_linux_console);
void NDECL(init_linux_cons);

void
linux_mapon()
{
#ifdef TTY_GRAPHICS
    if (WINDOWPORT("tty") && linux_flag_console) {
        write(1, "\033(B", 3);
    }
#endif
}

void
linux_mapoff()
{
#ifdef TTY_GRAPHICS
    if (WINDOWPORT("tty") && linux_flag_console) {
        write(1, "\033(U", 3);
    }
#endif
}

void
check_linux_console()
{
    struct vt_mode vtm;

    if (isatty(0) && ioctl(0, VT_GETMODE, &vtm) >= 0) {
        linux_flag_console = 1;
    }
}

void
init_linux_cons()
{
#ifdef TTY_GRAPHICS
    if (WINDOWPORT("tty") && linux_flag_console) {
        atexit(linux_mapon);
        linux_mapoff();
#ifdef TEXTCOLOR
        /*if (has_colors())*/ /* Assume true in NLE. */
        iflags.use_color = TRUE;
#endif
    }
#endif
}
#endif /* __linux__ */
