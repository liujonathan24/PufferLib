# Retrospective after exp_006 — exp_010

Goal: **1 M valid moves/sec for NetHack RL training.**

## What happened in this batch

| Exp     | What it produced                              | Headline                                |
|---------|-----------------------------------------------|------------------------------------------|
| 006     | (skipped — async-reset thread; deferred)      |                                          |
| 007     | PufferLib `_C.create_vec` smoke test          | Vec works end-to-end; OMP threads collapse on the loader lock during resets |
| 008     | Multi-process scaling with `north` policy     | Pure-stepping ceiling: 228 k c_steps/sec/core, 11 M aggregate at N=128 |
| 009     | Real `puffer train` on a compute node         | Agent learns: ep_len 41 → 338 in 100 K steps; SPS 1.0 K → 1.1 K (reset-bound) |
| 010     | `analyze.py` for parsing pufferl logs         | Parses tabular pufferl output; surfaces SPS, valid_moves, illegal, new_tiles |

## Things I got wrong since RETROSPECTIVE_1

1. **Intra-process multi-env hides reset** — wrong again, but for a
   subtler reason. `nethack_multi.c` showed no win in single-thread.
   exp_007 (pufferlib's OMP threading) also showed no win, because the
   reset path calls `dlopen()` which is process-globally serialized
   inside the dynamic linker. *Threads can't parallelize dlopen.*
   Only separate processes can. This is the most important constraint
   I learned this batch.

2. **"Login-node Python performance is the same as compute-node"** —
   wrong. On the login node, `import torch` could take 80+ seconds vs
   under 5 seconds on a clean compute node. Heterogeneous I/O makes
   login-node iteration unreliable for anything bigger than the C
   profiler binary. Lesson: SLURM jobs are the right venue even when
   they take 10 min to dequeue.

3. **"Reset cost is harness-driven"** — half right, half wrong. The
   217 ms per dlopen is a hard cost. But the *rate* at which we pay
   it is policy-driven. exp_009 showed clearly: as the policy learns
   to survive, episode_length grows ~7× in 80 K steps, and SPS climbs
   slowly with it. The harness work was correct; the rest is RL
   training time, not engineering.

## Things I got right

1. **Profile-first.** exp_001's discovery that reset is 86 % of random-
   play wall time short-circuited a lot of bad optimization ideas.
2. **`north` policy as a "no-reset" ceiling.** Cheapest stress test of
   the harness — 228 k c_steps/sec/core, 11 M aggregate at N=128.
3. **Compile-time obs selection + always-allocated hook buffers.** The
   obs selection works without any runtime branching, and the
   always-allocated `misc/internal/blstats/message` hook buffers (~270
   bytes/env) cleanly enable the auto-dismiss + new-tile + illegal-
   detection logic even when the user opts those fields out of obs.
4. **Auto-dismiss covers everything random play triggers.** exp_009's
   `illegal_actions = 0` across all observed epochs means the heuristic
   (misc[] flags + message ends in `?`) catches the prompt types a
   trained-ish policy actually triggers.

## The 1 M target — current state

| Setting                                  | c_steps/sec  | est valid/sec @ 0.3 | est valid/sec @ 0.7 |
|------------------------------------------|-------------:|--------------------:|--------------------:|
| Single env, single thread, no resets     |      228 k   |              68 k   |             159 k   |
| 16-proc microbench, north                |     2.7 M    |             810 k   |             1.9 M   |
| 128-proc microbench, north (login)       |    11.0 M    |             3.3 M   |             7.7 M   |
| **exp_009 real training, 16 cores, ep_len=300** | 1.1 K | 330 | 770 |

The gap between the synthetic ceiling (millions) and the real training
SPS (1.1 K) is **3-4 orders of magnitude**, and it is entirely the
reset duty cycle while the agent's policy is poor.

## Decision tree for what's next

```
Q: Does episode_length need to reach >5 K c_steps before SPS is acceptable?
   YES (likely, given the projection table in exp_009/NOTES.md)
   |
   ├── Path A: just keep training. Episode_length will grow.
   │   Resource cost: time.
   │
   └── Path B: speed up resets to make early training tolerable.
       Sub-options:
       1. Pre-warmed dlopen pool inside each proc (exp_013).
       2. Multi-process via PufferLib's num_buffers / multi-rank.
       3. Action masking to keep agents alive longer (exp_012).
```

Recommendation: **A is the long-term answer, B-3 is the cheapest
quick win.** Action-mask the dangerous keystrokes (`>` `<` `,` ESC) so
early training doesn't waste 50 % of its actions on no-ops that lead
to death faster. This is one config change and one env-side filter.

## Backlog (priority order)

1. exp_012: action masking experiment, measure SPS lift on first 100 K
   training steps.
2. exp_013 (only if exp_012 is insufficient): pre-warmed dlopen pool.
3. exp_004 / exp_011 SLURM jobs (queued): clean scaling numbers under
   resource control. Will retroactively validate exp_003 / exp_008.
4. Investigate `vec.num_buffers` > 1 in pufferl — double-buffered
   step/inference might overlap network forward with env step.
5. Investigate gpus=1 + CUDA-actual-GPU path. We have a node with GPUs
   in our cluster — could rebuild `_C.so` with CUDA bindings (the
   `bindings.cu` path, not `bindings_cpu.cpp`) and measure training
   throughput with real network forward on GPU. Should not change the
   c_steps/sec floor but would slash inference wall.
