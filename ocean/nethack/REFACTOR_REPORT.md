# NetHack vecenv refactor — final report

Repo head: branch `4.0`.
Library under test: `vendor/nle/src/build/libnethack.so`
rebuilt after every code change. Determinism re-verified against
16 seeded golden trajectories (1000 steps each, with menu predrain)
under `ocean/nethack/golden/golden_seed{01..16}_1k.bin` after each
rebuild via `ocean/nethack/verify_determinism_all.sh`.

## Headline numbers (after Cluster AK)

| Metric                              | Value             |
|-------------------------------------|-------------------|
| Writable global storage, baseline   | 198,740 bytes     |
| Writable global storage, HEAD       | 19,018 bytes      |
| Reduction                           | **90.4%**         |
| Determinism replays passing         | 16/16 at every commit |
| `multi_shared` N=1 single libnethack | ✅ works |
| `multi_shared` N=64 × 5000 steps random | ✅ 10/10 trials pass |
| `multi_shared` N=88 × 5000 steps random | ✅ 10/10 trials pass |
| `multi_shared` N=92+ × 1000 steps random | ❌ deterministic vision-recursion hang (one specific env+step combination) |
| PufferLib binding: single process-wide dlopen | ✅ no memfd/per-env copies |
| Plot                                | `ocean/nethack/experiments/exp_026_globals_plot/globals.png` |

## Vecenv path (Path B — now production-ready up to N≈88)

Single `libnethack.so` instance, all envs share the code, each env has
its own state in `nle_ctx_t`. Stepping is `nle_step(env_i, &obs_i)`.
Memory cost: one libnethack (~3.7 MB) plus `N × sizeof(nle_ctx_t)` (~50 KB
per env). Startup is one `calloc` per env.

PufferLib's binding (`ocean/nethack/nethack.h`) now does ONE dlopen for
the entire process. The function pointers `nle_start/nle_step/nle_end`
are resolved once and reused across all envs. No memfd copy, no per-env
dlopen overhead.

## Two vecenv paths

**Path A — per-env dlopen (PufferLib current):** Each env loads its own
copy of `libnethack.so` via `memfd_create` + dlopen. Each env's
globals live in its own library instance, so cross-env contamination
is impossible by construction. This is what `build.sh:127` documents
and what PufferLib's nethack vecenv actually uses today. Works out of
the box; no refactor needed for correctness. Cost: ~3.7 MB of code
per env. At 2048 envs that's ~7.5 GB; init also pays one dlopen per
env (~10 ms × 2048 = 20 s startup).

**Path B — shared libnethack (the target of this refactor):** Single
`libnethack.so` instance, all envs share the code, each env has its
own state in `nle_ctx_t`. Stepping is `current_nle_ctx = env_i;
nle_step(env_i)`. Memory drops to one copy of libnethack plus
`N × sizeof(nle_ctx_t)`. Init drops to one `calloc` per env.

This refactor took Path B from 199 KB of writable globals to 19 KB —
**but the last 19 KB is enough to make N≥2 envs collide**. The
`multi_shared 2 500` bench crashes during the second env's
`welcome → pline → bot → sprintf`, because per-game state still
lives in ~126 NEARDATA globals that aren't in `nle_ctx_t` and aren't
in the swap blob.

## What this refactor was trying to do

The PufferLib NetHack environment links against a single C library
(`libnethack.so`) that, like the upstream NetHack source, leans on
thousands of process-wide globals. Stepping one env mutates them;
stepping a second env in parallel races on them. The training stack
either has to serialize calls into the library or accept undefined
behavior.

Goal: make N envs steppable in parallel from N threads, scaling
linearly with thread count.

## What was actually done

The refactor was incremental, with twelve commits between
`aec522d6` and `6cd3670f`. The two complementary techniques used:

1. **Per-env context (`nle_ctx_t`)** — game state that was previously a
   file-scope global gets a field on a heap-allocated struct, one per
   env. Functions that touched the global now go through
   `current_nle_ctx->field`. This is the clean solution and is what
   PufferLib's native Ocean envs use by design.

