# NetHack training throughput: regression, fix, and profile

_Analysis date: 2026-06-09. Hardware: NVIDIA A100-PCIE-40GB (full card, dedicated
SLURM node `della-i14g8`, 32 CPU cores). Model: DefaultEncoder → MinGRU(hidden=192,
2 layers) → DefaultDecoder, 535K params._

## TL;DR

- Training had collapsed to **~18.8K SPS** at N=4096. Root cause was a **config
  regression**: commit `0dd1d385` ("MIG-safe sweep defaults") set
  `minibatch_size = 256` (down from `32768`).
- With `minibatch_size = 256`, each PPO update is chopped into thousands of tiny
  GPU kernels that are launch-latency-bound, leaving the A100 ~idle between
  kernels. The fix (restore a large minibatch) recovers **~124–129K SPS — a 6.6×
  speedup** — with no change to the learning algorithm.
- It is **not** a login-node problem: a dedicated A100 produced the *same* 18.8K
  with the bad minibatch.
- The observation is **not** the bottleneck. It is already a tiny **92 bytes**
  (9×9 cropped chars + 11 compact blstats). Bit-packing it would not help.
- The remaining cost is the **PPO update compute** — GEMMs (the Linear layers) and
  the MinGRU parallel-scan elementwise kernels — and is GPU-bound (100% util).

## The regression

`config/nethack.ini`:

```
- minibatch_size = 32768   # historic default
+ minibatch_size = 256     # 0dd1d385 "MIG-safe sweep defaults"
```

`minibatch_size = 256` was chosen so sweeps on tiny MIG slices (1g.10gb) wouldn't
OOM, but it became the default for *every* run, including full-A100 training.

### Why a tiny minibatch is so slow

The PPO update does a fixed amount of compute: it passes the whole batch
(`total_agents × horizon`) through the network several times. `minibatch_size`
only controls how that work is *chunked* into GPU kernel launches.

- `minibatch_size = 256` → a 4096-env batch (1,048,576 samples) is split into
  ~4096 minibatches. Each launches a burst of small kernels whose runtime is
  dominated by launch latency, not math. The GPU stalls between launches.
- `minibatch_size = 32768` → ~32 minibatches of large kernels that actually
  saturate the SMs.

Observed effect (epoch wall-time at N=4096): **~53 s/epoch → ~6 s/epoch.**

## Measurements

### Minibatch sweep (N=4096, threads=32, buffers=1, cudagraphs=1)

| minibatch_size | SPS |
|---------------:|----:|
| 256 (regressed) | 18.8K |
| 32768 | 124.2K |
| **131072** | **129.2K** ← peak |
| 262144 | 100.1K |
| 1048576 (full batch) | 91.2K |

Throughput peaks near `minibatch ≈ 131072` and then *falls* — past the point where
kernels saturate the GPU, larger minibatches add memory traffic and reduce the
overlap of compute with the rollout phase.

### Scaling the number of envs (minibatch=262144, threads=32)

| total_agents | SPS |
|-------------:|----:|
| 4096 | 100.1K |
| 8192 | 96.2K |
| 16384 | 95.5K |

Adding envs does **not** raise throughput here — the env is not the bottleneck
(standalone it does ~9.2M steps/s), so more envs just add rollout work and larger
recurrent batches. N=4096 is the sweet spot.

### Built-in PufferLib profiler (per-epoch breakdown, minibatch=32768)

| Phase | Share |
|-------|------:|
| **Train** (PPO update: fwd + bwd + optimizer) | **~79%** |
| Evaluate / rollout | ~20% |
| ↳ env stepping | ~18% |
| ↳ policy inference (GPU) | ~1% |

GPU utilization during Train is ~100%, VRAM ~8.6 GB / 40 GB. The bottleneck is the
optimization step, not data collection or memory.

### Kernel-level profile of one train-step (torch.profiler, minibatch=131072)

A single `fwd + bwd + opt` step = **42 ms**. Top GPU self-time:

| Kernel / op | Share | What it is |
|-------------|------:|------------|
| `aten::mm` + `ampere_sgemm_*` | ~50% combined | the Linear layers (encoder, MinGRU 192→576 ×2, decoder) |
| `vectorized_elementwise` + `neg/mul/add/sub/where/flip` | ~40% combined | MinGRU Heinsen parallel-scan ops (softplus, logcumsumexp, exp) |
| `Command Buffer Full` | ~11% | kernel-launch / submission overhead |

So the train cost is GEMM + the MinGRU scan's elementwise passes over the
`(segments, horizon, hidden)` tensor. The encoder's `Linear(92→192)` over the
92-byte obs is negligible.

## Why this isn't the "~300K SPS" from memory

With the current architecture (hidden=192, 2-layer MinGRU, horizon=256) ~129K is
the realistic peak on one A100. A remembered ~300K almost certainly came from a
*different* regime — a smaller/cheaper policy (e.g. the `hidden=128` default), a
shorter horizon, or env-only throughput — not this exact train config. The
actionable result is the 6.6× recovery from fixing the minibatch.

## The fix (applied)

`config/nethack.ini`: `minibatch_size = 32768`. Chosen over the 131072 peak
because 32768 is the more robust divisor (divides the common N=1024/4096 batch
sizes and `% horizon == 0`) while still hitting ~96% of peak throughput. MIG-slice
sweeps that OOM should override downward with `--train.minibatch-size`.

## If we want to go past ~129K (all trade learning for speed)

The obs is already minimal, so the levers are all on the update-compute side:

1. **Fewer update passes** (`replay_ratio` / epochs) — linear SPS win, less sample
   reuse.
2. **Smaller recurrent core** — fewer MinGRU layers or smaller hidden shrinks both
   the GEMMs and the scan.
3. **Shorter `horizon`** — less scan work per sample, weaker long-range credit
   assignment.
4. **bf16 autocast** for the update — the GEMMs are the largest single cost and
   would roughly halve on tensor cores; needs loss-scaling care.

Bit-packing the observation is **not** on this list: at 92 bytes it is neither a
bandwidth nor a compute cost, and the encoder would have to unpack it before the
Linear anyway.
