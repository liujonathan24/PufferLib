#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>
#include "profile.h"

#ifndef SYS_memfd_create
#  ifdef __x86_64__
#    define SYS_memfd_create 319
#  endif
#endif
static inline int nethack_memfd_create(const char* name, unsigned int flags) {
    return (int)syscall(SYS_memfd_create, name, flags);
}

// ---------------------------------------------------------------------------
// NLE minimal API
// ---------------------------------------------------------------------------
// We opaquely declare nle_ctx_t and the function pointer types so each
// Nethack instance can dlopen its own private copy of libnethack.so, giving
// each env its own copy of NetHack's many global variables. This is the same
// trick NLE Python uses (memfd_create + copy + dlopen). The standalone
// binary therefore does NOT link -lnethack — it locates libnethack.so at
// runtime via NETHACK_LIBPATH (default: ./vendor/nle/lib/libnethack.so).
#define NLE_ALLOW_SEEDING 1
#include "nleobs.h"

typedef struct nle_ctx nle_ctx_t;
typedef nle_ctx_t* (*nle_start_fn)(nle_obs*, FILE*, nle_seeds_init_t*, nle_settings*);
typedef nle_ctx_t* (*nle_step_fn)(nle_ctx_t*, nle_obs*);
typedef void       (*nle_end_fn)(nle_ctx_t*);

// ---------------------------------------------------------------------------
// Geometry constants
// ---------------------------------------------------------------------------
#define NH_ROWS 21
#define NH_COLS 79
#define NH_GRID (NH_ROWS * NH_COLS)

// ---------------------------------------------------------------------------
// Compile-time observation selection
// ---------------------------------------------------------------------------
// Override any of these with -DNETHACK_USE_<FIELD>=0/1. Defaults: chars only.
// Each enabled field reserves its own slice of the ByteTensor observation
// buffer; disabled fields are not allocated, not bound, and NLE does not
// write to them (NLE's fill_obs guards every field with `if (ptr) ...`).
//
// Field widths (bytes per element):
//   chars     uint8     1
//   colors    uint8     1
//   specials  uint8     1
//   glyphs    int16     2  (packed as little-endian)
//   blstats   int64     4  (truncated to int32 to save space)
//   message   uint8     1
//   inv_letters / inv_oclasses  uint8  1  (size NLE_INVENTORY_SIZE)
//
// `inv` enables both inv_letters and inv_oclasses together (cheap pair).
// inv_glyphs and inv_strs are deliberately omitted for v1 to keep the
// observation a flat ByteTensor.

#ifndef NETHACK_USE_CHARS
#define NETHACK_USE_CHARS    1
#endif
#ifndef NETHACK_USE_COLORS
#define NETHACK_USE_COLORS   0
#endif
#ifndef NETHACK_USE_SPECIALS
#define NETHACK_USE_SPECIALS 0
#endif
#ifndef NETHACK_USE_GLYPHS
#define NETHACK_USE_GLYPHS   0
#endif
#ifndef NETHACK_USE_BLSTATS
#define NETHACK_USE_BLSTATS  0
#endif
#ifndef NETHACK_USE_MESSAGE
#define NETHACK_USE_MESSAGE  0
#endif
#ifndef NETHACK_USE_INV
#define NETHACK_USE_INV      0
#endif

// Auto-dismiss prompts (welcome screen, --More--, yes/no, getline) before
// applying the agent's action, by inspecting misc[]={in_yn_function,
// in_getlin, xwaitingforspace} on every step. These tiny buffers are
// always allocated (~48 bytes) regardless of obs-field selection. Cap the
// dismiss loop at NETHACK_AUTODISMISS_MAX iterations to avoid hanging on
// pathological prompt chains.
#ifndef NETHACK_AUTODISMISS
#define NETHACK_AUTODISMISS 1
#endif
#ifndef NETHACK_AUTODISMISS_MAX
#define NETHACK_AUTODISMISS_MAX 64
#endif

