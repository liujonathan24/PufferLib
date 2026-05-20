# exp_008 — multi-process scaling with `north` policy

Goal: measure the harness's pure-stepping ceiling, free of reset noise.
`north` policy hits a wall on turn 1 then sends `k` forever — NetHack
returns "It's a wall." every step, never advances game time, never dies.
After the single initial reset, no further resets happen. So the
benchmark measures only `agent_fn_step` + obs pack + bookkeeping.

Also caught a bug en route: `init()` was redundantly calling
`nethack_load_lib`, then `c_reset` also called it. Wasted ~180 ms /
env at startup. Removed the init-side call. Single-thread throughput
went from 68 k → 113 k c_steps/sec (50 k step run), 172 k → 228 k
(200 k step run — fixed cost amortizes better).

## Scaling (200 000 c_steps per proc, north policy)

| N   | wall (s) | aggregate c_steps/s | scaling | efficiency |
|-----|---------:|--------------------:|--------:|-----------:|
| 1   | 0.88     |  **227,611**        | 1.00×   | 100 %      |
| 2   | 0.98     |    409,109          | 1.80×   |  90 %      |
| 4   | 0.90     |    891,424          | 3.92×   |  98 %      |
| 8   | 0.92     |  1,742,487          | 7.66×   |  96 %      |
| 16  | 0.96     |  3,321,904          | 14.59×  |  91 %      |
| 32  | 1.25     |  5,124,091          | 22.51×  |  70 %      |
| 64  | 1.95     |  6,553,338          | 28.79×  |  45 %      |
| 128 | 2.31     | **11,093,710**      | 48.74×  |  38 %      |

## Key numbers

- **Per-core ceiling: ~285 k c_steps/sec** (200 k steps in 0.88 s minus
  the 0.18 s one-time reset = 0.70 s of pure stepping).
- **Aggregate ceiling (128 cores, login node, contended): 11 M c_steps/sec.**
- Linear efficiency up to N = 16; degrades past that on the login node
  (shared with other users). Need the `srun` numbers (exp_004) to
  separate "contention" from "actual scaling limit".

## Throughput projection vs the 1 M valid/sec goal

At valid/c_step = 0.5 (a reasonable mid-training rate):
- N = 32: 5.1 M c_steps × 0.5 = **2.55 M valid/sec** — past 1 M
- N = 64: 6.5 M × 0.5 = **3.28 M**
- N = 128: 11 M × 0.5 = **5.55 M**

At valid/c_step = 0.2 (very early training, lots of bad moves):
- N = 16: 3.3 M × 0.2 = **665 k** — under 1 M, but already very useful
- N = 32: 5.1 M × 0.2 = **1.02 M** — at target

**Conclusion: 1 M valid/sec is achievable from ~32 cores once the policy
isn't suicidal.** The 128-core machine has 4-10× headroom on top.

## What we still need

1. The above is "no resets after the first" — `north` is an artificial
   policy. For a *trained* policy doing real moves, episodes are finite
   (say 10-100 k c_steps before death). Reset cost amortizes well at
   those lengths but isn't free. Run a synthetic test that mixes
   stepping with a reset every N steps and measure the curve.
2. `agent_fn_step` cost varies by action type. north (wall bumps) is
   fast (~3.2 µs/fn_step). Random play hits more varied code paths
   (~18-20 µs/fn_step). A trained policy doing real movement will be
   somewhere in between. So 285 k c_steps/sec/core is the *upper*
   bound; expect 80-150 k/sec in real training.
3. The 38 % efficiency at N=128 on the login node is suspicious. The
   pending SLURM job will give cleaner numbers.

## Decision

The harness has more than enough headroom. The remaining work is
**integration**, not optimization:
- exp_007: PufferLib vecenv smoke test (does our `binding.c` actually
  work end-to-end through Python?)
- exp_009: real training-style benchmark — feed pufferlib's vecenv a
  policy that does N steps then occasionally a "reset trigger" action,
  measure the end-to-end pipeline rate.
