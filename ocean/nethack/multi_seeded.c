/*
 * multi_seeded — true vecenv determinism test for libnethack.
 *
 * dlopens libnethack.so ONCE, allocates N envs each with its own nle_ctx_t,
 * replays one of the 16 golden action streams per env, and checks that each
 * env's per-step observation hash matches its golden. This is the direct
 * vecenv parallel: single library instance, N envs in one process, one thread.
 *
 * If any env's stream diverges from its golden, that's per-env state
 * contamination from a global we haven't migrated to nle_ctx_t yet.
 *
 * Build (CC defaults to clang):
 *   $CC -O2 -Wall -std=gnu11 -I./vendor/nle/include -I./ocean/nethack \
 *       ocean/nethack/multi_seeded.c -o multi_seeded -ldl -lpthread -lm
 *
 * Usage:
 *   ./multi_seeded                            -- N=16, all 16 goldens
 *   ./multi_seeded 4                          -- first 4 goldens, N=4
 *   ./multi_seeded 16 ocean/nethack/golden    -- explicit golden dir
 *
 * Each env's stream is read from golden_seedXX_1k.bin (XX = 01..16).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>

#include "nleobs.h"

/* Same record/header layout as verify_determinism.c. */
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
#define GOLDEN_MAGIC 0x4E484447u  /* 'NHDG' (matches verify_determinism.c) */
#define GOLDEN_VERSION 2u

typedef struct __attribute__((packed)) {
    int32_t action;
    uint8_t hash[20];      /* SHA1 of obs */
    float   reward;
    uint8_t terminal;
    uint8_t _pad[3];
} GoldenRecord;

#define NUM_ACTIONS 23
static const char ACTION_KEYS[NUM_ACTIONS] = {
    /* Matches binding's NETHACK_NUM_ACTIONS table — adjust if it diverges. */
    'k','j','h','l','y','u','b','n',
    'K','J','H','L','Y','U','B','N',
    '.','s','>','<','i','q', 27
};

/* libnethack typedefs (opaque). */
typedef void nle_ctx_t;
typedef nle_ctx_t* (*nle_start_fn)(nle_obs*, FILE*, void*, void*);
typedef nle_ctx_t* (*nle_step_fn)(nle_ctx_t*, nle_obs*);
typedef void       (*nle_end_fn)(nle_ctx_t*);

typedef struct {
    char hackdir[4096];
    char options[4096];
    int  spawn_monsters;
    int  _pad;
} NleSettings;

#define OBS_GRID 1659  /* 21 * 79 */
typedef struct {
    nle_ctx_t* ctx;
    nle_obs    obs;
    NleSettings settings;
    char       vardir[256];
    /* Observation backing storage. */
    uint8_t    grid[OBS_GRID];
    uint8_t    blstats[64];
    /* Replay state. */
    GoldenRecord* records;
    uint64_t      n_records;
    uint64_t      pos;
    int           seed;
    int           mismatched;
    int           done_early;
} Env;

/* Hash one observation's grid+blstats; must match verify_determinism. */
#include <openssl/sha.h>  /* fallback if libnethack's bundled SHA1 isn't reachable */
/* We don't actually need a real SHA1 match — just BYTEWISE compare hashes
 * recorded in the golden. To avoid linking openssl, we implement the same
 * SHA1 used in verify_determinism.c. */

