/* replay_view.c — render a golden trajectory's chars grid + blstats per step.
 *
 * Goldens are produced by ocean/nethack/verify_determinism.c with
 *   record / record-multi. Each step record holds (action, hash, reward,
 *   terminal); raw observations are NOT stored. To "view" a golden, we
 *   re-run the env with the seed in the header and feed it the recorded
 *   action stream, dumping the per-step chars grid (21x79) and a one-line
 *   blstats summary to stdout.
 *
 * Build (from PufferLib repo root):
 *   clang -O2 -Wall -std=gnu11 \
 *       -I./vendor/nle/include -I./ocean/nethack \
 *       ocean/nethack/replay_view.c \
 *       -o replay_view -ldl -lpthread -lm
 *
 * Usage:
 *   ./replay_view <golden.bin> [--from N] [--to M] [--step 1]
 *      --from / --to (inclusive lo, exclusive hi) bounds in step index;
 *      --step S      dump only every Sth step
 *      --no-grid     skip the chars grid; just blstats one-liners
 *
 * Output is ASCII-only so you can `less` it. To make a video:
 *   ./replay_view ocean/nethack/golden/golden_seed05_1k.bin > /tmp/trace.txt
 *   less -R /tmp/trace.txt
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dlfcn.h>
#include <time.h>
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

/* Mirror the v1/v2 record layout used by verify_determinism.c. We only
 * need the action field; the hash/reward/terminal are ignored here. */
#define GOLDEN_MAGIC   0x4E484447u  /* 'NHDG' */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint64_t seed;
    uint64_t action_seed;
    uint64_t n_steps;
    uint64_t obs_size;
    uint32_t num_actions;
    uint32_t use_blstats;
} GoldenHeader;

typedef struct __attribute__((packed)) {
    int32_t action;
    uint8_t hash[20];
    float   reward;
    uint8_t terminal;
    uint8_t _pad[3];
} GoldenRecord;

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
} Env;

static void bind_obs(Env* e) {
    memset(&e->obs, 0, sizeof(e->obs));
    e->obs.chars    = e->chars;
    e->obs.misc     = e->misc;
    e->obs.internal = e->internal;
    e->obs.blstats  = e->blstats;
    e->obs.message  = e->message;
}

