# NetHack-on-PufferLib infrastructure

How NetHack 3.6 (a 30-year-old single-process terminal game) was hooked
into PufferLib's vectorized RL harness, and how its many process-global
mutable variables were progressively migrated into per-env state so 1024+
envs can step in parallel under one process without crashes.

This document is the engineering tour. The story across ~70 named
clusters (1 → BK) is in commit history; here we summarize the steady
state.

---

## 1. The binding surface

PufferLib's harness calls a small C interface per env: `init`, `c_reset`,
`c_step`, `c_close`, plus the observation/reward/terminal arrays defined
on a per-env `Env` struct. Every env in `ocean/*` implements that
interface in `binding.c` plus a header that defines `Env`.

For NetHack the entry points live in:

- `ocean/nethack/binding.c` — the pufferlib `MY_INIT` / `MY_LOG` callbacks.
- `ocean/nethack/nethack.h` — defines `struct Nethack`, the `c_step` /
  `c_reset` bodies, observation packing, reward shaping, and the wrapper
  around `nle_start` / `nle_step` / `nle_end`.

`struct Nethack` carries:

- A pointer to `nle_ctx_t* ctx` — NLE's per-env state object (see §3).
- Function pointers `fn_start` / `fn_step` / `fn_end`. In production
  these resolve to libnethack's `nle_start` / `nle_step` / `nle_end`
  directly (static link via `build/libstatic_nethack.o` for puffer
  training; runtime `dlopen` for standalone benches).
- The observation backing arrays the agent actually reads: `chars[]`
  (21×79), `colors[]`, `specials[]`, `glyphs[]`, `blstats[]`, `message[]`,
  `inv_letters[]`, `inv_oclasses[]`, etc. Each is bound into the
  `nle_obs` struct passed to `nle_step`.
- Per-episode bookkeeping: tick, prev_score, prev_depth, episode_return,
  episode_length, episode_valid_moves, episode_illegal_actions,
  episode_new_tiles, visited bitmap for the scout-bonus reward term.
- Reward-shaping coefficients (per `config/nethack.ini`).

### c_step contract

`c_step(env)` does, in order:

1. Map the agent's discrete action (0..NUM_ACTIONS-1) to a NetHack
   keystroke via `NETHACK_ACTION_TABLE[]`.
2. Call `fn_step(ctx, &obs)` — one NLE inner step. This jumps into
   the env's fcontext stack (§4), runs the game logic until the next
   yield, and returns.
3. Detect whether NLE yielded inside a sub-prompt (`misc[0]` = yn,
   `misc[1]` = getlin, `misc[2]` = xwait, or message ends with `?`).
   If so, auto-dismiss by injecting ESC / Enter until back at the
   top-level command prompt. This means one c_step may issue
   multiple internal `fn_step` calls.
4. Read the NetHack turn counter (`blstats[TIME]`). If it advanced,
   credit the agent with a `valid_move` (the agent's action was
   accepted); otherwise it was an illegal move that hit a wall, a
   no-op key, etc.
5. Compute the shaped reward:
   `score_coef * (score - prev_score) + descent_coef * (depth - prev_depth) + scout_coef * new_tile + illegal_penalty * illegal`.
6. Pack the observation: `memcpy` chars/colors/glyphs/specials into the
   contiguous observation buffer; truncate blstats from long to int32;
   copy message and inventory.
7. If `obs.done`, call `c_reset(env)` (which calls `nle_end` + `nle_start`
   for a fresh game).

### Observation memory layout

The per-env backing arrays in `struct Nethack` are scratch space — the
binding owns them. They get memcpy'd into the contiguous `observations`
array PufferLib provides per step (so the agent reads contiguous
[N, OBS_SIZE] rather than scattered `Env*` fields). Compile-time flags
(`NETHACK_USE_CHARS`, `NETHACK_USE_BLSTATS`, etc.) gate whether each
observation channel is included.

---

## 2. Linkage

`./build.sh nethack` produces `pufferlib/_C.cpython-*.so` containing:

- The puffer harness (`src/vecenv.h` etc.) instantiated for `Env = Nethack`.
- `ocean/nethack/binding.c` and `nethack.h` compiled in.
- A `libstatic_nethack.o` that bundles libnethack's `nle_start` /
  `nle_step` / `nle_end`. **No dlopen at runtime in production
  training** — the symbols are linked directly.

For standalone benches (`multi_threaded`, `train_bench`, `replay_view`,
`live_view`, `vec_smoke`) we `dlopen` `vendor/nle/src/build/libnethack.so`
to keep the harness lightweight. Set `NETHACK_LIBPATH` to override the
path.

---

## 3. The `nle_ctx_t` per-env context

This is the heart of the refactor. NetHack 3.6 was written assuming a
single global game state — hundreds of `static` and `extern` mutable
variables (`level`, `flags`, `u`, monster chains, RNG seeds, save/restore
buffers, dungeon graph, …) live at file/process scope and get touched
every step.

For vectorized parallel envs that's a disaster: env A's pet move could
overwrite env B's `gtyp`, env A's level-save buffer could collide with
env B's, env A's `iflags.status_updates` could turn off rendering env B
relies on, etc.

The fix: define `nle_ctx_t` (`vendor/nle/src/include/nle.h`) — one big
struct that holds *all* mutable game state. Every NLE entry point swaps
in the current env's `nle_ctx_t` before running. Macros at the top of
each `.c` file rewrite every bare reference to a former global into a
field access on `current_nle_ctx`.

Current size: **`sizeof(nle_ctx_t) = 74,552 bytes ≈ 72 KB per env**.

### The TLS anchor

`__thread nle_ctx_t *current_nle_ctx;` in `vendor/nle/src/src/nle.c`.

Set by `nle_step` (and `nle_start`, `nle_end`) at entry; everything
inside libnethack that wants to read or write per-env state goes through
this pointer.

For performance: declared with `__attribute__((tls_model("initial-exec")))`
so the access compiles to a single segment-relative load instead of a
`__tls_get_addr` function call (~3.3% CPU savings per perf-record).

### How a global gets migrated

The pattern across clusters AT through BK is mechanical:

1. Identify the global. E.g. `static struct musable m;` in `muse.c`.
2. Add `void *s_muse_m_p;` (or appropriate-typed field) to `nle_ctx_t`
   in `vendor/nle/src/include/nle.h`.
3. Allocate the backing storage in `init_nle()` (`vendor/nle/src/src/nle.c`)
   via `calloc(1, sizeof(struct musable))` or array-sized variants.
4. Add `#define m (*(struct musable *) current_nle_ctx->s_muse_m_p)` at
   the top of `muse.c`. Every bare `m.offensive` reference now reads
   the per-env field.
5. Delete the original `static` declaration (or `extern` declarations
   in other TUs that reference it).
6. Rebuild, verify no compile errors. The macro must not collide with
   struct field names (e.g. cluster BF renamed the global `known` →
   `scr_known` because `obj->known` was a struct member that would have
   gotten clobbered by the macro).

Cluster BJ (commit `0b095336`) is a clean reference: 3-file diff for the
muse.c / trapx / trapy migration.

### Cross-TU globals

Some "globals" weren't `static` — they were file-scope tentative
definitions linked across translation units. `boolean m_using` (zap.c
declared `extern boolean m_using;`) was one such; cluster BF migrated
it. For those: replace the definition AND every `extern` declaration
with a `#define` that points at the per-env field.

### Save/restore safety

Per-env state on disk needs to round-trip when the game saves a level
and reloads it (e.g. when the player walks down then back up the stairs).
Two foot-guns we hit:

- **`sizeof X` on a pointer macro yields 8** instead of the size of the
  underlying struct/array (cluster BH). After migration, code that wrote
  `bwrite(fd, ..., sizeof X)` silently truncated the on-disk record to
  the pointer size, while the matching reader on the receiving end
  still used the static array's size — misalignment, downstream
  `DEF_MREAD_SHORT` panic. Fix: be explicit:
  `bwrite(fd, lastseentyp, COLNO * ROWNO * sizeof(schar))`.