// Penalty applied to the reward when the agent's action triggers a sub-prompt
// (illegal/ambiguous action — e.g. "Apply what?" / "What direction?") that
// we then have to ESC out of. Default -0.01 — small enough that legitimate
// y/n confirmations during real play don't tank rewards.
// Per-step reward shaping (compile-time tunable).
//   reward = (score - prev_score)
//          + NETHACK_DEPTH_BONUS  if went deeper this step
//          + NETHACK_SCOUT_BONUS  if entered a previously-unvisited tile
//          + NETHACK_ILLEGAL_PENALTY  if action triggered a sub-prompt
#ifndef NETHACK_ILLEGAL_PENALTY
#define NETHACK_ILLEGAL_PENALTY -0.5f      // -invalid_moves/2 per user spec
#endif
#ifndef NETHACK_SCOUT_BONUS
#define NETHACK_SCOUT_BONUS      0.1f       // for each new tile visited
#endif
#ifndef NETHACK_DEPTH_BONUS
#define NETHACK_DEPTH_BONUS      1.0f       // for each new dungeon level
#endif

#define NETHACK_SZ_CHARS    (NETHACK_USE_CHARS    * NH_GRID)
#define NETHACK_SZ_COLORS   (NETHACK_USE_COLORS   * NH_GRID)
#define NETHACK_SZ_SPECIALS (NETHACK_USE_SPECIALS * NH_GRID)
#define NETHACK_SZ_GLYPHS   (NETHACK_USE_GLYPHS   * NH_GRID * 2)
#define NETHACK_SZ_BLSTATS  (NETHACK_USE_BLSTATS  * NLE_BLSTATS_SIZE * 4)
#define NETHACK_SZ_MESSAGE  (NETHACK_USE_MESSAGE  * NLE_MESSAGE_SIZE)
#define NETHACK_SZ_INV      (NETHACK_USE_INV      * NLE_INVENTORY_SIZE * 2)

#define NETHACK_OFF_CHARS    0
#define NETHACK_OFF_COLORS   (NETHACK_OFF_CHARS    + NETHACK_SZ_CHARS)
#define NETHACK_OFF_SPECIALS (NETHACK_OFF_COLORS   + NETHACK_SZ_COLORS)
#define NETHACK_OFF_GLYPHS   (NETHACK_OFF_SPECIALS + NETHACK_SZ_SPECIALS)
#define NETHACK_OFF_BLSTATS  (NETHACK_OFF_GLYPHS   + NETHACK_SZ_GLYPHS)
#define NETHACK_OFF_MESSAGE  (NETHACK_OFF_BLSTATS  + NETHACK_SZ_BLSTATS)
#define NETHACK_OFF_INV      (NETHACK_OFF_MESSAGE  + NETHACK_SZ_MESSAGE)
#define NETHACK_OBS_SIZE     (NETHACK_OFF_INV      + NETHACK_SZ_INV)

#if NETHACK_OBS_SIZE == 0
#error "At least one NETHACK_USE_* field must be enabled."
#endif

#define NETHACK_NUM_ACTIONS 23

// Reduced action set: ASCII codes passed straight to nle as obs->action.
// 0-7 compass, 8-15 compass-long, 16 down, 17 up, 18 wait/.
// 19 search, 20 MORE (CR), 21 ESC, 22 pickup ','.
static const int NETHACK_ACTION_TABLE[NETHACK_NUM_ACTIONS] = {
    'k','j','h','l','y','u','b','n',
    'K','J','H','L','Y','U','B','N',
    '>','<','.','s','\r',27,',',
};

#define NETHACK_DEFAULT_OPTIONS \
    "name:Agent-mon-hum-neu-mal," \
    "autopickup,color,disclose:+i +a +v +g +c +o," \
    "mention_walls,nobones,nocmdassist,nolegacy,nosparkle," \
    "pickup_burden:unencumbered,pickup_types:$?!/," \
    "runmode:teleport,showexp,showscore,time"

typedef struct Log {
    float perf;
    float score;
    float episode_return;
    float episode_length;
    float depth;
    float valid_moves;        // c_steps where the agent's action advanced NetHack's turn counter
    float illegal_actions;    // c_steps where the agent's action hit a sub-prompt we had to ESC out of
    float new_tiles;          // unique tiles entered this episode (sums over episodes via Log.n)
    float n;
} Log;

