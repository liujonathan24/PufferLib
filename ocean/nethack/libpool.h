// Pre-warmed libnethack.so pool — hides the ~217 ms cost of dlopen+nle_start
// from the c_reset hot path.
//
// One background thread refills a ring of K pre-loaded copies of
// libnethack.so. c_reset() acquires the next ready entry in O(µs) instead
// of synchronously doing dlopen.
//
// Gated by -DNETHACK_LIBPOOL=1. When disabled (default 0), the pool struct
// is unused and c_reset goes through the legacy synchronous path.

#ifndef NETHACK_LIBPOOL_H
#define NETHACK_LIBPOOL_H

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <errno.h>

#include "nleobs.h"

#ifndef NETHACK_LIBPOOL
#define NETHACK_LIBPOOL 0
#endif
#ifndef NETHACK_LIBPOOL_SIZE
#define NETHACK_LIBPOOL_SIZE 4
#endif

typedef struct nle_ctx nle_ctx_t;
typedef nle_ctx_t* (*nle_start_fn)(nle_obs*, FILE*, nle_seeds_init_t*, nle_settings*);
typedef nle_ctx_t* (*nle_step_fn)(nle_ctx_t*, nle_obs*);
typedef void       (*nle_end_fn)(nle_ctx_t*);

// Forward declarations from nethack.h that we touch via opaque struct.
struct Nethack;

// Per-entry storage. The pool builds each entry from scratch (dlopen +
// vardir + nle_start) and parks it until somebody calls libpool_acquire.
// The acquirer copies fn_step/fn_end/ctx out and uses them as their own.
// On libpool_release the filler tears down nle_end + dlclose + rm vardir
// and rebuilds a fresh entry into this slot.
typedef struct PoolEntry {
    void*        dl_handle;
    int          dl_fd;
    nle_start_fn fn_start;
    nle_step_fn  fn_step;
    nle_end_fn   fn_end;
    nle_ctx_t*   ctx;
    char         vardir[4096];
    nle_settings settings;
    nle_obs      obs;
    // Backing storage for nle_obs pointer fields. The entry owns these.
    // We size for the *maximum* obs set — the user's NETHACK_USE_* flags
    // gate which ones nethack_pack_obs reads, but the pool always lets
    // NLE write everything (cheap; <10 KB / entry).
    short          glyphs[21 * 79];
    unsigned char  chars[21 * 79];
    unsigned char  colors[21 * 79];
    unsigned char  specials[21 * 79];
    long           blstats[NLE_BLSTATS_SIZE];
    unsigned char  message[NLE_MESSAGE_SIZE];
    int            program_state[NLE_PROGRAM_STATE_SIZE];
    int            internal[NLE_INTERNAL_SIZE];
    unsigned char  inv_letters[NLE_INVENTORY_SIZE];
    unsigned char  inv_oclasses[NLE_INVENTORY_SIZE];
    int            misc[NLE_MISC_SIZE];
} PoolEntry;

typedef struct LibPool {
    PoolEntry*     slots;       // [capacity]
    int            capacity;
    int            head;        // next slot to consume (read)
    int            tail;        // next slot to refill (write)
    int            count;       // available entries (0..capacity)
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    pthread_t       filler;
    int             stop;
    char            libpath[4096];
    char            source_hackdir[4096];
    char            options[32768];
} LibPool;

static int  libpool_load_into(PoolEntry* e, const char* libpath);
static int  libpool_make_vardir(const char* source_hackdir, char* out, size_t cap);
static void libpool_rm_vardir(const char* dir);
static void libpool_bind_obs(PoolEntry* e);
static int  libpool_drain_welcome(PoolEntry* e);

static int libpool_memfd_create(const char* name) {
    return (int)syscall(SYS_memfd_create, name, 0);
}

static void libpool_touch(const char* path) {
    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) close(fd);
}

static int libpool_make_vardir(const char* source_hackdir, char* out, size_t cap) {
    char tmpl[] = "/tmp/nle-pool-XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (dir == NULL) return -1;
    if ((size_t)snprintf(out, cap, "%s", dir) >= cap) return -1;

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
        libpool_touch(dst);
    }
    snprintf(dst, sizeof(dst), "%s/save", dir);
    mkdir(dst, 0755);
    return 0;
}

