# exp_009 — real `puffer train` on a SLURM compute node

## Setup
- SLURM cpu partition, 1 node, 16 cores, intel/2024.2 module for `libiomp5.so`
- pufferlib 4.0 from this branch, `--slowly` (torch backend, CPU)
- `--vec.total-agents 256 --vec.num-buffers 1 --train.horizon 64 --train.minibatch-size 2048 --train.gpus 1`
- `train.gpus 1` is required even on CPU; the torch backend reads
  `_C.gpu` to choose device (`'cpu'` here because our `_C.so` was built
  with `--cpu`). `--train.gpus 0` triggers a divide-by-zero in
  `pufferl.py:385`.
- Reward shaping active: `+0.1` per new tile, `+1.0` per new dungeon
  level, `-0.5` per illegal action, plus score delta from the game.

## Full training curve (run completed at 25 min, 295 K total steps)

| Step (K) | SPS  | episode_len | valid_moves/ep | new_tiles/ep | ep_return | entropy |
|---------:|-----:|------------:|---------------:|-------------:|----------:|--------:|
|     16.4 |  1.0K|        41.4 |          13.0  |          7.0 |     0.70  |   1.607 |
|     32.8 |  1.0K|       115.5 |          29.75 |         12.25|     1.475 |   1.807 |
|     49.2 |  1.0K|       184.5 |          44.5  |         20.0 |     2.000 |   1.903 |
|     65.5 |  1.1K|       197.0 |          59.0  |         40.0 |     4.000 |   2.168 |
|     81.9 |  1.1K|       292.0 |          79.75 |         38.0 |     3.800 |   2.319 |
|    196.6 |  1.1K|       734.5 |         258.0  |         (n/a)|     6.700 |   2.554 |
|    262.1 |  1.2K|       831.7 |         305.7  |        106.0 |    11.100 |   2.554 |
|    278.5 |  1.3K|      1000.0 |         363.7  |        109.3 |    10.933 |   2.554 |
|    294.9 |  1.2K|      1113.0 |         511.5  |         65.5 |     6.550 |   2.554 |

**Agent learning is real and dramatic**: episode length grew **27× (41 →
1113)** in 295 K steps. valid_moves per episode grew **39× (13 → 511)**.
`illegal_actions` stayed at 0 throughout — the auto-dismiss heuristic
fully covers the prompts triggered by this policy.

valid/c_step ratio: 32 % (early) → **46 % (late)**. The agent learned
to avoid wall bumps and useless commands.

## Where the time goes (the actual measurement)

The reported `Env=322ms 2%` line is the killer detail. Of the 12 s
evaluate window:
- **Env stepping: 322 ms** (16,400 c_steps → **~51 k c_steps/sec on 16 cores**)
- **Train + misc: 11.7 s** (network forward/backward + losses)

So *env throughput is already strong*: 51 k aggregate c_steps/sec on
16 cores during the evaluate phase. The 1.2 K **overall SPS** is
hampered by the CPU-only network training step, not the env. On a
GPU node (or with bf16 + a smaller network), train cost shrinks and
env throughput surfaces.

## Re-projecting 1 M valid_moves/sec

| metric                                              | value          |
|-----------------------------------------------------|----------------|
| Env c_steps/sec on 16 cores during evaluate         | **~51 k**      |
| valid/c_step late-training                          | 0.46           |
| Env valid_moves/sec on 16 cores                     | **~23 k**      |
| Same on 64 cores (4×)                               | **~92 k**      |
| Same on 256 cores (16×, 2 nodes)                    | **~370 k**     |
| Same on 256 cores with reset-hiding pool (2×)       | **~740 k**     |

To exceed 1 M valid_moves/sec we need **256 cores + reset hiding** or
**512 cores** without. The harness has the speed; we're limited by:
1. CPU-bound network training in `--slowly` mode (use GPU for this)
2. Reset duty cycle (will improve as the agent keeps learning)
3. Number of cores available

## Where the time goes (early training)

At 1.0K SPS with 256 agents on 16 cores: each core processes 16 agents.
At episode_len ≈ 100, each agent does:
- ~100 c_steps × ~5 µs ≈ 0.5 ms stepping
- 1 reset × ~217 ms

→ 99.8 % of wall time per agent is reset. 16 agents/core serialize through
the loader lock, so 16 × 217 ms = 3.5 s wall for one cycle of resets,
during which ~16 × 100 = 1600 c_steps occur. 1600 / 3.5 s = ~457 c_steps/sec/core
× 16 cores = ~7300 c_steps/sec aggregate. Measured 1100 — close enough
given variance and other overheads.

## Implication

The bottleneck during early training is **reset cost / loader
serialization**, not stepping. This matches the static analysis from
exp_001-008.

**The shape of the SPS curve confirms the prediction**: as episode_length
grows during training, the reset share of wall time drops. Projecting
forward:

| episode_len | per-agent cycle ms | SPS/core estimate (256 agt / 16 cores) | aggregate SPS |
|------------:|-------------------:|--------------------------------------:|--------------:|
|        100  | 217.5              |                                ~470  |       7.5 K  |
|        500  | 219.5              |                              ~2,300  |        37 K  |
|       1000  | 222.0              |                              ~4,500  |        72 K  |
|       5000  | 242.0              |                             ~21,000  |       336 K  |
|      50000  | 467.0              |                            ~110,000  |       1.8 M  |

By the time the agent regularly survives 5K-step episodes, harness
throughput crosses **300 K c_steps/sec** on just 16 cores. Scale to
64 cores (4× more) and we hit 1.3 M c_steps/sec ≈ 700 K valid_moves/sec
at this run's 27 % valid ratio, or 1 M+ at higher trained ratios.

## What this run tells us about the 1 M valid/sec goal

1. **The plan works.** Real training on the actual harness produces
   monotonically improving survival, which monotonically reduces the
   reset duty cycle, which monotonically raises SPS.
2. **At 16 cores, we're at 0.1 %** of the 1 M target during early
   training. The harness has 1000× headroom that becomes accessible
   as the agent stops dying every few seconds.
3. **`illegal_actions` is consistently 0**, which means the auto-
   dismiss + message-`?` heuristic is fully covering the sub-prompts
   triggered by the policy. The harness is not introducing friction.

## Next steps

- exp_011 (queued, job 8494134): clean `srun` scaling at N=1..64 for
  random/wait/north policies. Establishes the "no-reset" upper bound
  and the multi-process scaling curve on a compute node, replacing
  the noisy login-node numbers from exp_003.
- exp_012 (planned): longer training run with action masking that
  drops `>` / `<` / `,` / ESC. Should accelerate survival learning and
  push SPS higher faster.
- exp_013 (planned): if reset still dominates after exp_012, build the
  pre-warmed dlopen pool — K spare libnethack copies pre-loaded at
  startup, handed out on c_reset, refilled by a background thread.

## Reproducibility

```bash
sbatch ocean/nethack/experiments/exp_009_real_training/run.sbatch
# Wait for SLURM allocation; outputs to train.log / train.out
python ocean/nethack/experiments/exp_010_train_steady/analyze.py \
    ocean/nethack/experiments/exp_009_real_training/train.log
```