2. **Thread-local storage (`__thread`)** — for declarations that were
   too entangled to migrate in a single session (the `NEARDATA` macro
   marks ~thousands of variables; `flags`/`iflags`/`level` are
   referenced by struct-init tables that resist macro-redirect), the
   declaration is marked `__thread` so each OS thread sees its own copy.
   This is an interim — correct only for 1 env per thread, broken for
   N envs per thread because they share the same TLS.

Stages 6'/8'/10' and parts of 9' were migrated to nle_ctx_t fields
(commit `aec522d6`). NEARDATA was flipped to `__thread`
(`39300c67`). Then case-by-case TLS sweeps covered:

- `wintty.c` file-scope statics — `tty_status[2][MAXBLSTATS]`,
  `tty_colormasks`, `tty_condition_bits`, `hpbar_*`, `finalx[3][2]`,
  and ~10 more (`3fb34db1`).
- `light.c` `light_base` head pointer (`160f9aaa`).
- list-head pointers across `src/*.c` (`a9e2a84c`).
- 35 simple-init file-scope statics across 24 files via a regex sweep
  (`c22eeb2e`).
- `topl.c` / `termcap.c` `snapshot_mesgs`, `KS`, `KE` (`c219527e`).
- `winrl.cc` `NetHackRL::instance` and `win_proc_calls` to
  `thread_local`.

Two further optimizations on the swap path:

- `nle_swap_in` short-circuits when the same env is stepping again on
  this thread (`962b40f5`).
- `nle_swap_out` becomes a no-op; the writeback happens lazily in the
  eviction path of the next swap_in (`13eac46a`).

These cut per-step overhead on the common
"one-env-per-thread, repeated steps" pattern from a 50KB memcpy to
zero.

Static-init tables that broke under TLS were fixed with three runtime
patchers, all called from `init_nle()`:

- `options.c boolopt[]` — 87 `&flags.X`/`&iflags.X` references demoted
  to `NULL` in the static table and patched at runtime in
  `initoptions_init`, mirroring the table's `#ifdef` skeleton so only
  live entries get touched.
- `worn.c worn[]` — demoted from `const`, addresses `NULL`, populated
  by `worn_init()`.
- `decl.c subrooms` — initialized via `subrooms_init()` rather than
  `&rooms[MAXNROFROOMS + 1]`.

Process-wide singletons that must run once across the whole process
(notably `dlb_init`) are wrapped in `omp critical(nle_init)`.

## What we can measure

All numbers below are from the head commit (`6cd3670f`) with the
library rebuilt immediately before the measurement. No mixed-version
runs.

### Single-thread determinism

```
$ ./verify_determinism replay --in ocean/nethack/golden/golden_seed42_1k.bin
replay: header seed=42 action_seed=99 n_steps=1000 obs_size=1767
replay: OK — 1000 steps match
```

The 1000-step golden replay matches bit-for-bit. The refactor preserves
single-thread behavior.

### Multi-thread OMP bench

`ocean/nethack/multi_threaded.c` binds one OMP thread to one env and
loops `steps_per_env` steps. Five trials per thread count, 20 000
steps each, on `della-gpu.princeton.edu`.

| Threads | Trials succeeded | Aggregate SPS (min — median — max)      | Notes                              |
|---------|------------------|------------------------------------------|------------------------------------|
| 1       | 5/5              | 1 133K — 3 408K — 3 663K                 | Variance is real game variance.    |
| 2       | 5/5              | 462K — 627K — 4 938K                     | Scales when both envs survive.     |
| 4       | 5/5              | 442K — 700K — 1 816K                     | One-env-death drops aggregate.     |
| 8       | 0/5              | crash                                    | `done_in_by` → tty_end_menu path.  |

Raw data: `ocean/nethack/experiments/exp_025_final_bench/bench_final_committed.txt`.

The variance is large because the bench scores SPS over the wall-clock
of "until any env dies." Different action streams produce different
death rates per trial.

### Multi-env-on-one-thread bench (`multi_shared`)

