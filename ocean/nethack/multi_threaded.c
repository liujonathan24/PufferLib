/* multi_threaded.c — N envs sharing one libnethack, stepped IN PARALLEL.
 *
 * Same struct/dlopen plumbing as multi_shared.c, but the inner step loop
 * uses #pragma omp parallel for. Used to measure whether the nle_state
 * refactor reaches the "linear scaling with threads" goal — i.e. whether
 * per-thread SPS stays ~constant as `threads` grows.
 *
 * Status (post Option B stages 6'/8'/9'AB/10' + TLS current_nle_ctx):
 *   single-thread: works, 1000-step golden replay matches.
 *   multi-thread: NOT yet safe — residual swap on stages 5, 7, part of 9
 *   (flags/iflags/sysflags, dlevel_t level, level_info, lastseentyp,
 *   rooms[]/doors[]/subrooms, ftrap, tc_gbl_data, killer, youmonst,
 *   urealtime, body-slot pointers, spl_book, m_shot, mvitals,
 *   quest_status) writes to process globals around each nle_step.
 *
 * Build (from PufferLib repo root):
 *   clang -O2 -Wall -fopenmp -std=gnu11 \
 *       -I./vendor/nle/include -I./ocean/nethack \
 *       -DNETHACK_USE_BLSTATS=1 \
 *       ocean/nethack/multi_threaded.c \
 *       -o multi_threaded -ldl -lpthread -lm
 *
 * Usage:
 *   ./multi_threaded <num_envs> <steps> [threads]
 *
 * Bench protocol: bind one OMP thread per env (#envs == #threads); each
 * thread loops `steps_per_env` calls to fn_step on its own env.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <omp.h>
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
    long          steps_taken;
    int           done;
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
    char tmpl[] = "/tmp/nle-mt-XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) return -1;
    if ((size_t)snprintf(out_buf, cap, "%s", dir) >= cap) return -1;
    char src[4096], dst[4096];
    char abs[4096];
    if (source[0] == '/') snprintf(abs, sizeof(abs), "%s", source);
    else { char cwd[2048]; getcwd(cwd, sizeof(cwd)); snprintf(abs, sizeof(abs), "%s/%s", cwd, source); }
    snprintf(src, sizeof(src), "%s/nhdat", abs);
    snprintf(dst, sizeof(dst), "%s/nhdat", dir);
    if (symlink(src, dst) != 0) return -1;
    const char* touched[] = {"perm","record","logfile","xlogfile"};
    for (int i=0;i<4;i++) {
        snprintf(dst, sizeof(dst), "%s/%s", dir, touched[i]);
        int fd = open(dst, O_CREAT|O_WRONLY, 0644);
        if (fd >= 0) close(fd);
    }
    return 0;
}

static int drain_prompts(Env* e, nle_step_fn fn_step) {
    int max_iters = 200;
    while (max_iters-- > 0 && e->obs.internal[3] != 0) {
        e->obs.action = '\r';
        e->ctx = fn_step(e->ctx, &e->obs);
        if (e->obs.done) return -1;
    }
    return 0;
}

int main(int argc, char** argv) {
    int num_envs       = (argc >= 2) ? atoi(argv[1]) : 4;
    long steps_per_env = (argc >= 3) ? atol(argv[2]) : 2000;
    int num_threads    = (argc >= 4) ? atoi(argv[3]) : num_envs;

    const char* libpath = getenv("NETHACK_LIBPATH");
    if (!libpath) libpath = "./vendor/nle/src/build/libnethack.so";
    const char* nhdir = getenv("NETHACK_DIR");
    if (!nhdir) nhdir = "vendor/nle/nethackdir";

    void* h = dlopen(libpath, RTLD_NOW|RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen %s: %s\n", libpath, dlerror()); return 1; }
    nle_start_fn fn_start = (nle_start_fn) dlsym(h, "nle_start");
    nle_step_fn  fn_step  = (nle_step_fn)  dlsym(h, "nle_step");
    nle_end_fn   fn_end   = (nle_end_fn)   dlsym(h, "nle_end");
    if (!fn_start || !fn_step || !fn_end) {
        fprintf(stderr, "missing symbols\n"); return 1;
    }

    Env* envs = (Env*) calloc(num_envs, sizeof(Env));

    double t_init0 = now_sec();
    for (int i = 0; i < num_envs; i++) {
        envs[i].rng = 0xC0FFEEu + (unsigned)i * 0x9E3779B1u;
        bind_obs(&envs[i]);
        if (make_vardir(nhdir, envs[i].vardir, sizeof(envs[i].vardir))) {
            fprintf(stderr, "vardir setup failed for env %d\n", i); return 1;
        }
        memset(&envs[i].settings, 0, sizeof(envs[i].settings));
        snprintf(envs[i].settings.hackdir, sizeof(envs[i].settings.hackdir), "%s", envs[i].vardir);
        snprintf(envs[i].settings.scoreprefix, sizeof(envs[i].settings.scoreprefix), "%s/", envs[i].vardir);
        snprintf(envs[i].settings.options, sizeof(envs[i].settings.options), "%s", DEFAULT_OPTIONS);
        envs[i].settings.spawn_monsters = 1;
        envs[i].ctx = fn_start(&envs[i].obs, NULL, NULL, &envs[i].settings);
        if (!envs[i].ctx) { fprintf(stderr, "env %d start failed\n", i); return 1; }
        drain_prompts(&envs[i], fn_step);
    }
    double init_dt = now_sec() - t_init0;
    printf("init+drain: %d envs in %.3fs (%.1f ms/env)\n",
           num_envs, init_dt, init_dt * 1000.0 / num_envs);

    /* Bench: each thread owns one env (or a chunk if envs > threads).
     * Inner loop is steps_per_env iterations of nle_step. */
    omp_set_num_threads(num_threads);
    printf("running OMP with %d threads on %d envs, %ld steps/env\n",
           num_threads, num_envs, steps_per_env);

    double t0 = now_sec();
    #pragma omp parallel for schedule(static)
    for (int i = 0; i < num_envs; i++) {
        Env* e = &envs[i];
        for (long t = 0; t < steps_per_env; t++) {
            if (e->done) break;
            unsigned r = e->rng;
            r ^= r << 13; r ^= r >> 17; r ^= r << 5;
            e->rng = r;
            e->obs.action = '.';   /* wait — stays alive longest */
            e->ctx = fn_step(e->ctx, &e->obs);
            e->steps_taken++;
            if (e->obs.done) e->done = 1;
        }
    }
    double dt = now_sec() - t0;

    long total_steps = 0;
    int alive = 0, dead = 0;
    for (int i = 0; i < num_envs; i++) {
        total_steps += envs[i].steps_taken;
        if (envs[i].done) dead++; else alive++;
    }
    printf("wall=%.3fs total_steps=%ld alive=%d dead=%d\n",
           dt, total_steps, alive, dead);
    printf("aggregate steps/sec    = %.0f\n", total_steps / dt);
    printf("per-thread steps/sec   = %.0f\n", (total_steps / dt) / num_threads);

    for (int i = 0; i < num_envs; i++) {
        if (envs[i].ctx) fn_end(envs[i].ctx);
    }
    free(envs);
    dlclose(h);
    return 0;
}
