/* multi_shared.c — N envs in one process, all sharing ONE libnethack.so.
 *
 * Compare against multi.c which dlopens libnethack per-env (memfd hack).
 * After stages 4–9 of the refactor, all the per-env state lives in
 * nle_ctx_t (or is context-switched via nle_swap_in/out around each
 * nle_step). So N envs can correctly share the same libnethack mapping.
 *
 * Hypothesis: drops the icache pressure that crushed per-env SPS in
 * exp_023, where N=16 with dlopen-per-env gave 1.4 K/env (vs 44 K at N=1).
 *
 * Build (from PufferLib repo root):
 *   clang -O2 -Wall -std=gnu11 \
 *       -I./vendor/nle/include -I./ocean/nethack \
 *       -DNETHACK_USE_BLSTATS=1 \
 *       ocean/nethack/multi_shared.c \
 *       -o multi_shared -ldl -lpthread -lm
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
    char tmpl[] = "/tmp/nle-multi-XXXXXX";
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

int main(int argc, char** argv) {
    int num_envs    = (argc >= 2) ? atoi(argv[1]) : 4;
    long steps_per_env = (argc >= 3) ? atol(argv[2]) : 5000;

    const char* libpath = getenv("NETHACK_LIBPATH");
    if (!libpath) libpath = "./vendor/nle/src/build/libnethack.so";
    const char* nhdir = getenv("NETHACKDIR");
    if (!nhdir) nhdir = "./vendor/nle/nethackdir";

    /* Load libnethack ONCE — the key difference from multi.c. */
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
        memset(&envs[i].settings, 0, sizeof(envs[i].settings));
        if (make_vardir(nhdir, envs[i].vardir, sizeof(envs[i].vardir)) != 0) {
            fprintf(stderr, "vardir failed for env %d\n", i);
            return 1;
        }
        strncpy(envs[i].settings.hackdir, envs[i].vardir, sizeof(envs[i].settings.hackdir) - 1);
        envs[i].settings.spawn_monsters = 1;
        strncpy(envs[i].settings.options, DEFAULT_OPTIONS, sizeof(envs[i].settings.options) - 1);
        bind_obs(&envs[i]);

        envs[i].ctx = fn_start(&envs[i].obs, NULL, NULL, &envs[i].settings);
        if (!envs[i].ctx) { fprintf(stderr, "nle_start[%d] failed\n", i); return 1; }
        /* Drain welcome screen */
        drain_prompts(&envs[i], fn_step);
    }
    double init_dt = now_sec() - t_init0;
    printf("init+drain: %d envs in %.3fs (%.1f ms/env)\n",
           num_envs, init_dt, init_dt * 1000.0 / num_envs);

    /* Bench: round-robin step each env. Random actions per env. */
    long total_steps = (long) num_envs * steps_per_env;
    double t0 = now_sec();
    for (long t = 0; t < steps_per_env; t++) {
        for (int i = 0; i < num_envs; i++) {
            unsigned r = envs[i].rng;
            r ^= r << 13; r ^= r >> 17; r ^= r << 5; /* xorshift */
            envs[i].rng = r;
            envs[i].obs.action = ACTION_TABLE[r % NUM_ACTIONS];
            envs[i].ctx = fn_step(envs[i].ctx, &envs[i].obs);
            if (envs[i].obs.done) {
                /* End-of-game: skip; would normally reset. */
                envs[i].obs.done = 0;
            }
        }
    }
    double dt = now_sec() - t0;

    printf("steps_per_env=%ld total_steps=%ld wall=%.3fs\n",
           steps_per_env, total_steps, dt);
    printf("aggregate c_steps/sec  = %.0f\n", total_steps / dt);
    printf("per-env c_steps/sec    = %.0f\n", (total_steps / dt) / num_envs);

    /* Teardown */
    for (int i = 0; i < num_envs; i++) {
        if (envs[i].ctx) fn_end(envs[i].ctx);
    }
    free(envs);
    dlclose(h);
    return 0;
}
