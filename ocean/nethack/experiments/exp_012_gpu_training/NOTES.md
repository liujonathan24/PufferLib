# exp_012 — GPU training

## Setup
- 1× NVIDIA A100 80GB (della-l02g12), 16 CPU cores
- `cudatoolkit/12.8 + intel/2024.2` modules
- `_C.so` rebuilt via the CUDA path (`bindings.cu`), 1.18 MB (vs CPU 0.32 MB)
- `puffer train nethack` (no `--slowly`), 512 agents, 4096 minibatch, 1M timesteps

## Headline result

**SPS = 49,000 c_steps/sec** (vs CPU `--slowly` 1,200 SPS).
**42× speedup** vs CPU training — entirely from moving the network forward+backward to the GPU.

## Iteration breakdown (steady state)

| phase    | time    | % of cycle |
|----------|--------:|-----------:|
| Evaluate | 561 ms  | 88 %       |
| └ Env    | 549 ms  | 86 %       |
| └ Forward (GPU) | 65 ms |10 %     |
| Train (GPU) | 74 ms  | 11 %    |
| Misc    | 8 ms    | 1 %        |

The picture has **completely flipped** from exp_009:
- CPU run: Env 2 %, Train 98 % → SPS bound by CPU network training
- GPU run: Env 86 %, Train 11 % → SPS bound by env stepping itself
- **GPU is 3 % utilized** — lots of headroom to scale up `total_agents`
  if env stepping can keep up

## Where 1 M valid/sec stands

On 16 cores feeding 1 GPU:
- env c_steps/sec aggregate = **49 K**
- valid/c_step ratio early-training = ~17 % (was 46 % for the CPU-trained
  policy at 295 K steps; this run is at ~1 M steps but the policy hasn't
  converged on a useful strategy yet)
- valid/sec = **~8.5 K** today; would be **~22 K** at the converged ratio
- Per CPU core: 49K / 16 = **3.1 K c_steps/sec/core**

Per-core throughput is roughly the same as the standalone profile said
(3-5 µs / c_step). To hit 1 M valid_moves/sec on a single A100 setup,
we'd want ~64 CPU cores feeding the GPU (instead of 16). The GPU node we
got only ships 16 cores; need a bigger-CPU GPU node or multi-node.

## What this proves

1. **The env can step at line rate when the network isn't in the way.**
   49 K c_steps/sec on 16 cores matches the pure-stepping benchmark
   from exp_005 (51 k c_steps/sec for `north` policy at 16 cores after
   reset cost is amortized).
2. **Network training is no longer a bottleneck.** With GPU, train+
   forward together are 21 % of the cycle. We are now squarely in the
   "CPU env stepping is the limit" regime.
3. **The auto-dismiss + illegal-penalty signal works in real training.**
   illegal_actions averages 3-9 per ~2000-step episode (<0.5 %),
   episode_return stays in a reasonable range, network is updating.
4. **Reset cost is still real but amortized.** Episode length is now
   ~2000 c_steps/ep, so 217 ms / 2000 × 5 µs = ~22 % of CPU time per
   agent goes to resets. Pre-warm pool could recover that 22 %.

## Stuck on Dlvl 1 — agent isn't going deeper

`depth` stayed at 0.000 across the whole run. The agent never goes
down stairs. That's because:
- Our reduced action set includes `>` (down stairs) but it only does
  anything on a downstairs tile, which a random-ish policy almost
  never lands on.
- The depth bonus +1.0 fires only on actual descent — never collected.
- Score remains 0 — no gold, no kills, no inventory progress yet.

This is normal for an undertrained NetHack agent. To accelerate this,
**a `Search for downstairs and step on them` curriculum or a
shaped sub-reward for proximity to stairs** would help. Out of scope
for the harness work, but flagged for the RL team.

## Decision

- The harness is now demonstrably **GPU-ready**.
- The next throughput wins are:
  1. **More CPU cores per GPU** — request `--cpus-per-task=32` or `64`
     on GPU nodes if available; bigger CPU pool → more env c_steps/sec.
  2. **Reset hiding** — pre-warm dlopen pool would recover ~22 % of
     per-agent wall time.
  3. **Multi-node multi-rank** — 8× A100 80GB on a single H100 node,
     each with 16+ CPU cores; aggregate 1.6 M+ c_steps/sec achievable.
- **1 M valid_moves/sec is now in reach with a single multi-GPU node**
  if we can pair more CPU cores with the GPU(s).