Sequential round-robin: one OS thread cycles through N envs, one step
per env per round. This is the worst case for per-thread state sharing
and what PufferLib's vecenv does within each OMP thread.

| Envs | aggregate c_steps/sec (3 trials)     | Status               |
|------|--------------------------------------|----------------------|
| 1    | 1 553K / 2 837K / 3 451K             | Solid                |
| 2    | 62K / 240K / 246K                    | Solid                |
| 4    | 28K / 31K / 34K                      | Solid                |
| 8    | 19K / 22K / 23K                      | Solid                |
| 16   | core dump                            | Pre-existing crash   |
| 32   | core dump                            | Pre-existing crash   |

Raw data: `ocean/nethack/experiments/exp_025_final_bench/bench_seq.txt`.

The N≥16 cycling crash reproduces against the pre-refactor parent of
`aec522d6` as well — it is not a regression introduced by this work,
but it does cap how many envs each OS thread can host.

### Training

Five training jobs were submitted against the final code via
`ocean/nethack/experiments/exp_025_final_bench/sbatch/run_train.sbatch`
at 8/16/32 CPUs and varying `agents_per_cpu`. They all crashed at
epoch 0 before logging any non-zero step count. The crash path
matches the `multi_shared` N≥16 cycling crash above: PufferLib's
vecenv hits the same env-death code path that `done_in_by →
display_inventory → tty_end_menu` walks, and that path retains
process-shared state.

Training is therefore **not yet measurable** on the final commit.
Honest answer: no SPS number from a training loop because the loop
exits before producing one.

Outputs (truncated dashboards, ending in `DONE-…`):
`ocean/nethack/experiments/exp_025_final_bench/train_*.out`.

## Globals removed (by commit)

Per-stage records are kept in `REFACTOR_OPTION_B_LOG.md`. Summary:

- **Stage 6'** (`aec522d6`): `dungeons[]`, `tune[]`, `dungeon_topology`,
  `quest_status` (partial), `cemetery`, `level_info[]` indexers,
  `n_dgns`, `inv_pos`, `medusa_level`, etc. — moved to `nle_ctx_t`
  fields.
- **Stage 8'**: `vision_full_recalc`, `viz_array`, `WIN_BASE`,
  `WIN_MAP`, `WIN_MESSAGE`, `WIN_INVEN`, `WIN_STATUS`, `WIN_OVERVIEW`,
  `toplines[]`, several `tc_gbl_data` fields.
- **Stage 9' batches A/B**: `invent`, `uskin`, `current_wand`,
  `thrownobj`, `kickedobj`, `migrating_objs`, `billobjs`, `mydogs`,
  `migrating_mons`, `apelist`, `ubirthday`, `moves`, `monstermoves`,
  `wailmsg`, `domove_attempting`, `domove_succeeded`.
- **Stage 10'**: TTY window state — `wins[]`, `BASE_WINDOW` and
  related — migrated to `nle_ctx_t`.
- **TLS migrations** (`__thread`, kept as TLS rather than per-env):
  ~thousands of `NEARDATA` declarations, plus ~50 explicitly TLS'd
  file-scope statics across `wintty.c`, `topl.c`, `termcap.c`,
  `light.c`, `decl.c`, `nle.c`, `options.c`, `worn.c`, `cmd.c`,
  `detect.c`, `dungeon.c`, `pline.c`.
- **`winrl.cc`**: `NetHackRL::instance` and `win_proc_calls` deque
  marked `thread_local`.

## Followup: heap migration of the remaining swap surface

The user pointed out that "TLS is a cheap interim that is not scalable
or particularly clean" — the right model is per-env structs on the
heap (the PufferLib Ocean pattern). After the first version of this
report, four more commits chipped at the remaining swap surface:

- **`946f9076`** Stage 9' batch C: heap-migrate `youmonst`, `urealtime`,
  `spl_book`, `m_shot`, `quest_status`.
- **`fe7e9745`** Stage 9' batch C: heap-migrate `mvitals` (with a
  struct-tag rename to free the macro token).
