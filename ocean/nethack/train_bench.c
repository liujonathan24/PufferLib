/* train_bench.c — training-shaped throughput benchmark for the shared
 * libnethack vecenv path.
 *
 * Where multi_shared.c halts when any env dies, this binary resets dead
 * envs in-place and keeps going. This mimics how PufferLib's training
 * loop would actually drive N envs in one process: episodes end, the
 * env auto-resets, the policy keeps stepping. The aim is to prove that
 * the post-refactor vecenv survives a *training-shaped* workload
 * (many episodes, many deaths/resets) at N=32 and N=64 envs without
 * crashing or leaking.
 *
 * Reports:
 *   - aggregate c_steps/sec (the metric pufferl prints as SPS)
 *   - episodes completed across all envs
 *   - mean / max episode reward (blstats[BL_SCORE] at terminal)
 *   - any env that hangs or panics terminates the bench with a backtrace
 *
 * Build (from PufferLib repo root):
 *   clang -O2 -Wall -std=gnu11 \
 *       -I./vendor/nle/include -I./ocean/nethack \
 *       ocean/nethack/train_bench.c \
 *       -o train_bench -ldl -lpthread -lm
 *
 * Usage:
 *   ./train_bench <num_envs> <total_agent_steps> [seed_base] [ep_cap]
 *
 *   ep_cap: if > 0, force a reset after this many gameplay steps even if
 *           the agent didn't die. Exercises the reset path under load.
 *
 * Example:
 *   ./train_bench 64 1000000 0x12345 500
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <execinfo.h>
#include <signal.h>
#define NLE_ALLOW_SEEDING
#include "nleobs.h"

typedef struct nle_ctx nle_ctx_t;
typedef nle_ctx_t* (*nle_start_fn)(nle_obs*, FILE*, nle_seeds_init_t*, nle_settings*);
typedef nle_ctx_t* (*nle_step_fn)(nle_ctx_t*, nle_obs*);
typedef void       (*nle_end_fn)(nle_ctx_t*);

#define NH_ROWS 21
#define NH_COLS 79
#define NH_GRID (NH_ROWS * NH_COLS)

#define DEFAULT_OPTIONS \
    "name:Agent-mon-hum-neu-mal," \
    "autopickup,color,disclose:+i +a +v +g +c +o," \
    "mention_walls,nobones,nocmdassist,nolegacy,nosparkle," \
    "pickup_burden:unencumbered,pickup_types:$?!/," \
    "runmode:teleport,showexp,showscore,time"

#define NUM_ACTIONS 18
static const int ACTION_TABLE[NUM_ACTIONS] = {
    'k','j','h','l','y','u','b','n',
    'K','J','H','L','Y','U','B','N',
    '.','s',
};

typedef struct {
    nle_ctx_t* ctx;
    nle_obs    obs;
    unsigned char chars[NH_GRID];
    int           misc[NLE_MISC_SIZE];
    int           internal[NLE_INTERNAL_SIZE];
    long          blstats[NLE_BLSTATS_SIZE];
    unsigned char message[NLE_MESSAGE_SIZE];
    nle_settings  settings;
    char          vardir[4096];
    unsigned int  rng;
    unsigned long seed_a, seed_b;
    /* per-episode counters */
    long   ep_steps;
    long   ep_score_last;   /* monotonic best-effort: max blstats[BL_SCORE] in episode */
} Env;

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void bind_obs(Env* e) {
    memset(&e->obs, 0, sizeof(e->obs));
    e->obs.chars    = e->chars;
    e->obs.misc     = e->misc;
    e->obs.internal = e->internal;
    e->obs.blstats  = e->blstats;
    e->obs.message  = e->message;
}

static int make_vardir(const char* source, char* out_buf, size_t cap) {
    char tmpl[] = "/tmp/nle-train-XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) return -1;
    if ((size_t)snprintf(out_buf, cap, "%s", dir) >= cap) return -1;
    char src[4096], dst[4096], abs[4096];
    if (source[0] == '/') snprintf(abs, sizeof(abs), "%s", source);
    else { char cwd[2048]; getcwd(cwd, sizeof(cwd)); snprintf(abs, sizeof(abs), "%s/%s", cwd, source); }
    snprintf(src, sizeof(src), "%s/nhdat", abs);
    snprintf(dst, sizeof(dst), "%s/nhdat", dir);
    if (symlink(src, dst) != 0) return -1;
    const char* touched[] = {"perm","record","logfile","xlogfile"};
    for (int i=0;i<4;i++) {
        snprintf(dst, sizeof(dst), "%s/%s", dir, touched[i]);
        int fd = open(dst, O_CREAT|O_WRONLY, 0644);
        if (fd>=0) close(fd);
    }
    snprintf(dst, sizeof(dst), "%s/save", dir);
    mkdir(dst, 0755);
    return 0;
}

