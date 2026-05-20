# exp_023 — dlopen-per-env catastrophic cache pressure

## Setup

Login node (no parallelism — single-threaded sequential round-robin over
N envs, calling c_step on each in turn). Each env owns a private
`dlopen`-loaded copy of libnethack.so (~4 MB code + ~290 KB writable
data, per env).

Workload: random policy, 5000 c_steps per env, total = N × 5000 steps.

```
./nethack_multi N 5000 /dev/null
```

## Results

| N envs | aggregate SPS | per-env SPS | factor vs N=1 |
|-------:|--------------:|------------:|--------------:|
|      1 |        44,059 |      44,059 |          1.00 |
|      4 |        28,888 |       7,222 |          0.16 |
|      8 |        26,102 |       3,263 |          0.07 |
|     16 |        22,543 |       1,409 |          0.03 |

**Aggregate throughput DECREASES** as N rises beyond 4. **Per-env
throughput drops 31× from N=1 to N=16.**

## Diagnosis

Each env owns its own dlopen copy of libnethack:
- ~4 MB code segment per env
- ~290 KB writable data per env
- Heap allocations also separate

L1i cache: ~32 KB per core. L1d: 32 KB. L2: 1 MB. L3: ~30 MB shared.

At N=16, the working set is 16 × ~5 MB = ~80 MB — well past L3 (30 MB)
and orders of magnitude past L1/L2. Each sequential round-robin step
switches the active env's code+data, so the icache and dcache get
purged every step. The CPU spends its time on memory bandwidth fetching
NetHack's logic from DRAM instead of executing it.

This is **not** loader-lock contention (we're single-threaded).
This is **not** Python/PufferLib overhead (we're running a pure C binary).
It's pure cache pressure from dlopen-per-instance.

## Implications

### For single-process scaling
This is the **dominant** scaling bottleneck for multi-env-in-one-
process workloads. Even with the perfect fast-reset, aggregate SPS at
N>4 is below N=1. Whatever throughput we get from fast-reset will be
swamped by cache misses if we don't address dlopen.

### For multi-process scaling
Each process has its own ~5 MB working set; if processes run on
different cores with different L1/L2 cache state, no contention. exp_003
already confirmed multi-process scales well (22× at N=32). The catch:
N processes need N × 5 MB resident memory, plus a per-process Python
interpreter (~40 MB), plus PufferLib state. At N=256 on a node: 256 ×
~50 MB ≈ 12 GB just for libnethack copies. On a 256 GB node this fits.

### For the functional refactor
**One** shared libnethack mapping for N envs eliminates this entirely:
- Code segment shared across all envs (single icache footprint)
- Per-env state is just the heap-allocated `nle_state` struct (~100 KB
  per env from the audit's estimate)
- Total RSS at N=16 = ~5 MB code + 16 × 100 KB state ≈ 7 MB
  (vs the current 80 MB)

So a functional refactor isn't just about "elegance" — it's the only
way to scale single-process throughput.

## Caveat

This benchmark is SEQUENTIAL round-robin, not parallel. In a parallel
(OMP-threaded) bench, each thread is dedicated to one env, so the
working set per thread is just one libnethack copy. exp_012's GPU run
showed 49K SPS on 16 threads = 3.1K per thread, which is closer to the
single-env-at-N=1 baseline of 44K — but still 14× degraded, suggesting
that *parallel* in-process is dominated by something else (probably
loader-lock contention on resets + memory-bandwidth contention across
threads). The full functional refactor addresses both.

## Verification commands

```
# Build the standalone multi-env bench
MODE=fast OUTPUT_NAME=nethack_multi EXTRA_SRC="ocean/nethack/multi.c" \
  bash build.sh nethack

# Run with patched libnethack
NETHACK_LIBPATH=$PWD/vendor/nle/src/build/libnethack.so \
NETHACKDIR=$PWD/vendor/nle/nethackdir \
  ./nethack_multi 16 5000 /dev/null
```

## Conclusion

The functional refactor is **load-bearing** for the user's "fast env"
goal — not just nice-to-have. Heap-aware fast-reset (exp_019, in
flight) recovers reset latency but does not address the dlopen-per-env
cache pressure that crushes throughput at N>4 in one process.

To reach 1M SPS without the full refactor, the path is:
- N processes × ~50K SPS each = need ~20 processes
- Requires multi-node multi-GPU (20 CPUs/proc × 20 = 400 cores)

With the functional refactor:
- 1 process × N envs sharing libnethack
- ~40K SPS × 64 envs/process (cache-fits in L3) = ~2.5M SPS aggregate
- Single 64-core node suffices

Both paths get to the user's goal. The refactor path uses less hardware.
