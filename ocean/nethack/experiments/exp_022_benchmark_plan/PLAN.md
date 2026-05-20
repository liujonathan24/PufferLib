# exp_022 — Benchmark plan for fast-env validation

Once a correct fast-reset lands (from exp_019 heap-aware path or the full
nle_state refactor), this is the protocol for measuring speedup at scale.

## A/B configurations to compare

| ID  | Reset path        | Lib build             | Notes |
|-----|-------------------|-----------------------|-------|
| BL  | dlopen+nle_start  | unpatched prebuilt    | The original baseline before any patches |
| S0  | dlopen+nle_start  | from-source unpatched | Confirms our build is behavior-equivalent to BL |
| S1  | dlopen+nle_start  | RNG refactor          | Confirms refactor doesn't slow stepping |
| F1  | snapshot+restore  | RNG + arena alloc     | The target win — fast resets at correctness |

## Workloads

For each config, measure on the same hardware:

### 1. Pure reset throughput (no stepping)
```
./nethack_<variant> resets 10000
```
Reports resets/sec. SLOW gives ~250; FAST should give >10K.

### 2. Single-thread stepping with episodes
```
./nethack_<variant> bench 100000
```
Random policy, 100K c_steps, prints SPS + final stats. With short
episodes, FAST should significantly outperform SLOW.

### 3. Multi-thread in-process (where the loader lock matters)
```
# Hack ocean/nethack/multi.c to run N OMP threads, each with its own
# Nethack env, each calling c_step in a tight loop.
./multi 100000 16     # 16 threads, 100K steps each
```
SLOW: bound by dlopen-loader-lock at reset — should saturate ~4 threads.
FAST: should scale near-linearly with thread count, up to memory-bandwidth
limit (~30 GB/sec / snapshot-bytes ≈ thousands of resets/sec/thread).

### 4. Real PufferLib training (the actual workload)
```
# Reuse exp_012 GPU run config, swap libnethack.so for the patched one
sbatch ocean/nethack/experiments/exp_022_benchmark_plan/gpu_train.sbatch
```
Same A100 80GB, 16 CPU cores, 512 agents, 4096 minibatch, GPU forward.
Measure aggregate SPS. exp_012 baseline was 49K SPS.

## Metrics to capture per config

- **c_steps/sec** (aggregate)
- **resets/sec** (extracted from profile counters)
- **mean c_reset wall time** (from profile histogram)
- **OMP-thread scaling** (1, 4, 16, 64 threads)
- **Per-iteration breakdown** (Evaluate / Env / Forward / Train) from puffer training output
- **RSS over 1M steps** (does the arena leak memory? Use `top -p $pid` snapshots)
- **valid_moves/sec** = SPS × valid/total ratio (this is the user's actual target)

## Hardware envelopes to test

- Login node (small, no GPU): single-thread sanity only
- Compute node `--cpus-per-task=16 --gres=gpu:1`: matches exp_012 (existing reference)
- Compute node `--cpus-per-task=32 --gres=gpu:1`: see if loader-lock fix unlocks scaling that exp_014 couldn't get
- Compute node `--cpus-per-task=64`: if 32 scales, push higher

## Sbatch template (for compute-node runs)

Reuse `ocean/nethack/experiments/exp_012_gpu_training/run.sbatch` — only
need to add `EXTRA_CFLAGS="-DNETHACK_FAST_RESET=1"` to the build step
and ensure `NETHACK_LIBPATH` points at the patched build.

## What "win" looks like

- Single-thread SPS: at least 2× over baseline (from reset cost reduction)
- 16-thread SPS: at least 4× over baseline (from loader-lock elimination)
- Real-training SPS on existing GPU rig: at least 2× over the exp_012
  49K SPS — call that win threshold
- Stretch: hit the 1 M valid_moves/sec goal on a 64-core multi-GPU node

## What "regression" looks like

If FAST is more than 10% slower than SLOW on any single-thread workload,
something is wrong in the new path. Bisect the snapshot/restore code.

If memory grows unboundedly (RSS continues climbing past 1 GB on a
single env's lifetime), the arena leak isn't bounded — investigate per-
game live set + arena reset semantics.