typedef struct Nethack {
    Log log;
    unsigned char* observations;
    float* actions;
    float* rewards;
    float* terminals;
    int num_agents;

    // Per-instance dynamic load of libnethack.so.
    void* dl_handle;
    int   dl_fd;
    nle_start_fn fn_start;
    nle_step_fn  fn_step;
    nle_end_fn   fn_end;

    // NLE state
    nle_ctx_t* ctx;
    nle_obs obs;
    nle_settings settings;
    char vardir[4096];

    // Backing storage — only fields with USE_* enabled are actually allocated.
    // Conditionally compile these to save memory.
#if NETHACK_USE_GLYPHS
    short          glyphs[NH_GRID];
#endif
#if NETHACK_USE_CHARS
    unsigned char  chars[NH_GRID];
#endif
#if NETHACK_USE_COLORS
    unsigned char  colors[NH_GRID];
#endif
#if NETHACK_USE_SPECIALS
    unsigned char  specials[NH_GRID];
#endif
#if NETHACK_USE_BLSTATS
    long           blstats[NLE_BLSTATS_SIZE];
#endif
#if NETHACK_USE_MESSAGE
    unsigned char  message[NLE_MESSAGE_SIZE];
#endif
#if NETHACK_USE_INV
    unsigned char  inv_letters[NLE_INVENTORY_SIZE];
    unsigned char  inv_oclasses[NLE_INVENTORY_SIZE];
#endif

    // Always-allocated hook buffers (independent of obs selection).
    // misc/internal: prompt-state flags for auto-dismiss
    // blstats: lets us read NLE_BL_TIME to count valid moves
    // message: scanned for '?' to catch single-key prompts (direction, item)
    //          that NLE does not expose via the misc[] flags.
    int           hook_misc[NLE_MISC_SIZE];
    int           hook_internal[NLE_INTERNAL_SIZE];
    long          hook_blstats[NLE_BLSTATS_SIZE];
    unsigned char hook_message[NLE_MESSAGE_SIZE];

    long episode_start_time;
    long episode_valid_moves;
    long episode_illegal_actions;
    long episode_new_tiles;       // unique tiles entered this episode

    // Exploration bitmap for the current dungeon level. Cleared on
    // dungeon-level change. One bit per (row, col); 21*79 = 1659 bits,
    // round up to 208 bytes.
    unsigned char visited[(NH_GRID + 7) / 8];
    int  visited_level;            // dungeon level the bitmap corresponds to

    int tick;
    long prev_score;
    int prev_depth;
    float episode_return;
    int episode_length;
    unsigned int rng;   // required by vecenv.h (seeded with env index)
} Nethack;

// ---------------------------------------------------------------------------
// dlopen-per-instance: copy libnethack.so into a memfd, dlopen that fd
// ---------------------------------------------------------------------------
static int nethack_load_lib(Nethack* env) {
    PROF_INIT_IF_NEEDED();
    PROF_START(reload);
    const char* libpath = getenv("NETHACK_LIBPATH");
    if (libpath == NULL) libpath = "./vendor/nle/lib/libnethack.so";

    int src = open(libpath, O_RDONLY);
    if (src < 0) {
        fprintf(stderr, "nethack: cannot open libnethack at %s: %s\n",
                libpath, strerror(errno));
        return -1;
    }
    int dst = nethack_memfd_create("libnethack-copy", 0);
    if (dst < 0) {
        close(src);
        fprintf(stderr, "nethack: memfd_create failed: %s\n", strerror(errno));
        return -1;
    }
    char buf[65536];
    ssize_t n;
    while ((n = read(src, buf, sizeof(buf))) > 0) {
        char* p = buf;
        while (n > 0) {
            ssize_t w = write(dst, p, n);
            if (w < 0) {
                close(src); close(dst);
                fprintf(stderr, "nethack: write to memfd failed: %s\n", strerror(errno));
                return -1;
            }
            p += w; n -= w;
        }
    }
    close(src);

    char fdpath[64];
    snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%d", dst);
    // RTLD_LOCAL so each env's copy stays isolated.
    void* h = dlopen(fdpath, RTLD_NOW | RTLD_LOCAL);
    if (h == NULL) {
        close(dst);
        fprintf(stderr, "nethack: dlopen failed: %s\n", dlerror());
        return -1;
    }
    env->dl_fd = dst;
    env->dl_handle = h;
    env->fn_start = (nle_start_fn)dlsym(h, "nle_start");
    env->fn_step  = (nle_step_fn) dlsym(h, "nle_step");
    env->fn_end   = (nle_end_fn)  dlsym(h, "nle_end");
    if (!env->fn_start || !env->fn_step || !env->fn_end) {
        fprintf(stderr, "nethack: dlsym missing symbols: %s\n", dlerror());
        dlclose(h);
        close(dst);
        return -1;
    }
    PROF_END(reload, PROF_RESET_RELOAD);
    return 0;
}