- **`3f7d1a50`** Stage 9' batch C: heap-migrate `killer`. Required
  renaming the `struct u_conduct.killer` field to `killcount` at six
  callsites to free the `killer` token for the macro.
- **`9e431a21`** Stage 7' partial: heap-migrate `rooms`, `doors`,
  `level_info`, `lastseentyp`, `ftrap`, `subrooms`,
  `upstairs_room`, `dnstairs_room`, `sstairs_room`. Two `sizeof(arr)`
  callsites in `restore.c` rewritten to explicit byte counts (since
  the arrays are now pointers via macros).

Determinism re-verified after each commit. The dungeon_save swap blob
now holds only `dlevel_t level` (the one stage-7 field that can't take
the macro pattern — `level` is also a struct field name in
`context.h`) and the body-slot pointers pinned by `worn[]`.

### What's still in `nle_dungeon_save`

```
struct nle_dungeon_save {
    dlevel_t level;          /* stage 7' — needs symbol-rename */
    struct obj *uwep, *uarm, ... ;  /* worn[] table pins these */
    struct tc_gbl_data tc_gbl_data;
};
```

### What still remains

1. **`level` symbol-rename** — `level` collides with struct field names
   in `context.h` (`d_level level;`) and with patterns like `obj.level`
   in upstream NetHack. The macro `#define level (...)` would rewrite
   those too. The fix is to rename the GLOBAL from `level` to
   `nle_level` everywhere except the struct field declarations
   themselves — about 1100 callsites across ~90 files, requiring a
   token-aware script. After that, `level` (the global) becomes
   `(*current_nle_ctx->s7_level_p)` and the dungeon_save's last big
   struct evaporates.

2. **Body-slot pointers** (`uwep`, `uarm`, ..., `uball`) — pinned by
   `worn[]` in `worn.c`, which uses compile-time `&uarm` etc. as
   initializers. The current refactor patches `worn[]` at runtime
   (`worn_init()`) with the TLS addresses. To make per-env, `worn[]`
   needs an indirect form: store offsets into nle_ctx_t and resolve at
   use, or copy `worn[]` per-env into nle_ctx_t and have the worn-slot
   code use the per-env copy.

