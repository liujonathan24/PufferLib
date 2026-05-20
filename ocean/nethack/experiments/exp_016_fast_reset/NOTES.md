# exp_016 — Fast-reset prototype: snapshot/restore of libnethack writable segments

## TL;DR

A hacky in-process reset that **memcpy**s libnethack's writable .data/.bss
segments + the fcontext coroutine stack back from a buffer captured after
the first `nle_start` + welcome-drain — bypassing dlclose/dlopen/nle_start
entirely. Reset latency drops from ~4 ms to ~10 µs.

**Speedup on raw reset cost: ~430× (109K resets/sec vs 250 resets/sec).**

The prototype is intentionally not correct for full training use (see
"Known issue" below) — it's a benchmark of the reset memcpy itself,
which is what the user asked for.

## A/B results (back-to-back c_reset, no stepping)

| N resets | SLOW (dlopen+nle_start) | FAST (snapshot/restore) | speedup |
|---------:|------------------------:|------------------------:|--------:|
|        1 |        3.91 ms / reset  |        0.03 ms / reset  |    130× |
|       10 |        3.73 ms / reset  |        0.01 ms / reset  |    298× |
|      100 |        4.01 ms / reset  |        0.01 ms / reset  |    340× |
|     1000 |        3.98 ms / reset  |        0.01 ms / reset  |    395× |
|    10000 |              —          |        0.01 ms / reset  |       — |
|   100000 |              —          |        0.01 ms / reset  |       — |

Steady state: **109K fast resets/sec** (single thread).
Steady state: **250 slow resets/sec** (single thread, loader-lock-bound).

## Note on the 217 ms number from prior experiments

Earlier writeups (exp_009/012) cited "217 ms per reset" — that's the
**cold-cache first reset** cost. Once filesystem caches are warm, repeated
dlopens of the same memfd-backed library are ~4 ms (kernel paging /
glibc dl-cache reuse). The "raw" reset cost as measured by the `resets`
subcommand was already much lower than I'd been estimating in the
multi-process analysis. The 430× still matters because per-reset cost
becomes the loader-lock-serialized cost when multi-threaded resets are
contended — see the parallelism section below.

## Implementation

Patch lives in `vendor/nle/src/src/nle_fast_reset.c`. Exposes three
symbols: `nle_fr_snapshot`, `nle_fr_restore`, `nle_fr_destroy`. The
glob `src/*.c` in CMakeLists picks it up automatically.

1. `nle_fr_snapshot(nle_ctx_t* nle)`: returns an opaque blob containing
   - The `nle_ctx_t` struct contents (bytewise).
   - The full fcontext coroutine stack contents (size = `ssize - pagesz`
     to skip the bottom guard page; deboost.context's `sptr` is the TOP
     of the stack, stack grows down, so live bytes are
     `[sptr - ssize + pagesz, sptr)`).
   - All writable PT_LOAD segments of libnethack.so (found via
     `dl_iterate_phdr`, using a function-address probe to identify our
     own image), **minus the PT_GNU_RELRO prefix** (which is
     mprotect(PROT_READ) after dynamic linking — writing it segfaults).
2. `nle_fr_restore(nle_ctx_t* nle, void* snap)`: memcpys each saved
   chunk back, then re-anchors `current_nle_ctx = nle` (which the
   .data restore would otherwise revert to the snapshot-time value —
   harmless for single-instance but matters for future multi-instance).
3. `nle_fr_destroy(void* snap)`: frees the blob.

Bugs hit and fixed during bring-up:
1. **Naming clash** — struct typedef and function shared the name
   `nle_fr_snapshot`. Renamed struct to `nle_fr_snapshot_t`.
2. **Duplicate `current_nle_ctx` symbol** — `nle.h` declares it as a
   tentative definition without `extern`, so including it in two TUs
   created two definitions. Worked around by `#define` hiding the name
   before include, then `extern`-redeclaring it.
3. **GNU_RELRO crash on restore** — the writable LOAD segment's first
   ~33 KB is `mprotect`-ed read-only after dynamic linking (RELRO).
   Initial naïve snapshot tried to write back to those pages → SIGSEGV.
   Fix: scan for PT_GNU_RELRO in the same callback, page-align its end
   up, and start the snapshot range there.