static int make_vardir(const char* source, char* out_buf, size_t cap) {
    char tmpl[] = "/tmp/nle-view-XXXXXX";
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
    for (int i = 0; i < 4; i++) {
        snprintf(dst, sizeof(dst), "%s/%s", dir, touched[i]);
        int fd = open(dst, O_CREAT|O_WRONLY, 0644);
        if (fd >= 0) close(fd);
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

static void dump_grid(const Env* e) {
    for (int r = 0; r < NH_ROWS; r++) {
        for (int c = 0; c < NH_COLS; c++) {
            unsigned char ch = e->chars[r * NH_COLS + c];
            putchar(ch ? ch : ' ');
        }
        putchar('\n');
    }
}

static void dump_blstats(const Env* e, uint64_t step) {
    const long* b = e->blstats;
    printf("step=%-5" PRIu64 "  (x,y)=(%ld,%ld)  HP=%ld/%ld  Lv=%ld  T=%ld  Score=%ld  Hunger=%ld  Depth=%ld",
           step,
           b[NLE_BL_X], b[NLE_BL_Y],
           b[NLE_BL_HP], b[NLE_BL_HPMAX],
           b[NLE_BL_XP],
           b[NLE_BL_TIME],
           b[NLE_BL_SCORE],
           b[NLE_BL_HUNGER],
           b[NLE_BL_DEPTH]);
    /* Trailing message (up to first NUL). */
    int msg_n = 0;
    while (msg_n < NLE_MESSAGE_SIZE && e->message[msg_n]) msg_n++;
    if (msg_n > 0) {
        printf("  msg=\"");
        for (int i = 0; i < msg_n; i++) {
            unsigned char c = e->message[i];
            if (c >= 32 && c < 127) putchar(c);
            else printf("\\x%02x", c);
        }
        putchar('"');
    }
    putchar('\n');
}

static void usage(const char* prog) {
    fprintf(stderr, "Usage: %s <golden.bin> [--from N] [--to M] [--step S] [--no-grid]\n", prog);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc < 2) usage(argv[0]);
    const char* path = argv[1];
    long from = 0, to = -1, step_every = 1;
    int show_grid = 1;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--from") && i+1 < argc) from = atol(argv[++i]);
        else if (!strcmp(argv[i], "--to") && i+1 < argc) to = atol(argv[++i]);
        else if (!strcmp(argv[i], "--step") && i+1 < argc) step_every = atol(argv[++i]);
        else if (!strcmp(argv[i], "--no-grid")) show_grid = 0;
        else { fprintf(stderr, "Unknown arg: %s\n", argv[i]); usage(argv[0]); }
    }

    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    GoldenHeader h;
    if (fread(&h, sizeof(h), 1, f) != 1) { perror("fread header"); fclose(f); return 1; }
    if (h.magic != GOLDEN_MAGIC) {
        fprintf(stderr, "bad magic 0x%08x (expected 0x%08x)\n", h.magic, GOLDEN_MAGIC);
        fclose(f); return 1;
    }
    if (h.version != 1u && h.version != 2u) {
        fprintf(stderr, "unknown golden version %u\n", h.version);
        fclose(f); return 1;
    }
    fprintf(stderr, "golden: seed=%" PRIu64 " action_seed=%" PRIu64
            " n_steps=%" PRIu64 " v=%u num_actions=%u use_blstats=%u\n",
            h.seed, h.action_seed, h.n_steps, h.version,
            h.num_actions, h.use_blstats);

    const char* libpath = getenv("NETHACK_LIBPATH");
    if (!libpath) libpath = "./vendor/nle/src/build/libnethack.so";
    const char* nhdir = getenv("NETHACKDIR");
    if (!nhdir) nhdir = "./vendor/nle/nethackdir";

    void* lh = dlopen(libpath, RTLD_NOW | RTLD_LOCAL);
    if (!lh) { fprintf(stderr, "dlopen %s: %s\n", libpath, dlerror()); fclose(f); return 1; }
    nle_start_fn fn_start = (nle_start_fn) dlsym(lh, "nle_start");
    nle_step_fn  fn_step  = (nle_step_fn)  dlsym(lh, "nle_step");
    nle_end_fn   fn_end   = (nle_end_fn)   dlsym(lh, "nle_end");
    if (!fn_start || !fn_step || !fn_end) {
        fprintf(stderr, "dlsym: %s\n", dlerror());
        dlclose(lh); fclose(f); return 1;
    }

    Env env;
    memset(&env, 0, sizeof(env));
    if (make_vardir(nhdir, env.vardir, sizeof(env.vardir)) != 0) {
        fprintf(stderr, "vardir failed\n"); dlclose(lh); fclose(f); return 1;
    }
    strncpy(env.settings.hackdir, env.vardir, sizeof(env.settings.hackdir) - 1);
    env.settings.spawn_monsters = 1;
    strncpy(env.settings.options, DEFAULT_OPTIONS, sizeof(env.settings.options) - 1);
    bind_obs(&env);

    nle_seeds_init_t seeds;
    memset(&seeds, 0, sizeof(seeds));
    seeds.seeds[0] = h.seed;
    seeds.seeds[1] = h.seed ^ 0x9E3779B97F4A7C15ULL;  /* fallback if header has only core seed */
    seeds.reseed = 0;
    env.ctx = fn_start(&env.obs, NULL, &seeds, &env.settings);
    if (!env.ctx) { fprintf(stderr, "nle_start failed\n"); dlclose(lh); fclose(f); return 1; }
    drain_prompts(&env, fn_step);

    if (show_grid) {
        printf("=== initial ===\n");
        dump_grid(&env);
        dump_blstats(&env, 0);
    }

    for (uint64_t step = 0; step < h.n_steps; step++) {
        GoldenRecord rec;
        if (fread(&rec, sizeof(rec), 1, f) != 1) {
            fprintf(stderr, "truncated at step %" PRIu64 "\n", step);
            break;
        }
        if (h.version >= 2u) {
            drain_prompts(&env, fn_step);
            if (env.obs.done) {
                fprintf(stderr, "env terminated during pre-step drain at step %" PRIu64 "\n", step);
                break;
            }
        }
        env.obs.action = rec.action;
        env.ctx = fn_step(env.ctx, &env.obs);

        long s = (long)step;
        int in_range = (s >= from) && (to < 0 || s < to);
        int hit_stride = ((s - from) % step_every) == 0;
        if (in_range && hit_stride) {
            if (show_grid) {
                printf("=== step %" PRIu64 "  action=%d ('%c') ===\n",
                       step + 1, rec.action,
                       (rec.action >= 32 && rec.action < 127) ? rec.action : '?');
                dump_grid(&env);
            }
            dump_blstats(&env, step + 1);
        }
        if (env.obs.done) {
            fprintf(stderr, "env terminated at step %" PRIu64 "\n", step + 1);
            break;
        }
    }

    if (env.ctx) fn_end(env.ctx);
    dlclose(lh);
    fclose(f);
    return 0;
}