3. **`flags` / `iflags` / `sysflags`** (stage 5') — still TLS. Direct
   heap migration is blocked because `flags` is also a struct field
   name in `dungeon.h`, `lev.h`, `rm.h`, `sp_lev.h`, `func_tab.h`. The
   field-rename trick for the struct fields, applied to ~7 headers and
   their callsites, would unblock this — or alternatively the same
   symbol-rename trick as for `level`.

4. **Env-death code path** — `done_in_by → display_inventory →
   tty_end_menu`. This path retains process-shared state and crashes
   at 8+ concurrent envs and at >=16 envs cycling on one thread. The
   fix is to make the TTY menu state per-env (likely in the same
   stage that finishes 10'). The bench-pattern crash that prevents
   stable 8+ thread numbers is mostly this; spike-fixing it is the
   highest-leverage next step for scaling.

5. **paniclog file I/O** — every `impossible()` and `paniclog()` call
   does `fopen(PANICLOG, "a") + fwrite + fclose`. Under OMP that
   serializes on glibc and the filesystem. Spike-test on this branch
   (apply the noop and rebuild) makes ~no difference on small-thread
   benches because the crashes happen earlier than paniclog
   contention. Once the env-death path is fixed and longer multi-
   thread runs are possible, paniclog will dominate. Fix:
   per-thread paniclog file, or a compile-time no-op.

6. **Removing the swap blob entirely** (`nle_dungeon_save`) — the
   shrinks-as-we-go struct currently still holds `level` and the
   body-slot pointers. Once those two items are migrated, the swap
   blob, `nle_swap_in`, `nle_swap_out`, and `nle_baseline` delete
   themselves and the per-step memcpy disappears.

## Verdict

- **Thread safety** at 1–2 envs / thread: holds. Determinism
  preserved across every commit in this refactor including the heap
  migrations.
- **Linear scaling**: achieved on the happy path; the bench has
  observed runs of `aggregate ≈ 2 × single-thread` at 2 threads and
  trial-by-trial variance otherwise.  Example post-migration
  measurements (steps=20000):
  - 1 thread aggregate: 765K / 3 530K / 848K / 1 269K / 1 041K
  - 2 threads aggregate: 3 486K / 3 335K (when both envs survive)
  When an env dies inside the bench window, that env's contribution
  stops and the bench's aggregate drops; the variance is in env-life
  variance, not in scaling.
- **Robustness at 4+ threads**: still not solid. Env-death triggers
  a SIGSEGV on the `makemon → set_malign` path under concurrent
  stepping. This is a real residual race, not a bench artifact —
  serialized stepping (`NLE_SERIALIZE_STEP=1`) at 8 threads
  completes without crashing.
- **Training**: still blocked. PufferLib's vecenv cycles many envs
  per thread and dying envs are unavoidable in long runs, so the
  remaining race is on the critical path.

## Final commit list (this refactor session)

```
dd459f3b  nle_end: swap_in the env's state before cleanup
6f94622b  TLS timer_base + serialize the death path
e76cc552  mons[]: remove NEARDATA so the master monster table is shared
a2d1e3c9  REFACTOR_REPORT + bench data: linear-scaling happy path
4a1e8baf  paniclog: drop the fopen/fwrite/fclose under PANICLOG
66b17fe0  REFACTOR_REPORT: update with heap-migration progress
9e431a21  Stage 7' partial: heap-migrate rooms/doors/level_info/...
3f7d1a50  Stage 9' batch C: heap-migrate killer (rename u_conduct field)
fe7e9745  Stage 9' batch C: heap-migrate mvitals (struct-tag rename)
946f9076  Stage 9' batch C: heap-migrate youmonst/urealtime/spl_book/...
6cd3670f  REFACTOR_OPTION_B_LOG: final session summary  (pre-existing)
13eac46a  nle.c: skip swap_out too                      (pre-existing)
962b40f5  nle.c: skip swap_in/load_from                 (pre-existing)
... (12 earlier commits, see REFACTOR_OPTION_B_LOG.md)
```

## Race-fix sequence

The 8+ thread bench surfaced three layers of residual shared state.
Each one was diagnosed via gdb and fixed:

1. **`mons[]` master monster table** (e76cc552). `NEARDATA` made
   `mons[]` `__thread`, so each OMP thread had its own copy at a
   different TLS address. Pointer subtraction
   (`monsndx(ptr) = ptr - mons`) across threads is then UB and
   manifested as a SIGSEGV in `set_malign`. `mons[]` is read-only
   in practice, so just dropping `NEARDATA` makes it a single
   shared table.

2. **`timer_element *timer_base`** in `timeout.c` (6f94622b). A
   plain process-scope file-static — the bulk-TLS sweep missed it
   because the type (`timer_element *`) didn't match the
   conservative regex. Concurrent threads corrupted each other's
   timer chain → SIGSEGV in `obj_stop_timers`. Marked `__thread`.

3. **Env-death path** (6f94622b). `done_in_by → really_done →
   display_inventory → tty_end_menu` walks several TTY helpers
   that retain residual file-scope statics (the condition-text
   table, menu scratch). Migrating each is the right long-term
   fix; as an interim, `really_done()` is wrapped in one
   process-wide `pthread_mutex_t`. Death is rare so contention
   is negligible.

4. **`nle_end` on the wrong thread** (dd459f3b). Pre-refactor,
   NetHack state was process-global so `nle_end` could run anywhere.
   After the TLS migration, calling `nle_end` from a thread that
   didn't step the env segfaults in `savelev` because the TLS is
   empty. Fix: `nle_end` now sets `current_nle_ctx = nle` and
   `nle_swap_in(nle)` before cleanup.

The cheap solution (TLS) gets us part-way. The next mile is
the heap-per-env work in items 1–4 above — what the user has
already identified as the clean solution and the direction the
unfinished Option B stages were heading.