static void libpool_rm_vardir(const char* dir) {
    if (dir == NULL || dir[0] == '\0') return;
    char p[4096];
    const char* files[] = {"nhdat", "perm", "record", "logfile", "xlogfile", "paniclog", "save"};
    for (size_t i = 0; i < sizeof(files)/sizeof(files[0]); i++) {
        snprintf(p, sizeof(p), "%s/%s", dir, files[i]);
        if (i == 6) rmdir(p); else unlink(p);
    }
    rmdir(dir);
}

static int libpool_load_into(PoolEntry* e, const char* libpath) {
    int src = open(libpath, O_RDONLY);
    if (src < 0) {
        fprintf(stderr, "libpool: open %s: %s\n", libpath, strerror(errno));
        return -1;
    }
    int dst = libpool_memfd_create("libnethack-pool");
    if (dst < 0) {
        close(src);
        fprintf(stderr, "libpool: memfd_create: %s\n", strerror(errno));
        return -1;
    }
    char buf[65536];
    ssize_t n;
    while ((n = read(src, buf, sizeof(buf))) > 0) {
        char* p = buf;
        while (n > 0) {
            ssize_t w = write(dst, p, n);
            if (w < 0) { close(src); close(dst); return -1; }
            p += w; n -= w;
        }
    }
    close(src);
    char fdpath[64];
    snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%d", dst);
    void* h = dlopen(fdpath, RTLD_NOW | RTLD_LOCAL);
    if (h == NULL) {
        fprintf(stderr, "libpool: dlopen: %s\n", dlerror());
        close(dst);
        return -1;
    }
    e->dl_fd = dst;
    e->dl_handle = h;
    e->fn_start = (nle_start_fn)dlsym(h, "nle_start");
    e->fn_step  = (nle_step_fn) dlsym(h, "nle_step");
    e->fn_end   = (nle_end_fn)  dlsym(h, "nle_end");
    if (!e->fn_start || !e->fn_step || !e->fn_end) {
        fprintf(stderr, "libpool: dlsym: %s\n", dlerror());
        dlclose(h); close(dst);
        return -1;
    }
    return 0;
}

static void libpool_bind_obs(PoolEntry* e) {
    memset(&e->obs, 0, sizeof(e->obs));
    e->obs.glyphs        = e->glyphs;
    e->obs.chars         = e->chars;
    e->obs.colors        = e->colors;
    e->obs.specials      = e->specials;
    e->obs.blstats       = e->blstats;
    e->obs.message       = e->message;
    e->obs.program_state = e->program_state;
    e->obs.internal      = e->internal;
    e->obs.inv_letters   = e->inv_letters;
    e->obs.inv_oclasses  = e->inv_oclasses;
    e->obs.misc          = e->misc;
    // Deliberately leave inv_glyphs / inv_strs / screen_descriptions /
    // tty_chars / tty_colors / tty_cursor NULL — NLE skips them.
}

// Drain welcome screen prompts so the entry is ready at a real game prompt.
static int libpool_drain_welcome(PoolEntry* e) {
    for (int i = 0; i < 64; i++) {
        int xwait = e->misc[2], yn = e->misc[0], gl = e->misc[1];
        if (!xwait && !yn && !gl) return 0;
        if (xwait)      e->obs.action = '\r';
        else if (yn)    e->obs.action = 27;
        else            e->obs.action = '\r';
        e->ctx = e->fn_step(e->ctx, &e->obs);
        if (e->obs.done) return 1;
    }
    return 0;
}

// Build a single fresh entry from scratch. Reuses the storage in `e` but
// rebuilds dl_handle/ctx/vardir.
static int libpool_build_entry(LibPool* pool, PoolEntry* e) {
    // Tear down old, if any.
    if (e->ctx && e->fn_end) e->fn_end(e->ctx);
    if (e->dl_handle) { dlclose(e->dl_handle); e->dl_handle = NULL; }
    if (e->dl_fd > 0) { close(e->dl_fd); e->dl_fd = 0; }
    libpool_rm_vardir(e->vardir);
    e->vardir[0] = '\0';
    e->ctx = NULL;

    if (libpool_make_vardir(pool->source_hackdir, e->vardir, sizeof(e->vardir)) != 0) {
        fprintf(stderr, "libpool: vardir failed\n");
        return -1;
    }
    memset(&e->settings, 0, sizeof(e->settings));
    strncpy(e->settings.hackdir, e->vardir, sizeof(e->settings.hackdir) - 1);
    strncpy(e->settings.options, pool->options, sizeof(e->settings.options) - 1);
    e->settings.spawn_monsters = 1;

    if (libpool_load_into(e, pool->libpath) != 0) return -1;

    libpool_bind_obs(e);
    e->obs.action = 0;
    e->obs.done = 0;
    e->obs.in_normal_game = 0;
    e->obs.how_done = 0;

    e->ctx = e->fn_start(&e->obs, NULL, NULL, &e->settings);
    libpool_drain_welcome(e);
    return 0;
}