/* ---- mini SHA1 (public domain, Steve Reid) ---- */
typedef struct { uint32_t s[5], cnt[2]; uint8_t buf[64]; } sha1_ctx;
static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
static void sha1_transform(uint32_t s[5], const uint8_t b[64]) {
    uint32_t a,bb,c,d,e,w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)b[i*4]<<24) | ((uint32_t)b[i*4+1]<<16) | ((uint32_t)b[i*4+2]<<8) | (uint32_t)b[i*4+3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i-3]^w[i-8]^w[i-14]^w[i-16], 1);
    a = s[0]; bb = s[1]; c = s[2]; d = s[3]; e = s[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)       { f = (bb & c) | ((~bb) & d); k = 0x5A827999; }
        else if (i < 40)  { f = bb ^ c ^ d;             k = 0x6ED9EBA1; }
        else if (i < 60)  { f = (bb & c) | (bb & d) | (c & d); k = 0x8F1BBCDC; }
        else              { f = bb ^ c ^ d;             k = 0xCA62C1D6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(bb, 30); bb = a; a = t;
    }
    s[0] += a; s[1] += bb; s[2] += c; s[3] += d; s[4] += e;
}
static void sha1_init(sha1_ctx* x) {
    x->s[0]=0x67452301; x->s[1]=0xEFCDAB89; x->s[2]=0x98BADCFE; x->s[3]=0x10325476; x->s[4]=0xC3D2E1F0;
    x->cnt[0]=x->cnt[1]=0;
}
static void sha1_update(sha1_ctx* x, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t j = (x->cnt[0] >> 3) & 63;
    x->cnt[0] += (uint32_t)(len << 3);
    if (x->cnt[0] < (uint32_t)(len << 3)) x->cnt[1]++;
    x->cnt[1] += (uint32_t)(len >> 29);
    while (len-- > 0) {
        x->buf[j++] = *p++;
        if (j == 64) { sha1_transform(x->s, x->buf); j = 0; }
    }
}
static void sha1_final(sha1_ctx* x, uint8_t out[20]) {
    uint8_t fin[8];
    for (int i = 0; i < 8; i++) fin[i] = (uint8_t)(x->cnt[(i<4)?1:0] >> ((3 - (i&3))*8));
    uint8_t one = 0x80; sha1_update(x, &one, 1);
    while ((x->cnt[0] & 504) != 448) { uint8_t z = 0; sha1_update(x, &z, 1); }
    sha1_update(x, fin, 8);
    for (int i = 0; i < 20; i++) out[i] = (uint8_t)(x->s[i>>2] >> ((3 - (i&3))*8));
}
static void hash_obs(const Env* e, uint8_t out[20]) {
    sha1_ctx s; sha1_init(&s);
    sha1_update(&s, e->grid, OBS_GRID);
    sha1_update(&s, e->blstats, 64);
    sha1_final(&s, out);
}

static void bind_obs(Env* e) {
    memset(&e->obs, 0, sizeof(e->obs));
    e->obs.glyphs = NULL;
    e->obs.chars = e->grid;
    e->obs.colors = NULL;
    e->obs.specials = NULL;
    e->obs.blstats = (long*)e->blstats;
    e->obs.message = NULL;
    e->obs.program_state = NULL;
    e->obs.internal = NULL;
    e->obs.in_normal_game = 0;
}

static int make_vardir(const char* nhdir, char* out, size_t outsz) {
    snprintf(out, outsz, "/tmp/multi_seeded_%d_%p_XXXXXX", getpid(), (void*)out);
    if (!mkdtemp(out)) { perror("mkdtemp"); return -1; }
    /* Copy minimal data files. */
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "cp -r %s/. %s/", nhdir, out);
    if (system(cmd) != 0) { fprintf(stderr, "cp nethackdir failed\n"); return -1; }
    return 0;
}

#define DEFAULT_OPTIONS \
    "name:Agent-mon-hum-neu-mal," \
    "autopickup,color,disclose:+i +a +v +g +c +o," \
    "mention_walls,nobones,nocmdassist,nolegacy,nosparkle," \
    "pickup_burden:unencumbered,pickup_types:$?!/," \
    "runmode:teleport,showexp,showscore,time"