static void nethack_unload_lib(Nethack* env) {
    if (env->dl_handle) { dlclose(env->dl_handle); env->dl_handle = NULL; }
    if (env->dl_fd > 0) { close(env->dl_fd);       env->dl_fd = 0; }
    env->fn_start = NULL; env->fn_step = NULL; env->fn_end = NULL;
}

// ---------------------------------------------------------------------------
// Per-instance vardir (NetHack expects nhdat + a few touched files)
// ---------------------------------------------------------------------------
static void nethack_touch(const char* path) {
    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) close(fd);
}

static int nethack_make_vardir(const char* source_hackdir, char* out_buf, size_t out_cap) {
    PROF_START(vardir);
    char tmpl[] = "/tmp/nle-XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (dir == NULL) return -1;
    if ((size_t)snprintf(out_buf, out_cap, "%s", dir) >= out_cap) return -1;

    char abs_source[4096];
    if (source_hackdir[0] == '/') {
        snprintf(abs_source, sizeof(abs_source), "%s", source_hackdir);
    } else {
        char cwd[2048];
        if (getcwd(cwd, sizeof(cwd)) == NULL) return -1;
        snprintf(abs_source, sizeof(abs_source), "%s/%s", cwd, source_hackdir);
    }

    char src[4096], dst[4096];
    snprintf(src, sizeof(src), "%s/nhdat", abs_source);
    snprintf(dst, sizeof(dst), "%s/nhdat", dir);
    if (symlink(src, dst) != 0) return -1;
    const char* touched[] = {"perm", "record", "logfile", "xlogfile"};
    for (size_t i = 0; i < 4; i++) {
        snprintf(dst, sizeof(dst), "%s/%s", dir, touched[i]);
        nethack_touch(dst);
    }
    snprintf(dst, sizeof(dst), "%s/save", dir);
    mkdir(dst, 0755);
    PROF_END(vardir, PROF_RESET_VARDIR);
    return 0;
}

static void nethack_rm_vardir(const char* dir) {
    if (dir == NULL || dir[0] == '\0') return;
    char p[4096];
    const char* files[] = {"nhdat", "perm", "record", "logfile", "xlogfile", "paniclog", "save"};
    for (size_t i = 0; i < sizeof(files)/sizeof(files[0]); i++) {
        snprintf(p, sizeof(p), "%s/%s", dir, files[i]);
        if (i == 6) rmdir(p); else unlink(p);
    }
    rmdir(dir);
}

// ---------------------------------------------------------------------------
// Bind obs pointers — only enabled fields, others stay NULL so NLE skips them
// ---------------------------------------------------------------------------
static void nethack_bind_obs(Nethack* env) {
    nle_obs* o = &env->obs;
    memset(o, 0, sizeof(*o));
#if NETHACK_USE_CHARS
    o->chars = env->chars;
#endif
#if NETHACK_USE_COLORS
    o->colors = env->colors;
#endif
#if NETHACK_USE_SPECIALS
    o->specials = env->specials;
#endif
#if NETHACK_USE_GLYPHS
    o->glyphs = env->glyphs;
#endif
#if NETHACK_USE_BLSTATS
    o->blstats = env->blstats;
#endif
#if NETHACK_USE_MESSAGE
    o->message = env->message;
#endif
#if NETHACK_USE_INV
    o->inv_letters  = env->inv_letters;
    o->inv_oclasses = env->inv_oclasses;
#endif
    // Always bind misc/internal so the hook can read prompt-state flags
    // independent of which obs fields the user selected. blstats/message are
    // only bound to hook_* when the user opted them OUT of the obs tensor —
    // otherwise they are already bound to env->{blstats,message} above.
    o->misc     = env->hook_misc;
    o->internal = env->hook_internal;
#if !NETHACK_USE_BLSTATS
    o->blstats  = env->hook_blstats;
#endif
#if !NETHACK_USE_MESSAGE
    o->message  = env->hook_message;
#endif
}

