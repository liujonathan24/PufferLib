# exp_039: per-env arena, drop __sync_fetch_and_add (Cluster BE)

## Change

Replaced the process-wide 16 GB bump arena guarded by
`__sync_fetch_and_add(&nle_arena_used, need)` in
`vendor/nle/src/src/alloc.c` with a **per-env arena** living on `nle_ctx_t`
(`s_arena_base / s_arena_used / s_arena_cap`).

- Each env lazily mmaps a 64 MB arena on its first `alloc()` (with
  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, madvise DONTDUMP).
- The bump pointer is per-env and non-atomic — each env's coroutine is the
  sole writer of its own arena.
- Legacy process-wide arena is kept as a fallback for the rare allocation
  made before `current_nle_ctx` is anchored (early process init), and as
  the symbol nle_fast_reset.c still references (dead at runtime when
  `NETHACK_FAST_RESET=0`, which is the only safe config for N>=2).
- `nle_end` calls `munmap()` so virtual address space is returned to the
  kernel when envs are recycled.
- Global registry of live arena ranges (4096 slots, bounded high-water
  walk) lets `nle_arena_free` recognise pointers allocated in *any* env's
  arena. Required because process-global state (e.g. `sysopt.wizards`,
  populated by env A's `dupstr` → arena alloc) is freed during env B's
  teardown — without the registry, the cross-env pointer falls through
  to `__libc_free` and SIGSEGVs.

## Arena size

64 MB per env. With N=1024 that is 64 GB virtual; MAP_NORESERVE means
only touched pages are charged. Tests confirmed no OOM panic in alloc.

## SPS (N=1024 GPU, train.horizon=64, train.minibatch-size=65536)

Baseline (pre-change, `exp_036_sps_fix/n1024_cpu16.out`):
```
SPS    50.3K  47.9K  51.4K  50.5K  51.4K  50.8K  50.2K  50.5K  44.8K …
```
Peak ~51.4K, sustained 50–51K in first 30 s.

After change, run 1:
```
SPS    35.4K  53.6K  51.0K  52.0K  51.6K  51.8K  52.2K  51.1K  51.0K …
```
Peak 53.6K, sustained 51–52K.

After change, run 2:
```
SPS    36.7K  54.1K  51.2K  51.7K  51.3K  51.6K  51.5K  50.8K  50.8K …
```
Peak 54.1K, sustained 51–52K.

Roughly **+1 to +3 K SPS at peak**, sustained at parity or marginally
better. The __sync_fetch_and_add was a small but measurable cost; the
mmap-per-env overhead is paid once at startup.

## Process exit

EXIT=134 (SIGABRT) on both runs — the known steady-state short-read
abort in `restore.c`'s level read, *unrelated* to this change. No new
arena-related crashes (no OOM panic, no NULL deref on s_arena_base).
The first attempt before the registry was added did crash with
SIGSEGV in libc free from `sysopt_release`, which the global arena
registry fixes (sysopt strings allocated in env A's per-env arena are
now correctly no-op'd when env B's teardown calls free on them).

## Memory footprint

N=1024 × 64 MB = 64 GB virtual mapped, but with MAP_NORESERVE only
touched pages consume RSS. No observed OOM. Did not log
`s_arena_used / s_arena_cap` at teardown — left as future diagnostic.

## Smaller-N sanity

- N=4  GPU: 29 K SPS in 3 s, clean run to total-timesteps=100000.
- N=16 GPU: 43 K SPS in 30 s.
- N=64 GPU: peaked 62 K SPS, sustained 55-58 K.

All exit cleanly through the known shutdown path (EXIT=134 or
SIGSEGV in shutdown after total-timesteps reached — same as baseline).