- **`_Static_assert(sizeof(struct eshk) == 4936, ...)`** asserts in
  `save.c` + `restore.c` (cluster A/B/C) catch any future struct-size
  drift between the writer and reader TUs.

### Per-env arena

NetHack does `alloc()` heavily for monsters, objects, etc. Originally
this routed through libc malloc which (a) gave a single shared heap
across envs, slow because of glibc's per-arena lock contention, and
(b) made it hard to free everything at env teardown.

Cluster BE replaced the shared arena with **per-env 64 MB
`MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE` arenas** stored in
`current_nle_ctx->s_arena_base/used/cap`. `alloc()` bumps the pointer;
`arena_free()` is a no-op for arena pointers but routes external
(`__libc_malloc`'d) pointers through `__libc_free`. `nle_end` munmap's
the whole arena — no per-pointer free traversal.

This is fast (no syscalls on the hot path), per-env isolated, and gets
all freed at once. **Caveat**: data that has to outlive a particular
env must NOT use arena alloc — cluster BI fixed exactly this bug for
the process-global `dlb_libs[]` directory which had ended up in the
first env's arena.

### Migration history snapshot

By cluster:

- **1–9**: foundational per-env state for the main game struct (u, flags,
  level, dungeon graph, RNG), pre-`nle_ctx_t`.
- **AN–BB**: bulk per-env work — TTY backend, window-port, dlb,
  artifacts, body-slot pointers, return-buffer scratch, persistent
  counters, level-build state, combat tick state, quest state.
- **BC**: save/restore dispatch tables (`saveprocs`, `restoreprocs`).
- **BD**: artifact discovery state.
- **BE**: per-env arena (described above).
- **BF**: 5 hot-path monster-turn globals (dogmove gtyp/gx/gy,
  mhitm vis/far_noise, muse m_using, mon vamp_rise_msg/disintegested,
  read known → scr_known).
- **BG**: 6 warm per-action globals (pickup filters, potion counters,
  invent xprn, display lastx/lasty/dela).
- **BH**: save/restore byte-count asymmetry fix for lastseentyp / doors
  (the long-standing N≥64 `DEF_MREAD_SHORT` crash).
- **BI**: dlb_libs arena escape (the post-BH segfault from
  `__strcmp_avx2` in init_dungeons).
- **BJ**: muse.c musable / trapx / trapy.
- **BK**: sp_lev lev_message/lregions/num_lregions (the `obs=0x4` UAF),
  decl.c nroom/nsubroom (eliminates last `__thread` swap), track.c
  utcnt/utpnt, sp_lev xstart/ystart/xsize/ysize, mkmaze bbubbles +
  bounds.

---

## 4. Coroutines (fcontext)

NetHack's `moveloop` was designed as a top-level event loop that calls
into the windowport for input. RL training inverts this — the agent
supplies an action per step from outside.

Bridge: each env owns a **Boost::context fcontext stack**.
`nle_start` allocates the stack, schedules `moveloop` on it, and yields
back to the caller. `nle_step` jumps into the stack (passing the agent's
action) which resumes execution inside NetHack; NetHack runs until it
needs more input, then yields back via `jump_fcontext`. The stack stays
alive across steps so NetHack's call frames (including its inner
loops) preserve state naturally.

The stack is 64 KiB per env, allocated via `mmap` and pointed to by
`nle_ctx_t->stack`. The stack itself doesn't contain any other env's
data — perfectly isolated. The only shared concern is the
`current_nle_ctx` TLS pointer, which must be set to *this* env before
the jump. Done at line ~860 of `vendor/nle/src/src/nle.c`.

---

## 5. The puffer step loop (what we don't change)

PufferLib's per-buffer threadmanager (`src/vecenv.h:240`) does:

```
for t in 0..horizon:
    net_callback(...)        # Python policy: actions out
    cudaMemcpyAsync(...)     # actions D2H
    #pragma omp parallel for
    for i in 0..num_envs:
        c_step(&envs[i])     # one step per env
    cudaMemcpyAsync(...)     # obs+reward+terminal H2D
```

Each horizon iteration does *one* c_step per env, OMP-fanned across
`num_workers` (defaults to all 128 cores). At N=1024 that's 8 envs per
worker per outer iter — round-robin pattern.

**Implication**: cache thrash. Each thread touches 8 different 72 KB
`nle_ctx_t` structs per outer iter (total per-worker working set > L2
1 MB). Empirically this round-robin pattern is ~5× slower than
"one env per thread, many steps in a row" — see the
`/tmp/multi_threaded_rr` controlled bench in `exp_039_goal_8h`.

We don't change the harness per project constraint. We mitigate inside
the env code:

- `__builtin_prefetch` 4 cache lines of `nle_ctx_t` at `nle_step` entry
  (commit `e1989eda`). Lets the L1 prefetcher start the cold fills
  before the actual reads.
- Strip dead work (status_updates rendering, recalc_mapseen, tty
  emitters when no tty_chars obs is bound) — multiple commits, see
  REPORT.

The remaining 5× gap to multi_threaded is structural and would require
either splitting `nle_ctx_t` into hot/cold portions (so the prefetched
first cache line covers everything c_step needs) or batched stepping
(many steps per env before yielding back to OMP barrier). Both are
captured as future tasks.

---

## 6. Tooling

| Tool | Purpose |
|---|---|
| `multi_threaded` | Pure-C OMP env-loop bench (one thread, all steps of env, then next env). Measures the env's intrinsic ceiling without harness overhead. |
| `multi_threaded_rr` (in `/tmp/`) | Round-robin variant — same OMP, but one step per env per outer iter. Mirrors puffer's pattern and shows the cache-thrash cost. |
| `train_bench` | Serial training-shaped bench: one env, sequential, with reset-on-done. Best per-env SPS reference. |
| `replay_view` | Render a recorded golden trajectory's chars grid + blstats per step. |
| `live_view` | Interactive single-env debug viewer with ANSI colors. Random / replay / keystroke modes. |
| `vec_smoke` | Standalone repro for the N≥2 vec init crash (now fixed; kept for regression). |
| `verify_determinism` | Records golden trajectories and re-runs to ensure byte-for-byte reproducibility. |

---

## 7. How to add a new per-env field

The recipe in 5 minutes:

1. Decide the type and size. Hot tip: scalar `int`/`boolean`/`long` go
   inline in `nle_ctx_t`; larger structs go behind a pointer so the
   struct fits more in L2.
2. Add the field at the end of `nle_ctx_t` in
   `vendor/nle/src/include/nle.h`. Comment with the cluster name and
   what it replaces.
3. If pointer-typed: allocate in `init_nle()` (nle.c) via `calloc(...)`,
   `assert(ptr)`. Free in `nle_end()` via `free(...)` if libc-malloc,
   or rely on the arena `munmap` if arena-allocated.
4. At the top of each `.c` file that used the old global:
   `#define foo (current_nle_ctx->s_foo)` (scalar) or
   `#define foo (*(struct foo_t *) current_nle_ctx->s_foo_p)` (pointer-to-struct).
   Or `#define foo (current_nle_ctx->s_foo_p)` if `foo` was a pointer
   originally.
5. Remove the original `static` declaration. For cross-TU items,
   replace `extern foo;` declarations elsewhere with the same `#define`.
6. Watch for **macro collisions with struct fields** (cluster BF lesson:
   `known` collided with `obj->known`). If a member of any struct has
   the same name as the global being migrated, rename the global to
   something unique (e.g. `scr_known`).
7. Watch for **`sizeof X` on a now-pointer X** (cluster BH lesson).
   Audit every `bwrite(..., sizeof X)` / `mread(..., sizeof X)` /
   `memcpy(..., ..., sizeof X)` after migration. Use explicit array
   byte counts when X is a pointer-aliased array.