// Pointers we can always read for the hook, regardless of NETHACK_USE_*.
static inline const unsigned char* nethack_msg(const Nethack* env) {
#if NETHACK_USE_MESSAGE
    return env->message;
#else
    return env->hook_message;
#endif
}

// Read NetHack's current turn counter regardless of which obs fields are bound.
static inline long nethack_current_time(const Nethack* env) {
#if NETHACK_USE_BLSTATS
    return env->blstats[NLE_BL_TIME];
#else
    return env->hook_blstats[NLE_BL_TIME];
#endif
}

// Auto-dismiss: while NLE reports an active prompt (welcome/--More--/yn/getlin),
// inject the appropriate dismiss keystroke until the game is back at the
// main command prompt. counter_id distinguishes the calling site so the
// profiler can attribute fn_step calls correctly. Returns # fn_step calls made.
static int nethack_drain_prompts_cat(Nethack* env, ProfCounterId fn_step_counter) {
    int n = 0;
#if NETHACK_AUTODISMISS
    for (int i = 0; i < NETHACK_AUTODISMISS_MAX; i++) {
        int xwait = env->hook_misc[2];
        int yn    = env->hook_misc[0];
        int gl    = env->hook_misc[1];
        if (!xwait && !yn && !gl) break;
        if (xwait)      { env->obs.action = '\r'; PROF_COUNT(PROF_XWAIT_DRAIN, 1); }
        else if (yn)    { env->obs.action = 27;   PROF_COUNT(PROF_MISC_YN_ESC, 1); }
        else /* gl */   { env->obs.action = '\r'; PROF_COUNT(PROF_MISC_GETLIN_ESC, 1); }
        env->ctx = env->fn_step(env->ctx, &env->obs);
        n++;
        PROF_COUNT(PROF_FN_STEPS_TOTAL, 1);
        PROF_COUNT(fn_step_counter, 1);
        if (env->obs.done) break;
    }
#endif
    return n;
}

// Backwards-compatible alias for non-c_step callers that don't care about
// attribution. Counts toward "post_drain".
static int nethack_drain_prompts(Nethack* env) {
    return nethack_drain_prompts_cat(env, PROF_FN_STEPS_POST_DRAIN);
}

static void nethack_init_settings(Nethack* env) {
    memset(&env->settings, 0, sizeof(env->settings));
    const char* source = getenv("NETHACKDIR");
    if (source == NULL) source = "./vendor/nle/nethackdir";

    if (nethack_make_vardir(source, env->vardir, sizeof(env->vardir)) != 0) {
        fprintf(stderr, "nethack: failed to create vardir from source=%s\n", source);
        strncpy(env->settings.hackdir, source, sizeof(env->settings.hackdir) - 1);
    } else {
        strncpy(env->settings.hackdir, env->vardir, sizeof(env->settings.hackdir) - 1);
    }
    env->settings.scoreprefix[0] = '\0';
    env->settings.spawn_monsters = 1;
    env->settings.ttyrecname[0] = '\0';
    strncpy(env->settings.options, NETHACK_DEFAULT_OPTIONS, sizeof(env->settings.options) - 1);
    env->settings.wizkit[0] = '\0';
}

void init(Nethack* env) {
    env->ctx = NULL;
    env->tick = 0;
    env->prev_score = 0;
    env->prev_depth = 1;
    env->episode_return = 0.0f;
    env->episode_length = 0;
    env->vardir[0] = '\0';
    env->dl_handle = NULL; env->dl_fd = 0;
    // Don't load_lib here — c_reset will do it on first call. Avoids a
    // redundant ~180 ms dlopen+nle_start before the user even resets.
    nethack_init_settings(env);
    nethack_bind_obs(env);
}

void c_close(Nethack* env) {
    if (env->ctx != NULL && env->fn_end) {
        env->fn_end(env->ctx);
        env->ctx = NULL;
    }
    nethack_unload_lib(env);
    nethack_rm_vardir(env->vardir);
    env->vardir[0] = '\0';
}