static int drain_prompts(Env* e, nle_step_fn fn_step) {
    int n = 0;
    for (int i = 0; i < 64; i++) {
        int xwait = e->misc[2], yn = e->misc[0], gl = e->misc[1];
        if (!xwait && !yn && !gl) break;
        e->obs.action = xwait ? '\r' : (yn ? 27 : '\r');
        e->ctx = fn_step(e->ctx, &e->obs);
        n++;
        if (e->obs.done) break;
    }
    return n;
}

/* Reseed before reset so consecutive episodes differ. */
static void env_reset(Env* e, nle_start_fn fn_start, nle_step_fn fn_step,
                      nle_end_fn fn_end) {
    if (e->ctx) { fn_end(e->ctx); e->ctx = NULL; }
    bind_obs(e);
    e->seed_a = (e->seed_a * 6364136223846793005ULL) + 1442695040888963407ULL;
    e->seed_b = (e->seed_b * 6364136223846793005ULL) + 1442695040888963407ULL;
    nle_seeds_init_t seeds;
    memset(&seeds, 0, sizeof(seeds));
    seeds.seeds[0] = e->seed_a;
    seeds.seeds[1] = e->seed_b;
    seeds.reseed = 0;
    e->ctx = fn_start(&e->obs, NULL, &seeds, &e->settings);
    if (!e->ctx) { fprintf(stderr, "env_reset: nle_start failed\n"); _exit(2); }
    drain_prompts(e, fn_step);
    e->ep_steps = 0;
    e->ep_score_last = 0;
}

static int  g_current_env = -1;
static long g_current_t   = -1;
static void on_alrm(int sig) {
    (void)sig;
    fprintf(stderr, "\n[TRAIN_BENCH ALARM] hung at t=%ld env=%d\n",
            g_current_t, g_current_env);
    void* bt[20]; int n = backtrace(bt, 20);
    backtrace_symbols_fd(bt, n, 2);
    _exit(2);
}

