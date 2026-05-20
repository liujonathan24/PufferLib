# Retrospective after exp_001 — exp_005

Goal: **1 M valid moves/sec for NetHack RL training.**

## What we did

| Exp     | What it measured                          | Headline finding                        |
|---------|-------------------------------------------|------------------------------------------|
| 001     | Baseline phase breakdown (random/wait)    | Reset = 86 % of wall                     |
| 002     | Classification of no-time-advance c_steps | 52 % are "wall bump" feedback, not hidden prompts |
| 003     | Multi-process scaling, N = 1..128         | 22 × at N = 32, 53 × at N = 128 (41 % eff) |
| 004     | (SLURM scaling job submitted, pending)    | TBD                                      |
| 005     | Per-policy throughput                     | Pure stepping = 77 k c_steps/sec/core    |

## Patterns observed

1. **The headline cost is not what we expected.** We started thinking
   "chattiness" — many fn_step calls per c_step — was the problem. The
   profiler immediately showed mean fn_steps/c_step = 1.03, i.e. almost
   exactly one. The actual cost is reset (dlopen + NetHack init), and
   reset frequency is governed by the agent dying.

2. **Random play makes the harness look slower than it is.** Random
   actions kill the character in ~1 600 c_steps, triggering a 217 ms
   reset. That's a brutal duty cycle: 0.013 s of game + 0.217 s of
   reset = reset dominates 17:1. A non-suicidal policy (`north`) hits
   77 k c_steps/sec single-thread — **15× more** than random. The
   harness was always capable of this; we just couldn't see it through
   the reset noise.

3. **Scaling is sub-linear past N=4** but not catastrophically so. From
   exp_003: 22× at 32 procs, 53× at 128. Login-node noise is the chief
   suspect; exp_004 under `srun` will tell us the true number. The
   `nethack_multi.c` test showed intra-process multi-env in one thread
   doesn't help — resets are CPU-bound, single-thread can't overlap them.

4. **Compile-time obs selection is free.** exp_001's `obs_pack` mean is
   85 ns regardless of which fields are enabled. The conditional `#if`s
   work as intended; NLE skips unbound fields via its `if (ptr) ...`
   guards. No measurable cost.

5. **The profiler itself adds ~5 % overhead.** Comparing exp_001
   profile-enabled vs the earlier non-profile run, c_steps/sec dropped
   from ~5800 to ~5500. Acceptable. For final benchmark numbers we will
   build without `-DNETHACK_PROFILE=1`.

## What I got wrong (and corrected)

- **"Chattiness is the problem"** — wrong. Mean fn_steps/c_step is 1.03,
  not "many". The auto-dismiss loop almost never runs.
- **"Reset is the lowest-priority optimization"** (per user's original
  prioritization) — half-right. It's low-priority *for steady-state
  trained agents*, because their deaths are infrequent. But it's the
  *gating* problem during early training and for any reset-frequent
  policy. Worker-pool hiding remains worth building.
- **"Single-process multi-env hides reset"** — wrong. In one thread,
  there's nothing else to interleave with during the reset's CPU work.

## What's still uncertain

- Real efficiency at N = 32, 64, 128 under controlled CPU allocation —
  exp_004 (SLURM job) will clarify.
- Whether multi-threaded reset overlap (one OS thread doing the dlopen
  while another thread continues stepping a different env in the same
  proc) actually works given NLE's globals and the dlopen-per-instance
  trick. exp_006 will test.
- Whether the PufferLib vecenv path (not just my microbench) actually
  scales the same way. exp_007 will verify.

## Decisions for the next 5 experiments

1. **exp_006**: intra-process async-reset thread (the missing piece that
   could let one process step env A while resetting env B). If it works,
   single-process worker-pool design is viable.
2. **exp_007**: PufferLib vecenv smoke test — does our `binding.c`
   produce a `_C.so` that pufferlib can import and step? Just need 1k
   c_steps of "yes it works" plus a throughput number.
3. **exp_008**: clean `north`-policy scaling under `srun` (pure stepping
   ceiling).
4. **exp_009**: full pufferlib training scaling at N procs with a real
   action mask (no `>` / `<` / `,`) to keep illegal_actions near zero.
5. **exp_010**: pre-warm pool of reset envs. Spawn K spare envs at
   startup, hand them out on done. Measure effective reset latency.

After exp_010, second retrospective.