4. **Stack pointer direction** — initially snapshotted `[sptr, sptr+ssize)`
   forward from `sptr`, which is **past** the end of the actual stack
   (deboost.context's `sptr` is the high end, stack grows down). The
   "READ" of garbage above sptr happened to land in adjacent mapped
   memory most of the time; the "WRITE" back crashed when those pages
   weren't writable. Fix: use `[sptr - ssize + pagesz, sptr)` for both
   read and write, where the leading page is the deboost guard page.

## Integration into `nethack.h`

Build flag `NETHACK_FAST_RESET=1` (default 0) selects the fast path in
`c_reset`. First reset always takes the slow path (the snapshot is
captured on the way out); subsequent resets go through `fn_fr_restore`.
If the loaded `libnethack.so` doesn't export `nle_fr_*` (i.e. the
prebuilt unpatched .so), we transparently fall back to the slow path
and warn once.

Three new dlsym lookups in `nethack_load_lib`; three new fn-pointer
fields and one `void* fr_snapshot` on the `Nethack` struct.

## Known issue: heap pointer staleness (NOT yet fixed)

`bench` with random policy crashes at ~10K steps when the agent dies
and `c_reset` is triggered for an in-flight episode.

Cause: NetHack stores pointers to heap-allocated objects (monster
chains, item lists, level structures, message log, etc.) in its `.data`
segment. The snapshot captures those pointers at "post-welcome-drain"
state. During play, NetHack `free()`s some of those objects and
`malloc()`s new ones. On restore, the .data pointers are reset to point
back at the old addresses — some of which have been freed and possibly
re-used by other allocations. Use-after-free / wild pointer dereference
follows.

This is the **fundamental reason the dlopen-per-instance hack exists**.
A correct fast-reset requires either (a) snapshotting the entire heap
in addition to .data/.bss (effectively `fork()`), or (b) eliminating
heap pointers from .data — i.e. moving all of NetHack's mutable state
into an arena (which is the proposed nle_state refactor).

For the `resets`-back-to-back benchmark, no `free`/`malloc` happens
between snapshot and restore, so the prototype works and the speedup
number is genuine. For real episodes, it's wrong.

## Why the 430× still matters

Even ignoring correctness, the prototype demonstrates:

1. **Single-thread reset throughput** can be O(100K/sec) with a pure
   memcpy strategy. Today it's 250/sec via dlopen. That's a ~3-order-
   of-magnitude ceiling.
2. **Loader lock disappears.** dlopen serializes on a process-global
   mutex. Once we replace it with memcpy-of-data-segment, N threads
   in one process can reset truly in parallel (limited only by
   memory bandwidth ~30 GB/s ÷ 280 KB/snapshot ≈ 100K resets/sec total).
3. **This is the upper bound** the functional refactor is targeting.
   The refactor will achieve it correctly (no heap aliasing) by
   eliminating heap pointers from .data — the per-state arena lives
   alongside `struct nle_state`, all allocations relative to it.

## Next steps

1. Continue to functional refactor (struct nle_state) — that path
   delivers correct in-process resets at the speed this prototype
   demonstrates.
2. Preserve this prototype as the "speed ceiling" reference point.
3. Move it to the `nle-exposed-nethack` branch as part of the
   dlopen-based preservation line — useful for benchmarking the
   refactor against.

## Reproducing the numbers

```
# Build patched libnethack.so once.
cd vendor/nle/src/build && cmake .. && cmake --build . --target nethack -j8

# Build slow + fast bench binaries.
cd ../../../..
MODE=fast OUTPUT_NAME=nethack_slow bash build.sh nethack
MODE=fast EXTRA_CFLAGS="-DNETHACK_FAST_RESET=1" OUTPUT_NAME=nethack_fr bash build.sh nethack

# A/B
export NETHACK_LIBPATH=$PWD/vendor/nle/src/build/libnethack.so
export NETHACKDIR=$PWD/vendor/nle/nethackdir
./nethack_slow resets 200
./nethack_fr   resets 10000
```

## Files

- `vendor/nle/src/src/nle_fast_reset.c` — the patch
- `ocean/nethack/nethack.h` — `NETHACK_FAST_RESET` build flag, dlsym
  lookups, fast-path c_reset
- `ocean/nethack/experiments/exp_016_fast_reset/NOTES.md` — this file