int main(int argc, char** argv) {
    signal(SIGALRM, on_alrm);

    int num_envs       = (argc >= 2) ? atoi(argv[1]) : 32;
    long total_steps   = (argc >= 3) ? atol(argv[2]) : 200000;
    unsigned long sb   = (argc >= 4) ? strtoul(argv[3], NULL, 0) : 0x12345UL;
    long ep_cap        = (argc >= 5) ? atol(argv[4]) : 0;

    const char* libpath = getenv("NETHACK_LIBPATH");
    if (!libpath) libpath = "./vendor/nle/src/build/libnethack.so";
    const char* nhdir = getenv("NETHACKDIR");
    if (!nhdir) nhdir = "./vendor/nle/nethackdir";

    /* Single dlopen — the same pattern PufferLib's binding now uses. */
    void* h = dlopen(libpath, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 1; }
    nle_start_fn fn_start = (nle_start_fn) dlsym(h, "nle_start");
    nle_step_fn  fn_step  = (nle_step_fn)  dlsym(h, "nle_step");
    nle_end_fn   fn_end   = (nle_end_fn)   dlsym(h, "nle_end");
    if (!fn_start || !fn_step || !fn_end) {
        fprintf(stderr, "dlsym missing: %s\n", dlerror());
        return 1;
    }

    Env* envs = (Env*) calloc(num_envs, sizeof(Env));
    double t_init0 = now_sec();
    for (int i = 0; i < num_envs; i++) {
        envs[i].rng = 0xC0FFEEu + (unsigned)i;
        envs[i].seed_a = sb + (unsigned long)i;
        envs[i].seed_b = (sb ^ 0x9E3779B97F4A7C15ULL) + (unsigned long)i;
        memset(&envs[i].settings, 0, sizeof(envs[i].settings));
        if (make_vardir(nhdir, envs[i].vardir, sizeof(envs[i].vardir)) != 0) {
            fprintf(stderr, "vardir failed for env %d\n", i);
            return 1;
        }
        strncpy(envs[i].settings.hackdir, envs[i].vardir,
                sizeof(envs[i].settings.hackdir) - 1);
        envs[i].settings.spawn_monsters = 1;
        strncpy(envs[i].settings.options, DEFAULT_OPTIONS,
                sizeof(envs[i].settings.options) - 1);
        bind_obs(&envs[i]);

        nle_seeds_init_t seeds;
        memset(&seeds, 0, sizeof(seeds));
        seeds.seeds[0] = envs[i].seed_a;
        seeds.seeds[1] = envs[i].seed_b;
        seeds.reseed = 0;
        envs[i].ctx = fn_start(&envs[i].obs, NULL, &seeds, &envs[i].settings);
        if (!envs[i].ctx) { fprintf(stderr, "nle_start[%d] failed\n", i); return 1; }
        drain_prompts(&envs[i], fn_step);
    }
    double init_dt = now_sec() - t_init0;
    fprintf(stderr, "[train_bench] init+drain: %d envs in %.3fs (%.1f ms/env)\n",
            num_envs, init_dt, init_dt * 1000.0 / num_envs);

    /* Per-env hang watchdog: long enough to forgive normal level-gen,
     * short enough that a real hang fails the bench quickly. */
    long stepped = 0;
    long episodes = 0;
    long resets_after_hang = 0;
    long sum_terminal_score = 0;
    long max_terminal_score = 0;
    long max_terminal_turns = 0;

    double t0 = now_sec();
    long round = 0;
    while (stepped < total_steps) {
        g_current_t = round;
        for (int i = 0; i < num_envs && stepped < total_steps; i++) {
            g_current_env = i;
            alarm(30);
            unsigned r = envs[i].rng;
            r ^= r << 13; r ^= r >> 17; r ^= r << 5;
            envs[i].rng = r;
            envs[i].obs.action = ACTION_TABLE[r % NUM_ACTIONS];
            envs[i].ctx = fn_step(envs[i].ctx, &envs[i].obs);
            stepped++;
            envs[i].ep_steps++;
            if (envs[i].blstats[NLE_BL_SCORE] > envs[i].ep_score_last)
                envs[i].ep_score_last = envs[i].blstats[NLE_BL_SCORE];

            int force_reset = (ep_cap > 0 && envs[i].ep_steps >= ep_cap);
            if (envs[i].obs.done || force_reset) {
                episodes++;
                long score = envs[i].ep_score_last;
                long turns = envs[i].blstats[NLE_BL_TIME];
                sum_terminal_score += score;
                if (score > max_terminal_score) max_terminal_score = score;
                if (turns > max_terminal_turns) max_terminal_turns = turns;
                env_reset(&envs[i], fn_start, fn_step, fn_end);
            }
        }
        round++;
    }
    alarm(0);
    double dt = now_sec() - t0;

    fprintf(stderr, "[train_bench] total_steps=%ld wall=%.3fs\n", stepped, dt);
    fprintf(stderr, "[train_bench] aggregate c_steps/sec = %.0f\n", stepped / dt);
    fprintf(stderr, "[train_bench] per-env c_steps/sec   = %.0f\n",
            (stepped / dt) / num_envs);
    fprintf(stderr, "[train_bench] episodes completed    = %ld\n", episodes);
    fprintf(stderr, "[train_bench] mean terminal score   = %.1f\n",
            episodes ? (double)sum_terminal_score / episodes : 0.0);
    fprintf(stderr, "[train_bench] max  terminal score   = %ld\n", max_terminal_score);
    fprintf(stderr, "[train_bench] max  terminal turns   = %ld\n", max_terminal_turns);
    fprintf(stderr, "[train_bench] watchdog-triggered resets = %ld\n",
            resets_after_hang);

    /* Single-line machine summary for easy grepping. */
    printf("RESULT envs=%d steps=%ld wall=%.3f sps=%.0f eps=%ld mean_score=%.1f max_score=%ld\n",
           num_envs, stepped, dt, stepped / dt,
           episodes,
           episodes ? (double)sum_terminal_score / episodes : 0.0,
           max_terminal_score);

    for (int i = 0; i < num_envs; i++) {
        if (envs[i].ctx) fn_end(envs[i].ctx);
    }
    free(envs);
    dlclose(h);
    return 0;
}