8. Run `make -C vendor/nle/src/build nethack -j16`, `./build.sh nethack`,
   and `./multi_threaded 64 5000 64`. If multi_threaded crashes or
   throughput regresses, your migration is buggy. Use
   `bash ocean/nethack/experiments/exp_039_goal_8h/run_sps.sh 64 30 sanity`
   for the end-to-end check.

---

## 8. Known performance ceiling

At iter-9 of exp_039 the puffer training SPS curve looks like:

| N | agg SPS | per-env SPS |
|---|---|---|
| 8 | 65K | **8.2K** (peak per-env, cache fits) |
| 128 | 46K | 358 |
| 1024 | 62K | 60 |
| 4096 | 112K | 27 |

Aggregate keeps climbing with N because more cores are productively
used; per-env collapses because of cache thrash and memory-bandwidth
contention at 128 active threads. Both effects documented in §5.

Pure-C OMP env-loop pattern (`multi_threaded` random actions, no
harness) at N=1024 gives **1.5–2.0M SPS** — the libnethack ceiling once
the harness cache pattern is removed. The two future directions in §5
(hot/cold split, batched stepping) target closing that gap.

---

## 9. Open invariants the codebase relies on

These hold today; future changes must preserve them.

- **No process-global mutable state on the hot path.** All
  monster/object/level state is reached through `current_nle_ctx`.
  Verified by `find_globals.py` script + per-cluster audits.
- **No mutex / spinlock / atomic-fetch-add on the hot stepping path.**
  Cluster BD removed the last per-step atomic (`__sync_fetch_and_add`
  on the arena). The remaining atomics are init-once gates
  (`dlb_init`, `choose_windows`) using CAS for first-init-wins.
- **No dlopen in production training.** Static link via
  `libstatic_nethack.o`.
- **TLS access for `current_nle_ctx` is single segment-relative load**
  (initial-exec). Don't change to a regular `__thread` without
  re-benchmarking.
- **`sizeof(struct eshk) == 4936`, `sizeof(struct monst) == 144`,
  `sizeof(struct obj) == 96`** — `_Static_assert` enforced in both
  save.c and restore.c so any future struct change that breaks
  cross-TU agreement fails at compile time.
- **`tty_status_update`, `tty_clear_nhwindow`, `nle_putchar`** are
  no-ops when the corresponding TTY observation channels aren't
  bound (`tty_chars` / `tty_colors` / `tty_cursor`). Re-enable by
  binding those obs channels — see `nethack_pack_obs` in
  `ocean/nethack/nethack.h`.

## 10. Behavior change: `!status_updates`

The `NETHACK_DEFAULT_OPTIONS` string passed to every env at start
includes a trailing `"!status_updates"` token (commit `ce74ba2a`).
This sets `iflags.status_updates = FALSE`, which:
1. Short-circuits `bot()` at `botl.c:241` (the status-line formatter).
2. Short-circuits `recalc_mapseen()` at `dungeon.c:2467` via the
   `if (!iflags.status_updates) return;` early-return.

Neither of those code paths produces output the agent reads
(`blstats` comes from `update_blstats` reading `u.uX` directly;
`chars`/`message` come from independent paths). But the flag
**does** branch internal game-loop control flow (e.g.
`allmain.c:378`'s `timebot()` call), so trajectories with the flag
ON vs OFF diverge byte-for-byte after a few steps.

Performance cost of reverting (status_updates=TRUE):

| N    | OFF (current) | ON       | Cost |
|------|---------------|----------|------|
| 128  | 45–54K SPS    | 33–38K   | -30% |
| 1024 | 62–64K SPS    | 48–58K   | -15% |

The flag is load-bearing for perf. **Golden trajectories must be
re-captured whenever this flag flips.** The current 16-seed
`ocean/nethack/golden/` set is bound to `!status_updates`.
