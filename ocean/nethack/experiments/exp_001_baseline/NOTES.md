# exp_001 — baseline profile

Date: 2026-05-20
Build: `EXTRA_CFLAGS="-DNETHACK_PROFILE=1" bash build.sh nethack --fast`
Commands:
- `./nethack profile experiments/exp_001_baseline/random.json 100000 random`
- `./nethack profile experiments/exp_001_baseline/wait.json   100000 wait`

## Headline numbers

|                          | random          | wait            |
|--------------------------|-----------------|-----------------|
| wall time                | 17.92 s         | 22.31 s         |
| c_steps/sec              | 5,581           | 4,483           |
| valid_moves/sec          | **2,064**       | **3,364**       |
| valid/c_step             | 0.37            | 0.75            |
| illegal/c_step           | 0.0012          | 0.0             |
| fn_steps/c_step (mean)   | 1.031           | 1.046           |
| ns per valid move        | 484,384         | 297,312         |
| episodes                 | 71              | 90              |

## Where the wall time goes (random run)

| Phase             | sum ms   | % wall  |
|-------------------|---------:|--------:|
| `c_reset_total`   | 15,348.5 | **85.7%** |
| └ reset_reload    | 12,901.9 |  72.0%  |
| └ reset_nle_start |  2,594.7 |  14.5%  |
| └ reset_nle_end   |     37.2 |   0.2%  |
| `c_step_total`    |  2,375.3 |  13.3%  |
| └ agent_fn_step   |  2,022.3 |  11.3%  |
| └ post_drain      |    329.5 |   1.8%  |
| └ obs_pack        |      8.5 |   0.0%  |

Reset is essentially the entire wall clock. Each reset costs **~217 ms** (181 ms dlopen-copy + 36 ms `nle_start`). With ~1400 random keystrokes per character life, a reset happens every ~250 ms of actual play.

## Surprises vs prior hypotheses

1. **"Chattiness is high" was wrong.** The fn_steps_per_c_step histogram is heavily skewed to 1: 98,348 / 100,000 c_steps issued exactly one fn_step. Drain runs almost never. Mean fn_steps/c_step = 1.031.
2. **Stepping itself is fast.** With reset excluded, 100k c_steps in 2.4 s = **41,667 c_steps/sec** single-thread. NLE's mainloop is ~20 µs per fn_step — that's the single-thread floor.
3. **Reset dominates more than expected.** Was assumed to be ~18 % from earlier benches; the profile says ~86 %. The earlier "reset cost ~210 ms" estimate was right per-call, but characters die *very* fast under random play (~1400 keystrokes), so resets fire much more often than one-per-thousand.
4. **The "63 % of c_steps don't produce a valid move" problem is not menu absorption.** Misc-prompt drain only fires on ~3 % of c_steps. The other 60 % are actions that NetHack processed but that didn't tick `moves` — wall bumps, invalid commands on the current tile, etc.

## Throughput projections

If we hide reset overhead via a worker pool:

| scenario                   | c_steps/sec/env | valid/c_step | per-env valid/sec | 32 procs |
|----------------------------|----------------:|-------------:|------------------:|---------:|
| random, no reset hidden    |          5,581  | 0.37         |             2,064 |    66 k  |
| random, reset hidden       |         41,667  | 0.37         |            15,414 |   493 k  |
| wait, reset hidden         |         38,461  | 0.75         |            28,846 |   923 k  |
| trained-ish, 0.95 valid    |         41,000  | 0.95         |            38,950 |  **1.25 M** |

**Implication**: 1 M valid moves/sec is achievable with 32 parallel envs *only if* reset is amortized. Reset cost is therefore not "lowest priority" any more — it's the headline bottleneck.

## Revised priority

The user's stated order was: valid_moves → chattiness → parallelism → reset (lowest).

Profile says: reset is the gating problem. But the user's *intent* (and probably correct intent) is that the worker-pool layer **hides** reset — i.e. the optimization is "build the pool", not "make the dlopen faster". So the priorities still hold; we just need to be honest that **without** the pool, single-env steady-state is currently capped at ~2 k valid/sec by reset overhead.

## Next experiment: exp_002

Goal: characterize the 60 % "agent action processed, no time advance, no detected prompt" rate. Add counters:
- `agent_advanced_time` — game time advanced, agent action was a real turn
- `agent_no_advance_no_prompt` — agent action processed, no `?` in message, no misc flag set, no time advance → wall bumps / silent invalid commands
- `agent_no_advance_silent` — same but message was empty (action was a complete no-op like ESC at the main prompt)

Hypothesis: most "no time advance" are wall bumps and stairs/pickup on wrong tile — agent skill issues, not harness issues. If confirmed, the 37 % valid/c_step is just the right number for random play, and the lever to pull is the policy, not the env. If disproven (e.g. a category we missed), there's more harness work.

Secondary measurement: micro-bench `agent_fn_step` distribution (already in the histogram) to confirm 20 µs is a NetHack-imposed floor.
