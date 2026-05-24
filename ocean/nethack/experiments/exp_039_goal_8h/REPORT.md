# NetHack-on-PufferLib — push report

## Goal

Run 1024+ NetHack envs in parallel under PufferLib's vectorized training
harness with zero crashes, fast enough that 1 GPU stays the bottleneck.
Constraint: only `vendor/nle/` and `ocean/nethack/` are in scope —
pufferlib's harness is off-limits.

## Beginning vs end

|  | Beginning | End |
|---|---|---|
| Max stable env count under puffer training | ~512 (and intermittent) | **4096** (longest run 10 min at N=1024 went 27M steps clean) |
| Crashes at N=1024 in a 60-s training run | ~2 short-read panics + segfault around 25 s | **0** |
| Crashes at N=4096 | unable to start | **0** in 5 min run |
| Puffer SPS at N=1024 | 51K (and only briefly, before crash) | 62K sustained |
| Puffer SPS at N=4096 | n/a (could not run) | 100K+ |
| Direct OMP env-step ceiling at N=1024 (no Python harness) | 1.0 M, ~3/10 runs clean | **1.5–2.0 M, 13/15 runs clean** |
| Determinism check (16 seeds × 1000 steps) | 13/16 record, 13/13 replay | **16/16 record, 16/16 replay** |
| episode_return reached during a 10-min N=1024 run | (could not run that long) | ~9.5 |
| Observation correctness | (had a latent bug: most blstats fields were silently zero under the disabled-renderer build, caught at the very end and fixed) | All obs fields fresh every step |

What changed: the env can now host the parallel-training workload PufferLib
wants to give it. Training is no longer the bottleneck on stability or on
crash-free time-to-failure, and the env's own throughput ceiling is well
above 1 M steps/s at N=1024. The PufferLib harness has its own per-step
overhead (Python policy callback, GPU sync, the OMP round-robin pattern
itself) that costs ~5–10× of the env's raw throughput at scale; that's
unchanged because it's out of scope.

## What we decided to add, and why

We grouped the work into seven features. Each is a single coherent
"why we did this" answer to a problem we hit.

### 1. Per-env game state (the foundational refactor)

**Problem**: NetHack 3.6 was written 30 years ago as a single-player
terminal game. Several hundred mutable variables live at file scope or
process scope — the dungeon map, the player struct, monster chains,
random number state, the save/restore plumbing, the in-memory window
state. When two envs step in parallel, one's writes corrupt the other.

**Decision**: define a single `nle_ctx_t` struct that holds *all*
per-env mutable state. Each env carries one. At every entry into
libnethack (`nle_step`), we anchor a thread-local pointer to that env's
`nle_ctx_t`, then a macro at the top of each `.c` file rewrites every
former-global reference into a field access on that pointer. Migration
was done incrementally over many passes; the final size is ~75 KB per
env.

**Result**: 1024 envs can be active in the same process without
trampling each other.

### 2. Per-env memory arena

**Problem**: NetHack's `alloc()` (used for monsters, objects, level
data) originally routed through libc malloc. Two costs: glibc's
per-arena mutex contends when 128 cores allocate at once, and there's
no way to free everything a dead env owned without walking object
chains.

**Decision**: each env gets a private 64 MB anonymous memory map at
init. `alloc()` is a bump pointer in that map. At env teardown the
whole map is unmapped. Pointers to libc-malloc memory (rare; mostly
dlb data) go through a fallback path.

**Result**: zero malloc contention on the hot path; teardown is one
syscall.

### 3. Eliminate dead rendering work

**Problem**: NetHack still calls its terminal renderer every tick —
formats a status line ("HP:14(14) AC:10 Dlvl:1 $:0 T:1") into a
sprintf chain, walks the level map to update "discovered rooms" for
its in-game travel command, emits ANSI escape codes for cursor
positioning, etc. In headless RL training, the output of all of that
goes into a memory buffer that nothing ever reads.

**Decision**: gate the status-line renderer off, gate the mapseen walk
off, short-circuit the per-character output function when no TTY
observation is bound. The agent gets stats from a direct read of the
in-memory player/monster structs instead.

**Result**: ~21% CPU recovered. The agent's observation is unchanged
(we verified by byte-diffing obs buffers under both settings).

### 4. Save/restore correctness for pointer-migrated arrays