// ---------------------------------------------------------------------------
// Observation packing: copy enabled fields into the flat ByteTensor buffer
// ---------------------------------------------------------------------------
static void nethack_pack_obs(Nethack* env) {
    unsigned char* o = env->observations;
#if NETHACK_USE_CHARS
    memcpy(o + NETHACK_OFF_CHARS, env->chars, NETHACK_SZ_CHARS);
#endif
#if NETHACK_USE_COLORS
    memcpy(o + NETHACK_OFF_COLORS, env->colors, NETHACK_SZ_COLORS);
#endif
#if NETHACK_USE_SPECIALS
    memcpy(o + NETHACK_OFF_SPECIALS, env->specials, NETHACK_SZ_SPECIALS);
#endif
#if NETHACK_USE_GLYPHS
    memcpy(o + NETHACK_OFF_GLYPHS, env->glyphs, NETHACK_SZ_GLYPHS);
#endif
#if NETHACK_USE_BLSTATS
    // Pack 27 longs as 27 int32s (truncate; NetHack stat values fit in i32).
    int32_t* dst = (int32_t*)(o + NETHACK_OFF_BLSTATS);
    for (int i = 0; i < NLE_BLSTATS_SIZE; i++) dst[i] = (int32_t)env->blstats[i];
#endif
#if NETHACK_USE_MESSAGE
    memcpy(o + NETHACK_OFF_MESSAGE, env->message, NETHACK_SZ_MESSAGE);
#endif
#if NETHACK_USE_INV
    memcpy(o + NETHACK_OFF_INV,                          env->inv_letters,  NLE_INVENTORY_SIZE);
    memcpy(o + NETHACK_OFF_INV + NLE_INVENTORY_SIZE,     env->inv_oclasses, NLE_INVENTORY_SIZE);
#endif
}

static void nethack_add_log(Nethack* env) {
    long score = 0, depth = env->prev_depth;
#if NETHACK_USE_BLSTATS
    score = env->blstats[NLE_BL_SCORE];
    depth = env->blstats[NLE_BL_DEPTH];
#else
    score = env->hook_blstats[NLE_BL_SCORE];
    depth = env->hook_blstats[NLE_BL_DEPTH];
#endif
    env->log.perf            += (float)score;
    env->log.score           += (float)score;
    env->log.depth           += (float)depth;
    env->log.valid_moves     += (float)env->episode_valid_moves;
    env->log.illegal_actions += (float)env->episode_illegal_actions;
    env->log.new_tiles       += (float)env->episode_new_tiles;
    env->log.episode_return  += env->episode_return;
    env->log.episode_length  += env->episode_length;
    env->log.n               += 1.0f;
}

void c_reset(Nethack* env) {
    PROF_INIT_IF_NEEDED();
    PROF_START(reset_total);
    PROF_COUNT(PROF_C_RESETS, 1);

    PROF_START(nle_end);
    if (env->ctx != NULL && env->fn_end) {
        env->fn_end(env->ctx);
        env->ctx = NULL;
    }
    if (env->dl_handle != NULL) {
        nethack_unload_lib(env);
    }
    PROF_END(nle_end, PROF_RESET_NLE_END);

    // nle_load_lib has its own PROF_RESET_RELOAD timer inside.
    if (nethack_load_lib(env) != 0) {
        fprintf(stderr, "nethack: failed to reload libnethack on reset\n");
        return;
    }
    nethack_bind_obs(env);
    env->obs.action = 0;
    env->obs.done = 0;
    env->obs.in_normal_game = 0;
    env->obs.how_done = 0;

    if (env->fn_start == NULL) {
        fprintf(stderr, "nethack: fn_start is NULL — load_lib failed\n");
        return;
    }
    PROF_START(nle_start);
    env->ctx = env->fn_start(&env->obs, NULL, NULL, &env->settings);
    PROF_END(nle_start, PROF_RESET_NLE_START);

    // Drain the welcome screen / initial prompts so the first c_step actually
    // applies the agent's action to a clean game state.
    PROF_START(reset_drain);
    nethack_drain_prompts_cat(env, PROF_FN_STEPS_RESET_DRAIN);
    PROF_END(reset_drain, PROF_RESET_DRAIN);

    env->tick = 0;
    env->prev_score = 0;
#if NETHACK_USE_BLSTATS
    env->prev_depth = (int)env->blstats[NLE_BL_DEPTH];
#else
    env->prev_depth = 1;
#endif
    env->episode_return = 0.0f;
    env->episode_length = 0;
    env->episode_start_time = nethack_current_time(env);
    env->episode_valid_moves = 0;
    env->episode_illegal_actions = 0;
    env->episode_new_tiles = 0;
    memset(env->visited, 0, sizeof(env->visited));
    env->visited_level = 0;
    env->rewards[0] = 0.0f;
    env->terminals[0] = 0.0f;
    PROF_START(reset_obs_pack);
    nethack_pack_obs(env);
    PROF_END(reset_obs_pack, PROF_OBS_PACK);
    PROF_END(reset_total, PROF_C_RESET_TOTAL);
}