static GoldenRecord* load_golden(const char* path, GoldenHeader* hdr_out, uint64_t* n_out) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    GoldenHeader h;
    if (fread(&h, sizeof(h), 1, f) != 1) { fprintf(stderr, "%s: short header\n", path); fclose(f); return NULL; }
    if (h.magic != GOLDEN_MAGIC) { fprintf(stderr, "%s: bad magic\n", path); fclose(f); return NULL; }
    GoldenRecord* recs = (GoldenRecord*)malloc(h.n_steps * sizeof(GoldenRecord));
    if (!recs) { fprintf(stderr, "oom\n"); fclose(f); return NULL; }
    if (fread(recs, sizeof(GoldenRecord), h.n_steps, f) != h.n_steps) {
        fprintf(stderr, "%s: short records\n", path); fclose(f); free(recs); return NULL;
    }
    fclose(f);
    *hdr_out = h;
    *n_out = h.n_steps;
    return recs;
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char** argv) {
    int num_envs = (argc >= 2) ? atoi(argv[1]) : 16;
    const char* golden_dir = (argc >= 3) ? argv[2] : "ocean/nethack/golden";
    if (num_envs < 1 || num_envs > 16) {
        fprintf(stderr, "num_envs must be in 1..16 (we have 16 goldens)\n");
        return 2;
    }

    const char* libpath = getenv("NETHACK_LIBPATH");
    if (!libpath) libpath = "./vendor/nle/src/build/libnethack.so";
    const char* nhdir = getenv("NETHACKDIR");
    if (!nhdir) nhdir = "./vendor/nle/nethackdir";

    /* Single dlopen — the vecenv pattern. */
    void* h = dlopen(libpath, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    nle_start_fn fn_start = (nle_start_fn)dlsym(h, "nle_start");
    nle_step_fn  fn_step  = (nle_step_fn) dlsym(h, "nle_step");
    nle_end_fn   fn_end   = (nle_end_fn)  dlsym(h, "nle_end");
    if (!fn_start || !fn_step || !fn_end) { fprintf(stderr, "dlsym: %s\n", dlerror()); return 1; }

    Env* envs = (Env*)calloc(num_envs, sizeof(Env));

    /* Load goldens. */
    for (int i = 0; i < num_envs; i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/golden_seed%02d_1k.bin", golden_dir, i + 1);
        GoldenHeader h2;
        envs[i].records = load_golden(path, &h2, &envs[i].n_records);
        if (!envs[i].records) return 1;
        envs[i].seed = (int)h2.seed;
        printf("env %2d: golden=%s n_steps=%" PRIu64 " seed=%d\n",
               i, path, envs[i].n_records, envs[i].seed);
    }

    /* Init all envs (this is what currently crashes at N>=2). */
    double t_init0 = now_sec();
    for (int i = 0; i < num_envs; i++) {
        memset(&envs[i].settings, 0, sizeof(envs[i].settings));
        if (make_vardir(nhdir, envs[i].vardir, sizeof(envs[i].vardir)) != 0) return 1;
        strncpy(envs[i].settings.hackdir, envs[i].vardir, sizeof(envs[i].settings.hackdir) - 1);
        envs[i].settings.spawn_monsters = 1;
        strncpy(envs[i].settings.options, DEFAULT_OPTIONS, sizeof(envs[i].settings.options) - 1);
        bind_obs(&envs[i]);
        fprintf(stderr, "init env %d ...\n", i);
        envs[i].ctx = fn_start(&envs[i].obs, NULL, NULL, &envs[i].settings);
        if (!envs[i].ctx) { fprintf(stderr, "nle_start[%d] failed\n", i); return 1; }
    }
    fprintf(stderr, "init: %.3fs for %d envs\n", now_sec() - t_init0, num_envs);

    /* Replay round-robin. Each step picks one env, advances by one action,
     * checks hash. Stop on mismatch or end. */
    uint64_t total_steps = 0;
    int mismatches = 0;
    double t_step0 = now_sec();
    for (;;) {
        int alive = 0;
        for (int i = 0; i < num_envs; i++) {
            if (envs[i].pos >= envs[i].n_records || envs[i].done_early || envs[i].mismatched) continue;
            alive++;
            GoldenRecord* rec = &envs[i].records[envs[i].pos];

            /* Drain prompts (every '\n') if obs.done flag suggests so.
             * Real harness drains via predrain_prompts; we approximate by
             * stepping with the recorded action directly. The golden was
             * recorded with the same drain logic at record time. */
            envs[i].obs.action = ACTION_KEYS[rec->action % NUM_ACTIONS];
            envs[i].ctx = fn_step(envs[i].ctx, &envs[i].obs);
            uint8_t h2[20];
            hash_obs(&envs[i], h2);
            if (memcmp(h2, rec->hash, 20) != 0) {
                printf("env %2d MISMATCH at step %" PRIu64 " (action=%d=%c)\n",
                       i, envs[i].pos, rec->action, ACTION_KEYS[rec->action % NUM_ACTIONS]);
                envs[i].mismatched = 1;
                mismatches++;
                continue;
            }
            envs[i].pos++;
            total_steps++;
            if (envs[i].obs.done) { envs[i].done_early = 1; }
        }
        if (alive == 0) break;
    }
    double dt = now_sec() - t_step0;

    /* Summary. */
    int n_ok = 0, n_partial = 0;
    for (int i = 0; i < num_envs; i++) {
        if (envs[i].mismatched) {
            printf("env %2d: MISMATCH at step %" PRIu64 "/%" PRIu64 "\n",
                   i, envs[i].pos, envs[i].n_records);
        } else if (envs[i].pos == envs[i].n_records) {
            n_ok++;
        } else {
            n_partial++;
            printf("env %2d: terminated early at %" PRIu64 "/%" PRIu64 "\n",
                   i, envs[i].pos, envs[i].n_records);
        }
    }
    printf("\nmulti_seeded N=%d: %d OK, %d mismatch, %d partial\n",
           num_envs, n_ok, mismatches, n_partial);
    printf("total_steps=%" PRIu64 " wall=%.3fs aggregate=%.0f steps/s\n",
           total_steps, dt, total_steps / dt);

    /* Teardown. */
    for (int i = 0; i < num_envs; i++) {
        if (envs[i].ctx) fn_end(envs[i].ctx);
        free(envs[i].records);
    }
    free(envs);
    dlclose(h);
    return (mismatches != 0) ? 1 : 0;
}
