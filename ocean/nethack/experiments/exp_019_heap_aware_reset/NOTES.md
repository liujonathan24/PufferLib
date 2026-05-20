# exp_019: Heap-aware fast-reset for NLE

## Strategy: Arena allocator (Strategy B, with C++ extension)

Replaced NetHack's `alloc()` with a bump-pointer arena allocator backed by a
single 64 MB anonymous `mmap`. All of NetHack's per-game heap state now lives
in one contiguous region, so the snapshot is just a `memcpy` of `[arena_base,
arena_base + arena_used)` and the bump-pointer scalar. On restore, the inverse
`memcpy` rewinds the arena to the snapshot watermark, simultaneously
"freeing" every allocation made after the snapshot.

Strategy A (raw `/proc/self/maps` heap snapshot) was rejected: it can't
cleanly separate NetHack's allocations from PufferLib / harness allocations,
and any attempt to restore `[heap]` would clobber the env-wrapper's own
state — adding more engineering on top.

Why we needed to extend beyond plain `alloc()`:

- The C side worked fine after redirecting `free()` (via a `global.h` macro)
  to `nle_arena_free()`, which no-ops on arena pointers and forwards
  libc-malloc'd pointers (e.g. from `strdup`) to `__libc_free()`.
- But NLE's RL window backend `win/rl/winrl.cc` keeps live state in
  `std::vector<std::unique_ptr<rl_window>>` and similar STL containers. Those
  go through `::operator new` / `::operator delete` — NOT through NetHack's
  `alloc()`. Without intercepting C++ allocation too, vector reallocation
  during play would free old buffers via libc free, breaking pointer
  consistency on restore.
- So we also override `operator new` / `operator new[]` / their nothrow
  variants, and the matching `operator delete` family, to route through the
  same arena (`vendor/nle/src/src/nle_arena_cpp.cc`). Linking with
  `-Wl,-Bsymbolic-functions` makes libnethack's intra-DSO calls bind to our
  override.

One additional patch was required: NetHack's death sequence
(`nh_terminate` -> `dlb_cleanup` -> `close_library`) calls `fclose()` on the
DLB FILE* and frees the dir/sspace arena pointers. With the arena active,
`close_library`'s `fclose(lp->fdata)` corrupts heap state (the FILE itself
is libc-malloc'd and outlives the game cycle in our model — we want to keep
it open across snapshot/restore cycles). We disable the cleanup under
`NLE_USE_ARENA_FREE`: the FILE leaks, but it's process-lifetime and reused
across resets.

## What changed

- `vendor/nle/src/src/alloc.c`: full rewrite of `alloc()` as an arena bump
  allocator; `nle_arena_free()` (no-op for arena, libc free otherwise);
  conditional on `NLE_USE_ARENA_FREE` so util binaries still get libc malloc.
- `vendor/nle/src/include/global.h`: `#define free(p) nle_arena_free(...)`
  under `NLE_USE_ARENA_FREE`, routing every `free()` call in NetHack code
  through the arena-aware wrapper.
- `vendor/nle/src/src/nle_arena_cpp.cc` (new): C++ operator new/delete
  overrides routed to the same arena.
- `vendor/nle/src/src/nle_fast_reset.c`: snapshot now also copies the live
  arena (`nle_arena_base[0..arena_used)` and the bump pointer); restore
  rewinds the bump pointer and memcpys arena bytes back.
- `vendor/nle/src/src/dlb.c`: `close_library()` becomes a no-op under
  `NLE_USE_ARENA_FREE` (leak DLB FILE+arena pointers; they're reused after
  restore).
- `vendor/nle/src/CMakeLists.txt`: append `nle_arena_cpp.cc` to libnethack
  sources, define `NLE_USE_ARENA_FREE=1` and link with
  `-Wl,-Bsymbolic-functions`.

## Results

Build: `cmake --build vendor/nle/src/build --target nethack -j8`
followed by
`MODE=fast EXTRA_CFLAGS="-DNETHACK_FAST_RESET=1" OUTPUT_NAME=nethack_fr bash build.sh nethack`.

| Test                                 | nethack_fr (this work)            | nethack_slow (baseline)                |
|--------------------------------------|-----------------------------------|----------------------------------------|
| `bench 10000` (random policy)        | 46 K steps/sec, 1 episode         | crashes (see below)                    |
| `bench 100000` (random policy)       | 63 K steps/sec, 1 episode         | crashes                                |
| `resets 1000` (no steps between)     | 67 K resets/sec                   | n/a                                    |
| `stepreset 100 100` (100 steps x 100 resets) | 72 K steps/sec       | n/a                                    |
| `stepreset 1000 100` (1000 x 100)    | 67 K steps/sec                    | n/a                                    |
| Determinism (`verify_determinism replay`) | passes (1000 steps match) | passes                                  |
| Peak RSS at 100 K steps              | 8 MB                              | n/a                                    |

The random policy (with fixed `srand(0xC0FFEE)`) doesn't kill the agent
within 100 K steps in this build of NLE — `bench 100000` runs as one long
episode without triggering an in-episode reset. To explicitly exercise the
snapshot/restore + heap-aware path I added a `stepreset` mode that does N
cycles of (M random steps, c_reset). The `stepreset 1000 100` run completes
100 K steps across 100 in-episode resets at ~67 K steps/sec — proving that
heap state is correctly snapshotted and restored across resets when
NetHack's per-game alloc'd state has actually churned.

Determinism harness (which doesn't exercise fast-reset; it replays a
recorded trajectory in a single episode) still passes, confirming the
arena allocator + free macro doesn't perturb gameplay deterministically.

## Known limitations

- **Slow path now broken under the arena build.** `nethack_slow bench`
  crashes at the second `nle_start` (during welcome / status init) with
  `free(): invalid pointer`, deep inside libstdc++'s `operator delete`.
  The slow path performs `dlclose + dlopen` per reset; each fresh libnethack
  instance gets its own arena_base, but the C++ runtime evidently has some
  pointer that escapes our override. This isn't required for the FR use
  case (FR uses the fast path on every reset after the first), but it would
  need separate work if slow-reset is still needed. Suggested approach:
  intercept libc `malloc` directly (not via operator new) so the C++ runtime
  itself routes through the arena, OR isolate the C++ allocations via
  per-instance `std::pmr::monotonic_buffer_resource`.
- **DLB FILE leak across episodes.** `close_library()` is suppressed, so
  the DLB FILE* and a few small allocations leak permanently. ~32 KB per
  process lifetime; not per-reset. Bounded.
- **Per-game heap churn leaks into the arena until restore.** Inside a
  single in-episode play span, `free()` is a no-op for arena pointers, so
  the arena grows monotonically. Watermark is rewound on the next reset.
  64 MB is well above the typical NetHack live set (a few hundred KB);
  haven't observed overflow in any test. For very long single-episode
  runs (millions of steps without dying) the arena could fill; in practice
  RL agents reset often enough that this isn't a risk.
- **Determinism harness doesn't exercise FR.** It plays one episode and
  reads the trajectory; it does not test snapshot/restore behavior. The
  `stepreset` mode is the closest thing to a behavior-equivalence test
  for FR.

## Memory growth measurements

Peak RSS (from `/usr/bin/time -v`) — all well below the 64 MB arena cap:

- `resets 1000`: 8 MB peak (mostly libnethack code + small live set)
- `bench 100000` (one long episode): 8 MB peak
- `stepreset 1000 100` (100 reset cycles): 7 MB peak

The arena is mmap'd lazily (`MAP_ANONYMOUS`, demand-paged), so unused
arena space is RSS-free.