void c_step(Nethack* env) {
    PROF_INIT_IF_NEEDED();
    PROF_START(c_step_total);
    PROF_COUNT(PROF_C_STEPS, 1);
    unsigned long fn_step_calls_before = g_prof.counters[PROF_FN_STEPS_TOTAL];
    int action_idx = (int)env->actions[0];
    if (action_idx < 0) action_idx = 0;
    if (action_idx >= NETHACK_NUM_ACTIONS) action_idx = NETHACK_NUM_ACTIONS - 1;
    env->obs.action = NETHACK_ACTION_TABLE[action_idx];

    long time_before = nethack_current_time(env);
    PROF_START(agent_fn_step);
    env->ctx = env->fn_step(env->ctx, &env->obs);
    PROF_END(agent_fn_step, PROF_AGENT_FN_STEP);
    PROF_COUNT(PROF_FN_STEPS_TOTAL, 1);
    PROF_COUNT(PROF_FN_STEPS_AGENT, 1);

    // Determine whether the agent's action landed in a sub-prompt:
    //   (a) misc[] flags (yn / getlin) — strong signal
    //   (b) message ends with '?' — catches single-key prompts NLE doesn't
    //       expose via misc, e.g. "In what direction do you want to throw?",
    //       "Apply what?", "What do you want to wield?". Legitimate game
    //       messages ("It's a wall.", "You feel...") don't end in '?'.
    int yn_or_getlin = (env->hook_misc[0] || env->hook_misc[1]);
    int xwait        = env->hook_misc[2];
    int msg_is_prompt = 0;
    const unsigned char* msg = nethack_msg(env);
    if (msg[0]) {
        int end = 0;
        while (end < NLE_MESSAGE_SIZE && msg[end]) end++;
        // Trim trailing spaces.
        while (end > 0 && msg[end-1] == ' ') end--;
        if (end > 0 && msg[end-1] == '?') msg_is_prompt = 1;
    }

    int illegal = yn_or_getlin || msg_is_prompt;
    PROF_START(post_drain);
    if (illegal) {
        // ESC the prompt out. Loop up to MAX iterations: some prompts chain
        // ("What direction?" -> after ESC -> "--More--").
        for (int i = 0; i < NETHACK_AUTODISMISS_MAX; i++) {
            int still_prompt = (env->hook_misc[0] || env->hook_misc[1] || env->hook_misc[2]);
            // Also check trailing '?' on the latest message.
            if (!still_prompt) {
                const unsigned char* m2 = nethack_msg(env);
                int e = 0; while (e < NLE_MESSAGE_SIZE && m2[e]) e++;
                while (e > 0 && m2[e-1] == ' ') e--;
                if (e == 0 || m2[e-1] != '?') break;
            }
            env->obs.action = 27;  // ESC
            env->ctx = env->fn_step(env->ctx, &env->obs);
            PROF_COUNT(PROF_FN_STEPS_TOTAL, 1);
            PROF_COUNT(PROF_FN_STEPS_POST_DRAIN, 1);
            PROF_COUNT(PROF_MSG_PROMPT_ESC, msg_is_prompt && !yn_or_getlin ? 1 : 0);
            if (env->obs.done) break;
        }
        env->episode_illegal_actions++;
        PROF_COUNT(PROF_ILLEGAL_ACTIONS, 1);
    } else if (xwait) {
        nethack_drain_prompts_cat(env, PROF_FN_STEPS_POST_DRAIN);
    }
    PROF_END(post_drain, PROF_POST_DRAIN);

    long time_after = nethack_current_time(env);
    if (time_after > time_before) {
        env->episode_valid_moves++;
        PROF_COUNT(PROF_VALID_MOVES, 1);
    } else {
        // No time advance: classify why.
        if (illegal) {
            PROF_COUNT(PROF_NOADV_PROMPT_DETECTED, 1);
        } else {
            // No prompt detected. Did NetHack at least show a message?
            const unsigned char* m = nethack_msg(env);
            if (m[0]) PROF_COUNT(PROF_NOADV_HAS_MSG, 1);
            else      PROF_COUNT(PROF_NOADV_SILENT, 1);
        }
    }

    env->tick++;
    env->episode_length++;

    long score = 0; int depth = env->prev_depth;
    long px = 0, py = 0;
#if NETHACK_USE_BLSTATS
    score = env->blstats[NLE_BL_SCORE];
    depth = (int)env->blstats[NLE_BL_DEPTH];
    px = env->blstats[NLE_BL_X];
    py = env->blstats[NLE_BL_Y];
#else
    score = env->hook_blstats[NLE_BL_SCORE];
    depth = (int)env->hook_blstats[NLE_BL_DEPTH];
    px = env->hook_blstats[NLE_BL_X];
    py = env->hook_blstats[NLE_BL_Y];
#endif
    float reward = (float)(score - env->prev_score);

    // Depth-changed bonus + reset exploration bitmap.
    if (depth != env->visited_level) {
        memset(env->visited, 0, sizeof(env->visited));
        env->visited_level = depth;
    }
    if (depth > env->prev_depth) reward += NETHACK_DEPTH_BONUS;

    // Scout bonus: reward each new (row,col) entered this level.
    // px is column (0..79), py is row (0..21). Clamp defensively.
    if (px >= 0 && px < NH_COLS && py >= 0 && py < NH_ROWS) {
        int bit_idx = (int)py * NH_COLS + (int)px;
        unsigned char* b = &env->visited[bit_idx >> 3];
        unsigned char mask = (unsigned char)(1 << (bit_idx & 7));
        if (!(*b & mask)) {
            *b |= mask;
            reward += NETHACK_SCOUT_BONUS;
            env->episode_new_tiles++;
        }
    }

    if (illegal) reward += NETHACK_ILLEGAL_PENALTY;
    env->prev_score = score;
    env->prev_depth = depth;

    env->rewards[0] = reward;
    env->episode_return += reward;

    if (env->obs.done) {
        env->terminals[0] = 1.0f;
        nethack_add_log(env);
        PROF_START(obs_pack_done);
        nethack_pack_obs(env);
        PROF_END(obs_pack_done, PROF_OBS_PACK);
        PROF_END(c_step_total, PROF_C_STEP_TOTAL);
        PROF_FN_STEPS_PER_C_STEP(g_prof.counters[PROF_FN_STEPS_TOTAL] - fn_step_calls_before);
        c_reset(env);
        return;
    }
    env->terminals[0] = 0.0f;
    PROF_START(obs_pack);
    nethack_pack_obs(env);
    PROF_END(obs_pack, PROF_OBS_PACK);
    PROF_END(c_step_total, PROF_C_STEP_TOTAL);
    PROF_FN_STEPS_PER_C_STEP(g_prof.counters[PROF_FN_STEPS_TOTAL] - fn_step_calls_before);
}

void c_render(Nethack* env) {
    printf("\x1b[H\x1b[2J");
#if NETHACK_USE_CHARS
    for (int r = 0; r < NH_ROWS; r++) {
        for (int c = 0; c < NH_COLS; c++) {
            unsigned char ch = env->chars[r * NH_COLS + c];
            putchar(ch ? ch : ' ');
        }
        putchar('\n');
    }
#else
    printf("(chars obs disabled)\n");
#endif
#if NETHACK_USE_BLSTATS
    printf("HP %ld/%ld  AC %ld  Dlvl %ld  Score %ld  T %ld\n",
           env->blstats[NLE_BL_HP], env->blstats[NLE_BL_HPMAX],
           env->blstats[NLE_BL_AC],
           env->blstats[NLE_BL_DEPTH],
           env->blstats[NLE_BL_SCORE],
           env->blstats[NLE_BL_TIME]);
#endif
#if NETHACK_USE_MESSAGE
    printf("Msg: %.*s\n", NLE_MESSAGE_SIZE, env->message);
#endif
    fflush(stdout);
}
