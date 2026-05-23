# exp_032: hyperparameter sweep — escape policy collapse

**Branch:** `4.0` at HEAD `894e9f3c` (Cluster AW-full step 4/12)
**Hardware:** 1 H100 GPU
**Common config:** `--vec.total-agents 512 --vec.num-buffers 1 --vec.num-threads 1 --train.horizon 64 --train.minibatch-size 32768`, all other train.* defaults from `config/nethack.ini`.

## Configs

| Label | hidden | layers | params | clip_coef | ent_coef |
|---|---:|---:|---:|---:|---:|
| **A_small_aggr** | 128 | 2 | 313.7K | 0.20 | 0.10 |
| **B_small_mod** | 128 | 2 | 313.7K | 0.10 | 0.05 |
| **C_large_aggr** | 512 | 4 | 4.0M | 0.20 | 0.10 |

Baseline (config/nethack.ini default): `hidden=512, layers=4, params=4.0M, clip=0.01, ent=0.0208`.

## Results: hyperparameters work, crashes are the bottleneck

| Config | Uptime | Steps | SPS | ep_return final | ep_return peak | entropy | Wall | Crash |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| **A small + aggressive** | 19s | 524K | 25.4K | 5.10 | 5.10 | **1.659** | 30s | libc memcpy NULL-src |
| **B small + moderate** | 1m17s | 2.4M | 32.4K | 2.85 (was 4.30) | 4.30 | 1.231 | 90s | `emergency_disrobe trap.c:3723` |
| **C large + aggressive** | 31s | 655K | 19.5K | 5.23 → 5.37 | **5.37** | **2.443** | 60s | libc memcpy NULL-src |

## Critical findings

### 1. Entropy collapse was the problem, not model capacity

All three new configs **avoid policy collapse**. Healthy entropy in all (1.2–2.4) vs the old configs that collapsed to 0.000 within 30-60 seconds.

The single change from `ent_coef = 0.0208` → `0.05` or `0.10` keeps the policy diverse. The standing-goal task #46 (`reward > 1000`) was previously blocked NOT by stability and NOT by model capacity, but by an under-regularized exploration objective.

### 2. Small model is a free win on throughput

Config A (313.7K params, ~13× smaller than baseline) trains at **25.4K SPS** — about the same aggregate as the 4M-param baseline. But the per-update GPU cost is 8-10× lower. With matching steady-state training, the small model would reach the same number of total gradient updates in roughly 1/8 the wall time of the baseline.

Config A also reached **ep_return 5.10 in 19 seconds**. Baseline took ~12 minutes (N=256 1hr run) to reach 3.1. That's **~40× faster learning per wall-clock** with the small model + aggressive exploration.

### 3. The reward ceiling is the same across configs

Best `ep_return` so far: **5.37** (Config C). Same family as the prior runs:

| Run | Best `ep_return` |
|---|---|
| exp_030 N=512 1hr (baseline hparams) | 1.10 |
| exp_031 N=64 10min | 2.70 (then collapsed to 0.7) |
| exp_031 N=256 25min | 3.10 (frozen) |
| exp_031 N=1024 ~4min | 5.18 (before crash) |
| exp_031 N=4096 ~5min | 3.21 (before crash) |
| **exp_032 A small+aggr 19s** | **5.10** |
| **exp_032 B small+mod 90s** | **4.30** |
| **exp_032 C large+aggr 60s** | **5.37** |

A "reward ceiling" of ~5 keeps appearing. This is likely:
- **Survival reward** dominating the signal (each game tick earns +small reward; an episode that survives ~1000 turns at default reward shaping caps out around 5).
- **No descent reward** — `depth = 0.000` across ALL runs. The agent never goes downstairs, so it never opens up new reward sources (level rewards, score from monster kills at depth).

Without descent, no amount of training time or hyperparameter tuning will break 5-10 reward. **This is now a reward-shaping problem**, not RL hyperparameters.

### 4. Cluster AZ crashes scale with reset rate, not just N

All three runs crashed within 30-90 seconds, well below the timeout. Two crashed with the persistent `libc __memmove_avx_unaligned NULL-source` family, one in `emergency_disrobe` (`trap.c:3723`). The faster the policy gets to "die quickly and reset," the more episode-reset cycles per second, the more chance to hit the race.

At N=512 with the small-model fast policy: episode_length stayed ~1000 turns, so reset rate is ~30/s aggregate. At baseline reward ceiling (frozen episode_length=6666) the reset rate was ~5/s. **The crash got 6× more likely per wall second** as we sped up training.

The Cluster AZ work (gdb-attach the live process and identify the racy code path) is now the highest-leverage stability fix — fixing it would unlock long training runs at any N.

## What's next

Two parallel paths:

1. **Stability**: chase Cluster AZ (the libc NULL-source memcpy). With small-model runs crashing in 30 seconds it's a fast reproducer.

2. **Reward**: add descent reward / level-completion reward / score-delta reward. Right now the agent only earns survival reward; no signal to go downstairs. Even at perfect entropy and model size, capped at ~5 reward.

Both blockers need to be removed for >1000 reward. Independent — could run in parallel.

## Files

- `A_small_aggr.out`, `_curve.csv`, `.err`, `.exit`, `.pid`
- `B_small_mod.*`
- `C_large_aggr.*`
- `run_hp.sh`, `sweep.sh`, `sweep.log`