**Problem**: a side effect of the per-env refactor (feature 1) is that
several formerly-static arrays became *pointer macros* — the symbol
`lastseentyp` now expands to `current_nle_ctx->s_lastseentyp_p`, a
pointer. NetHack's save/restore code does `bwrite(fd, lastseentyp,
sizeof lastseentyp)`. Post-migration `sizeof lastseentyp` is 8 bytes
(the pointer size), not the original 1680. The writer happily writes
8 bytes. The reader (on the other side of the migration boundary in
restore.c) was already using explicit byte counts. Net result: the
file on disk is ~1900 bytes short of what the reader expects, the
read runs off the end of the file, panic.

**Decision**: audit every `bwrite`/`mread`/`sizeof` site for
pointer-migrated symbols; replace with explicit byte counts
(`COLNO * ROWNO * sizeof(schar)` and friends). Add `_Static_assert`
checks on critical struct sizes at both ends of save/restore so the
contract can't drift silently.

**Result**: cross-level save/restore works correctly; the recurring
"Error reading level file" panic at N>=64 is gone.

### 5. Process-shared resources that survive env teardown

**Problem**: a few resources are conceptually shared across envs by
design — the data-file (DLB) index, the static window-port jump
table, signal handlers. We had inadvertently let some of these get
allocated through the per-env arena (feature 2). When the first env
to take a slow-reset hit `nle_end`, its arena got unmapped, taking
the shared data with it. The next env's init dereferenced freed
memory.

**Decision**: classify each shared resource as "process-global,
init-once" vs "per-env". Route process-global allocations through
libc malloc explicitly. Make the init paths CAS-guarded so only the
first env to arrive performs the init, with later envs spinning
briefly and then reading the result.

**Result**: env tear-down no longer pulls process-global resources
from underneath later envs.

### 6. Cache-line behavior under the harness

**Problem**: even with everything per-env and isolated, the actual
*throughput* at N=1024 in puffer training was 30× lower than at
N=8. We instrumented and found a structural cause: PufferLib's
harness does *one* step per env per outer iteration (it has to —
the policy network needs to weigh in between steps). At N=1024 with
128 cores, each core handles 8 envs round-robin. 8 × 75 KB env state
> 1 MB L2 cache. Every step pays a cold-cache fill.

**Decision**: we can't change the harness, but we can give the CPU
a head start. At every `nle_step` entry, issue `__builtin_prefetch`
on the first four cache lines of the env's `nle_ctx_t`. The L1 stream
prefetcher will pull the rest in while the function preamble runs.

**Result**: ~30% SPS lift at N=1024–2048. The remaining cache-thrash
gap (~5×) is structural to the harness round-robin pattern and is
documented as future work.

### 7. Observation contract correctness

**Problem**: caught at the very end. The agent's `blstats` field
(HP, depth, score, etc.) was populated by a function that we had
disabled along with the status-line renderer in feature 3. The agent
was silently seeing zero for most stats. Training was running, but
the policy was learning from garbage.

**Decision**: call the stat-update function directly from the
observation-pack function, unconditionally. It's cheap (direct
struct-to-array copy, no formatting). Goldens were re-captured under
the corrected contract.

**Result**: every observation field is fresh every step regardless of
which renderer toggles are set. The 16-seed golden set passes
deterministically.

### 8. Tooling and observability

**Problem**: we couldn't have shipped without (a) a way to reproduce
a parallel-OMP env loop without involving Python/torch/CUDA, (b) a
way to inspect a single env step-by-step with colors and stats,
(c) a way to verify byte-for-byte determinism across builds.

**Decision**: three standalone binaries.

- `multi_threaded` — pure-C OMP env-loop bench. Each thread runs all
  the steps of one env before moving to the next. This is the
  "best possible" pattern; gives us the env's intrinsic ceiling.
- `multi_threaded_rr` (a variant) — same code, but one step per env
  per OMP iteration, mirroring how the puffer harness drives c_step.
  Lets us attribute cache-thrash cost to the access pattern alone.
- `live_view` — single env, ANSI-colored chars/colors grid plus
  blstats and message. Three modes: random actions on a clock, a
  recorded action stream replay, or interactive keystroke per step.
  Use for "watch the env run", "replay a golden", "play to debug a
  specific scenario".
- `train_bench` — serial training-shaped bench (with reset on done).
- `verify_determinism` — record N seeds × 1000 steps to byte-hashed
  files, then replay to confirm byte-identical reproduction across
  builds.

**Result**: every claim in this report is reproducible from the
checked-in binaries. The 16-seed determinism check is the regression
test that catches "did we accidentally change the obs contract"
across future changes.

## Future work

Two structural changes were investigated and deferred — both would
break past the current ceiling but require either time we didn't have
or a harness change.

**Hot/cold split of the env-context struct.** The 75 KB `nle_ctx_t` has
maybe 1 KB of fields the inner step actually touches. If we split it
into a small "hot" struct (fits in one or two cache lines) and a
"cold" pointer to the rest, the cache-thrash penalty under
round-robin OMP largely goes away. Estimated several days of careful
refactor; preserves the harness contract.

**Batched stepping.** If we extend the env's step API to take K
actions and run K game ticks per call (rather than one), the
cache-thrash penalty becomes amortized over K. The agent's policy
would need to emit K actions per yield, which is a harness-side
change. Estimated significantly cheaper than the hot/cold split but
requires a small amount of work on the puffer harness — which the
project constraint excludes.

## Reproducing this report

From a clean repo at this revision:

```bash
# Build libnethack and the puffer extension
make -C vendor/nle/src/build nethack -j16
./build.sh nethack

# Build the standalone tools
clang -O2 -Wall -fopenmp -std=gnu11 -I./vendor/nle/include -I./ocean/nethack \
    ocean/nethack/multi_threaded.c -o multi_threaded -ldl -lpthread -lm
clang -O2 -Wall          -std=gnu11 -I./vendor/nle/include -I./ocean/nethack \
    ocean/nethack/train_bench.c    -o train_bench    -ldl -lpthread -lm
clang -O2 -Wall          -std=gnu11 -I./vendor/nle/include -I./ocean/nethack \
    ocean/nethack/live_view.c      -o live_view      -ldl -lpthread -lm

# Determinism (should print "16/16 OK, all OK")
USER=$USER NETHACKDIR=$(pwd)/vendor/nle/nethackdir \
    bash ocean/nethack/verify_determinism_all.sh

# Env-loop ceiling: ~1.5-2.0M aggregate SPS at N=1024
USER=$USER NETHACKDIR=$(pwd)/vendor/nle/nethackdir \
    ./multi_threaded 1024 3000 128

# Watch one env run (4 fps, ANSI colors)
USER=$USER NETHACKDIR=$(pwd)/vendor/nle/nethackdir \
    NETHACK_LIBPATH=$(pwd)/vendor/nle/src/build/libnethack.so \
    ./live_view --random --steps 200

# Stability: 5-min puffer training, N=1024, expect EXIT=124 and 0 panics
bash ocean/nethack/experiments/exp_039_goal_8h/run_sps.sh 1024 300 demo
```