static void* libpool_filler_loop(void* arg) {
    LibPool* p = (LibPool*)arg;
    while (1) {
        pthread_mutex_lock(&p->lock);
        while (!p->stop && p->count == p->capacity)
            pthread_cond_wait(&p->not_full, &p->lock);
        if (p->stop) { pthread_mutex_unlock(&p->lock); break; }
        int slot = p->tail;
        pthread_mutex_unlock(&p->lock);

        // Build with lock dropped — only the dynamic-linker mutex is held
        // by dlopen, which doesn't block other threads' cached fn_step calls.
        libpool_build_entry(p, &p->slots[slot]);

        pthread_mutex_lock(&p->lock);
        p->tail = (p->tail + 1) % p->capacity;
        p->count++;
        pthread_cond_signal(&p->not_empty);
        pthread_mutex_unlock(&p->lock);
    }
    return NULL;
}

static LibPool* g_libpool = NULL;
static pthread_once_t g_libpool_once = PTHREAD_ONCE_INIT;

static LibPool* libpool_create(const char* libpath, const char* source_hackdir,
                               const char* options, int capacity) {
    LibPool* p = (LibPool*)calloc(1, sizeof(LibPool));
    p->capacity = capacity;
    p->slots = (PoolEntry*)calloc(capacity, sizeof(PoolEntry));
    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->not_empty, NULL);
    pthread_cond_init(&p->not_full, NULL);
    strncpy(p->libpath, libpath, sizeof(p->libpath) - 1);
    strncpy(p->source_hackdir, source_hackdir, sizeof(p->source_hackdir) - 1);
    strncpy(p->options, options, sizeof(p->options) - 1);
    p->head = 0; p->tail = 0; p->count = 0; p->stop = 0;
    pthread_create(&p->filler, NULL, libpool_filler_loop, p);
    return p;
}

static PoolEntry* libpool_acquire(LibPool* p) {
    pthread_mutex_lock(&p->lock);
    while (p->count == 0)
        pthread_cond_wait(&p->not_empty, &p->lock);
    int slot = p->head;
    p->head = (p->head + 1) % p->capacity;
    p->count--;
    pthread_cond_signal(&p->not_full);
    pthread_mutex_unlock(&p->lock);
    return &p->slots[slot];
}

static void libpool_release(LibPool* p, PoolEntry* e) {
    // Caller is done with this entry — the filler will rebuild it.
    // We don't need to do anything here other than signal that the
    // slot is free for refill. Since slots are addressed by index in
    // the ring, we just need to ensure the filler sees count < capacity.
    pthread_mutex_lock(&p->lock);
    // The entry was already consumed (head advanced past it) when we
    // acquired. We don't put it back in the ring; the filler picks the
    // next tail slot and overwrites whatever was there. So nothing to
    // do here — but we must NOT lose track of e's storage. The slot at
    // (e - p->slots) is the one we'd be rebuilding next. To keep the
    // ring simple, we accept this overlap: after release, the filler
    // tears down e's resources and refills the slot.
    pthread_cond_signal(&p->not_full);
    pthread_mutex_unlock(&p->lock);
    (void)e;  // suppress unused
}

static void libpool_destroy(LibPool* p) {
    if (!p) return;
    pthread_mutex_lock(&p->lock);
    p->stop = 1;
    pthread_cond_broadcast(&p->not_full);
    pthread_cond_broadcast(&p->not_empty);
    pthread_mutex_unlock(&p->lock);
    pthread_join(p->filler, NULL);
    for (int i = 0; i < p->capacity; i++) {
        PoolEntry* e = &p->slots[i];
        if (e->ctx && e->fn_end) e->fn_end(e->ctx);
        if (e->dl_handle) dlclose(e->dl_handle);
        if (e->dl_fd > 0) close(e->dl_fd);
        libpool_rm_vardir(e->vardir);
    }
    free(p->slots);
    pthread_mutex_destroy(&p->lock);
    pthread_cond_destroy(&p->not_empty);
    pthread_cond_destroy(&p->not_full);
    free(p);
}

#endif  // NETHACK_LIBPOOL_H
