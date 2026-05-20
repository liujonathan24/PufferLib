# exp_015 — libnethack pool to hide reset cost

## Problem

`c_reset` currently costs ~217 ms (180 ms dlopen + 36 ms nle_start +
small vardir setup). Each agent on average resets once every ~2000 c_steps
during mid-training, so resets account for ~22 % of per-agent wall time.
With episodes shorter than 2000 (early training / random play) the reset
share is much worse — up to 85 %.

## Goal

Make `c_reset` an **O(µs) pointer swap** by pre-loading K fresh libnethack
copies in a background thread before the main thread needs them.

## Design

```
process
├── main thread: runs c_step / c_reset, drives stepping
├── pool worker thread(s): produces fresh (dl_handle, ctx) entries
└── LibPool: shared queue of ready entries
```

### Pool entry

```c
typedef struct PoolEntry {
    void*        dl_handle;
    int          dl_fd;
    nle_start_fn fn_start;
    nle_step_fn  fn_step;
    nle_end_fn   fn_end;
    nle_ctx_t*   ctx;           // already started, ready to step
    char         vardir[4096];  // per-entry vardir
    nle_settings settings;
    // Pre-allocated obs backing buffers, already bound to ctx via nle_start
    int           hook_misc[NLE_MISC_SIZE];
    int           hook_internal[NLE_INTERNAL_SIZE];
    long          hook_blstats[NLE_BLSTATS_SIZE];
    unsigned char hook_message[NLE_MESSAGE_SIZE];
    short          glyphs[NH_GRID];
    unsigned char  chars[NH_GRID];
    /* ...etc, matching the per-USE_* fields... */
    nle_obs       obs;           // pointers all bound to this entry's buffers
} PoolEntry;
```

### Pool state

```c
typedef struct LibPool {
    PoolEntry* slots;            // ring of K slots
    int        capacity;
    int        head;             // next slot to consume
    int        tail;             // next slot to refill
    int        count;            // available entries
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    pthread_t       filler_thread;
    int             stop;        // shutdown flag
} LibPool;
```

### API

```c
// Spawned once at process init (or first env init).
LibPool* libpool_create(int capacity, const char* lib_path,
                        const char* nethackdir, const char* options);

// Get a ready entry. Blocks if the pool is empty (rare in steady state).
PoolEntry* libpool_acquire(LibPool* pool);

// Return a used entry. It will be torn down (nle_end + dlclose) and
// a new fresh one built by the filler thread.
void libpool_release(LibPool* pool, PoolEntry* entry);

void libpool_destroy(LibPool* pool);
```

### Filler thread

```c
static void* filler_loop(void* arg) {
    LibPool* p = (LibPool*)arg;
    while (!p->stop) {
        pthread_mutex_lock(&p->lock);
        while (!p->stop && p->count == p->capacity)
            pthread_cond_wait(&p->not_full, &p->lock);
        if (p->stop) { pthread_mutex_unlock(&p->lock); break; }
        PoolEntry* slot = &p->slots[p->tail];
        pthread_mutex_unlock(&p->lock);

        // Heavy work happens with the lock released — only consumes
        // the dynamic-linker mutex (which doesn't block other threads'
        // cached fn_step calls).
        if (slot->dl_handle) {
            // Tear down old entry first (recycled slot)
            if (slot->ctx) slot->fn_end(slot->ctx);
            dlclose(slot->dl_handle); close(slot->dl_fd);
            slot->ctx = NULL; slot->dl_handle = NULL;
        }
        load_lib_into(slot);             // memfd_create + copy + dlopen + dlsym
        make_vardir(slot->vardir);
        bind_obs_pointers(&slot->obs, slot);
        slot->ctx = slot->fn_start(&slot->obs, NULL, NULL, &slot->settings);
        drain_welcome(slot);              // pre-dismiss welcome screen too!

        pthread_mutex_lock(&p->lock);
        p->tail = (p->tail + 1) % p->capacity;
        p->count++;
        pthread_cond_signal(&p->not_empty);
        pthread_mutex_unlock(&p->lock);
    }
    return NULL;
}
```

### c_reset replacement

```c
void c_reset(Nethack* env) {
    // Return the current entry to the pool (filler will recycle it).
    if (env->pool_entry) libpool_release(g_pool, env->pool_entry);

    // Get the next ready entry (instant if pool is warm).
    env->pool_entry = libpool_acquire(g_pool);
    env->ctx       = env->pool_entry->ctx;
    env->fn_step   = env->pool_entry->fn_step;
    env->fn_end    = env->pool_entry->fn_end;
    // The obs pointers in env->pool_entry->obs are pre-bound to the
    // entry's own buffers. The agent reads through env->observations
    // (the pufferlib-owned flat tensor); we copy from entry->chars etc
    // in nethack_pack_obs() as usual.
    env->obs = env->pool_entry->obs;

    // ...episode bookkeeping reset (start_time, valid_moves, etc.) as before
    nethack_pack_obs(env);
}
```

## Why this works (and where it doesn't)

### Why it works for steady-state

- Steady-state reset rate: ~1 reset / (episode_length × steps_per_ms).
  At episode_length = 2000 and 5 µs/step = 100 ms per agent before death.
  256 agents on 16 cores → 16 agents/core → ~6 ms wallclock per agent
  → ~270 ms before any one agent re-needs a reset (sequential through
  the core).
- Pool refill rate: 1 entry / 217 ms (single filler thread).
- For 16 agents/core × 16 cores = 256 agents reset every ~270 ms,
  but a single filler thread produces 1 / 217 ms = 4.6 / sec, only
  enough for ~4.6 agents / sec. **Single-thread filler is NOT enough.**
  Need ~256/(0.27 sec) = ~950 entries/sec.

### Workaround: multiple filler threads

Each filler thread can produce ~4.6 / sec, but all of them serialize on
the dynamic-linker mutex inside dlopen. So adding threads doesn't help
the throughput — bottleneck is dlopen serial latency × count, not
total CPU.

**Implication: the pool helps the LATENCY of an individual reset
(217 ms → µs) but does NOT increase aggregate reset throughput.** If
all agents reset at the same rate, the per-process reset throughput
ceiling is unchanged.

### Where it does help

- **Burstiness smoothing**: when many agents finish episodes in the
  same evaluate window, the pool absorbs the burst (up to K entries).
  Without the pool the main thread blocks for the full 217 ms × N
  serial cost.
- **Early-training pain**: with episodes ~50 c_steps long, agents reset
  every 250 µs × 1000 = 0.25 s. 256 agents × 1/0.25 = 1024 resets/sec.
  Pool can't keep up — but it can at least dejag the latency profile.
- **Real win comes from multi-process**: each process has its own
  dynamic-linker mutex. So pool-per-process + N processes = N×
  scaling. This is consistent with the multi-process scaling result
  from exp_003 / 008.

### Where it might not help

- For long-episode trained policies (episode_length > 5000), reset
  share is already < 5 %, so the pool doesn't change much.
- For early random play, the pool absorbs latency but the aggregate
  throughput is still bound by dlopen serial cost.

## Decision

**Worth building.** Helps burstiness and early-training latency, even
if it doesn't lift the steady-state ceiling. Combined with multi-process
(which we already have via vecenv multi-rank or external worker pool),
the pool's per-process latency win compounds with the per-process
parallelism.

## Risk

Threading + dynamic loading is delicate. Plan:
1. Single-filler-thread, capacity K=4 prototype.
2. Add a `--pool` flag to standalone bench so we can A/B vs current.
3. Verify no UB via sanitizer build.
4. Production rollout only after exp_016 (an A/B benchmark).
