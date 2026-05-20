# nle_state Refactor Roadmap

## Goal

Eliminate the dlopen-per-instance hack in `ocean/nethack/nethack.h` by moving
NetHack's ~120–150 process-global variables into per-instance storage,
accessed through `current_nle_ctx` (or a thread-local equivalent for
multi-thread single-process scaling).

When complete, N independent NetHack envs can share **one**
`libnethack.so` mapping, each with its own state struct on the heap.
This unblocks two scaling problems we've observed:

1. **Loader-lock serialization** kills multi-thread reset throughput in
   one process (exp_014: 32 cores regressed vs 16 cores).
2. **Memory overhead** of N copies of libnethack (~5 MB per env) caps
   how many envs fit on a node.

The audit at `experiments/exp_017_globals_audit/AUDIT.md` enumerates the
scope. This roadmap sequences the work.

## Why incrementally

A single big-bang refactor of all 150 globals + 30 macros is high-risk:
one bug invalidates the entire run. Subsystem-by-subsystem migration:

- Each subsystem is bounded scope (10–30 globals).
- Determinism harness at `ocean/nethack/verify_determinism.c` + golden
  trajectory at `ocean/nethack/golden/golden_seed42_1k.bin` catches any
  behavioral drift at the first mismatching step.
- We can pause / resume between subsystems.
- The dlopen path keeps working through the migration — no dlopen
  removal until the last subsystem lands.

## Subsystem migration order (from the audit's recommendation)

Each stage produces a green build + matching golden trajectory.

### Stage 1 — RNG ⬅ in progress

State: `rnglist[2]` (static in `rnd.c`), `nle_seeds[2]` (`hacklib.c`),
`has_strong_rngseed` (`decl.c`).

Files touched: `vendor/nle/src/include/nle.h`,
`vendor/nle/src/src/nle.c`, `vendor/nle/src/src/rnd.c`.

Approach: move `isaac64_ctx[2]` and the init flags into `nle_ctx_t`.
Expose accessors `nle_rng_state(idx)` and `nle_rng_init_flag(idx)` from
`nle.c`. Keep the function-pointer table (`rnglist_fn[2]`) static —
those constants are identical across instances.

Verification: golden replay must pass.

### Stage 2 — NLE wrapper layer

State: `current_nle_ctx`, `settings`, `nle_seeds_init` (all in
`nle.c`), the `nle_settings` struct field accesses scattered through
NLE code. Also `program_state` (decl.c).

Approach: most of this is already inside the `nle_ctx_t` model
conceptually. Move `nle_seeds_init` and `settings` into the ctx
itself.

### Stage 3 — `flags` / `iflags` / `sysflags` / `Cmd`

State: three big option structs in `decl.c:136` + `options.c:10–14`.

This is **the hardest stage** because `flag.h` (lines 28–32, 459–469)
defines macros like `wizard ≡ flags.debug`, `discover ≡ flags.explore`,
`use_color ≡ wc_color`. Every macro must be rewritten:

```c
// before
#define wizard flags.debug
// after
#define wizard (current_nle_ctx->state->flags.debug)
```

Probably needs a sed pass over all .c files plus careful review of any
code that takes the address of a flag (which won't work via macro).

### Stage 4 — Player state

State: `struct you u` (in `decl.c`), `youmonst`, body slots (`uwep`,
`uarm…`), `urealtime`, `ubirthday`.

`u` is read/written everywhere. Same macro-substitution approach as
Stage 3 — define `u ≡ (current_nle_ctx->state->u)`.

### Stage 5 — Dungeon topology

State: `dungeons[]`, `dungeon_topology`, `sp_levchn`, `branches`,
`mapseenchn`, `level_info`, `n_dgns`.

Most are already accessed via macros that can be rerouted.

### Stage 6 — Current level

State: `struct dlevel_t level` (~40 KB) + vision arrays + light state.

Big in bytes but few macros to rewrite (`levl ≡ level.locations`).

### Stage 7 — File I/O / save / restore

State: `SAVEF[]`, `lock[]`, `fqn_prefix[]`, `hackpid`, `restoring`.

Care needed: file paths must be unique per instance to avoid races
between concurrent envs (this is already handled by our per-instance
vardir in `nethack_make_vardir`).

### Stage 8 — Display / message buffers

State: `toplines`, `WIN_*` winids, message buffers, TMT terminal state.

Largely cosmetic — most of these don't affect game logic, just the obs
fields we read.

### Stage 9 — Misc cleanup

`occupation` / `afternmv` function pointers (save-restored by name —
look-up via name table is fine).
`Cmd.commands[256]` table.
Regex chains in windowports.
`obufs[]` round-robin.

### Stage 10 — Remove the dlopen hack

`ocean/nethack/nethack.h:269` `nethack_load_lib` collapses to a regular
single-time `dlopen` (or static link if we want). Per-env state lives
entirely in `current_nle_ctx`.

## Verification protocol (run after every stage)

```
# 1. Rebuild patched libnethack
cd vendor/nle/src/build && cmake --build . --target nethack -j8

# 2. Replay the 1K-step golden against the new build
NETHACK_LIBPATH=$PWD/vendor/nle/src/build/libnethack.so \
NETHACKDIR=$PWD/vendor/nle/nethackdir \
  ./verify_determinism replay --in ocean/nethack/golden/golden_seed42_1k.bin
# Must print "OK — 1000 steps match". On mismatch, bisect to find
# which call site changed behavior.

# 3. Smoke test the standalone bench
NETHACK_LIBPATH=$PWD/vendor/nle/src/build/libnethack.so \
NETHACKDIR=$PWD/vendor/nle/nethackdir \
  ./nethack bench 10000
# Must complete without crash and report steps/sec.

# 4. (After Stage 10) Multi-env test
# Run two envs in one process WITHOUT dlopen-per-instance; both must
# produce independent golden trajectories with different seeds.
```

## Pragmatic alternative: heap-aware fast-reset (exp_019)

While the full refactor proceeds, an **arena-based fast-reset** can
deliver in-process fast resets without rewriting NetHack's globals. The
trade-off: still needs dlopen for per-instance isolation, but each
instance's reset becomes a O(µs) memcpy.

Investigation in `exp_019_heap_aware_reset/`. If that works at
production-quality, the full refactor becomes a lower-priority cleanup
rather than a critical-path scaling lever.

## Estimated effort (rough)

Stage 1 (RNG): ~half-day  (in-progress)
Stage 2 (NLE wrapper): ~half-day
Stage 3 (flags): 1–2 days
Stage 4 (player): 2–3 days
Stage 5 (dungeon): 1–2 days
Stage 6 (level): 1–2 days
Stage 7 (I/O): 1 day
Stage 8 (display): 1 day
Stage 9 (misc): 1–2 days
Stage 10 (remove dlopen + multi-instance test): 1 day

**Total: ~2-3 weeks of focused work.** Best done by one engineer with
deep context, not parallelized across many agents.

## Coordination with parallel workstreams

- exp_019 (heap-aware fast-reset, currently in progress via subagent):
  if it delivers a working in-process reset, much of the urgency for
  the full refactor disappears. Re-prioritize after exp_019 lands.
- The `nle-exposed-nethack` git branch preserves the pre-refactor
  state for reference benchmarks throughout the refactor.
