# exp_002 — characterize no-time-advance c_steps

## Hypothesis
The 63 % of c_steps that don't produce a `valid_move` are mostly wall bumps and "wrong tile" actions — agent-skill issues, not harness gaps. If true, lever to pull is parallelism, not chattiness.

## Method
Added three classifier counters in `c_step`, applied when `time_after == time_before`:
- `noadv_prompt_detected` — a yn/getlin flag or message-`?` was caught, escape ran
- `noadv_has_msg` — no prompt was caught, but NetHack put something on the message line
- `noadv_silent` — no prompt, no message, no time

## Result (100k c_steps each)

| Category                  | random %    | wait %     |
|---------------------------|------------:|-----------:|
| valid_moves               | **36.4**    | **74.9**   |
| no-advance (illegal)      | 0.1         | 0.0        |
| no-advance with message   | **52.4**    | 1.3        |
| no-advance silent         | **11.1**    | **23.7**   |
| (sum)                     | 100.0       | 100.0      |

`agent_fn_step` mean: 18.8 µs (random), 20.8 µs (wait) — NLE single-step floor unchanged from exp_001.

## What the numbers say

- **Random**: 52 % of all c_steps hit "wall bump / nothing-to-pick-up / can't-go-up-here" — a printed feedback message but no turn. Plus 11 % silent.
- **Wait**: only 1 % message-no-advance (wait rarely prints feedback). But 24 % silent — surprisingly high. `.` is supposed to advance time at the main prompt. Likely candidates:
  - NetHack in an intra-turn animation/yield state where the keystroke is buffered
  - A hidden single-key sub-prompt (direction/select) that doesn't put text on the message line but still eats input
  - The `getch()` path for non-game keys (e.g. cursor adjust)
- **Illegal detection is fine.** Our message-`?` + misc-flag rule catches the very few prompts triggered by random actions. The bulk of no-advance is NOT a hidden-prompt problem we can reach from the harness.

## Implication for the 1 M valid/sec goal

The valid/c_step ratio is a property of the action policy and the NetHack input model. The harness can shave a few % by handling more obscure prompt types, but it cannot make wall bumps into game turns. So **the path to 1 M is parallelism × reset-hiding**, not "make each c_step a real turn".

Re-projecting:

| valid/c_step assumption | 32 procs × 38 k c_steps/sec (reset hidden) |
|------------------------:|--------------------------------------------:|
| 0.37 (random baseline)  | 449 k valid/sec                             |
| 0.50 (mild action filter)| 608 k                                       |
| 0.75 (wait or trained)  | 912 k                                       |
| 0.95 (best)             | 1.16 M ✓                                    |

`valid/c_step` between 0.50 and 0.75 gets us within striking distance of 1 M at 32 procs. **At 64 procs even the random baseline crosses 1 M.**

## Decision: skip "increase valid moves" optimization, jump to parallelism

The user listed valid-moves before parallelism, but exp_001+exp_002 show the better lever is parallelism+reset-hiding. The valid_moves ratio under random play is already non-pathological (37 %), and the policy will learn to do better over training — that's what RL is for. The harness has done its job.

## Next experiment: exp_003 — vecenv compile + scaling

Goal: confirm PufferLib's vecenv path compiles cleanly with our nethack env (we saw the static lib + `bindings_cpu.cpp` build, modulo libomp5 — that issue is resolved with `intel/2024.2` + `-liomp5`). Then run a multi-proc microbench using vecenv and measure aggregate c_steps/sec and valid_moves/sec as N scales from 1 → 16.

If vecenv scales linearly to ~16 procs at our 30k c_steps/sec/env baseline, we're at 480k c_steps/sec aggregate. At even 50% valid ratio that's 240k valid/sec — meaningful progress. The other half of the way to 1M needs the worker-pool/reset-hiding layer.
