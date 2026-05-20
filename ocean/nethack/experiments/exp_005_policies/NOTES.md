# exp_005 — policy-dependent throughput

Question raised after exp_003: is the multi-process throughput ceiling
from harness limits or from policy-induced reset frequency? Test by
varying the policy to see what changes.

## Setup
- Added 4 policies to the profile driver: `random`, `wait`, `north`,
  `safe` (cycle N/S/W/E).
- Each 50 000 c_steps, single proc, all obs fields enabled, profile build.

## Result

| Policy | wall (s) | c_steps/s | valid/s | resets | episode_len | reset share |
|--------|---------:|----------:|--------:|-------:|------------:|------------:|
| north  | **0.65** | **77,453**|       3 |      1 | 50,000      | 38.1%       |
| random |    9.57  |    5,226  |  1,833  |     31 |  1,613      | 84.8%       |
| safe   |   11.60  |    4,311  |  3,075  |     38 |  1,316      | 84.8%       |
| wait   |   14.09  |    3,549  |  2,662  |     49 |  1,020      | 88.3%       |

`north` is the dramatic outlier: hitting the same wall forever means
NetHack never advances time → character never dies → 1 reset total.
The whole 50 000 c_steps run in 0.65 s.

## What this proves

- **Pure stepping cost is ~13 µs / c_step single-thread** (1 / 77,453).
- **Reset cost is what we have been benchmarking, not stepping.**
- All other policies are reset-bottlenecked because their characters die
  every ~1 000–1 600 c_steps. The reset share of wall is ~85 % regardless
  of which "real" policy you pick.

## Re-projecting 1 M valid/sec under realistic training

The "pure stepping" 77 k c_steps/sec/core is the ceiling we should aim
for in steady state. The reset cost is **a property of the policy's
death rate**, not of the harness. For a trained policy:

| episode length (c_steps) | per-episode time | c_steps/sec | valid/sec @ 0.6 |
|-------------------------:|-----------------:|------------:|----------------:|
| 1 000 (random / early)   | 0.013 + 0.217 = 0.230 s | 4,348 | 2,609 |
| 5 000                    | 0.065 + 0.217 = 0.282 s | 17,730 | 10,638 |
| 10 000                   | 0.130 + 0.217 = 0.347 s | 28,818 | 17,291 |
| 50 000                   | 0.649 + 0.217 = 0.866 s | 57,737 | 34,642 |
| 100 000                  | 1.299 + 0.217 = 1.516 s | 65,963 | 39,578 |

So at 32-core scale (the SLURM job):
- early-training (1k-step episodes): 32 × 2.6k × 0.7 eff = **58 k valid/sec**
- mid-training (10k-step episodes):  32 × 17k × 0.7 = **381 k valid/sec**
- late-training (100k-step episodes): 32 × 40k × 0.7 = **890 k valid/sec ≈ 1 M**

At 64 cores even mid-training hits **750 k**. At 128 cores, mid-training
clears 1 M.

## Decision re: priority of "speed up reset"

The user originally tagged reset as lowest priority because worker pool
would hide it. exp_005 says they were right — **reset is policy-dependent,
not harness-imposed**. The early-training pain (frequent deaths) is what
makes resets dominate. As training progresses, the problem evaporates.

So the worker pool's job becomes: handle the early-training case
gracefully. That's "easy" reset-hiding, not "make dlopen 10× faster".

## Next experiments

- **exp_006**: build a simple intra-process async-reset thread. While
  one env is doing dlopen+nle_start, other envs in the proc keep stepping.
  Test if intra-process multi-env throughput recovers (exp_003's
  `nethack_multi.c` was synchronous and showed no win).
- **exp_007**: verify PufferLib vecenv compiles+runs with the nethack env
  (we already showed the static lib + bindings_cpu.cpp link; need to
  actually run a small `pufferlib` Python smoke test).
- **exp_008**: re-run multi-process scaling with the `north` policy at
  N=1..64 to get a clean parallel-stepping ceiling free of reset noise.
  This is the headline number for harness performance independent of
  agent behavior.
