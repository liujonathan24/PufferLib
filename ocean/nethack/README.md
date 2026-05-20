# NetHack environment for PufferLib

C-level NLE binding for the PufferLib RL framework. Each env owns a
private `dlopen`-copy of `libnethack.so` so NetHack's many globals stay
isolated per-env. Auto-dismiss for menu prompts, compile-time
observation selection, reward shaping, profiler hooks.

See `SETUP.md` for build instructions and `experiments/` for the
performance analysis log.

## Observation (compile-time selectable, default = chars only)

Each enabled field reserves a slice of a single flat `ByteTensor`
observation buffer. Disabled fields are not allocated (no struct
member), not bound (NLE skips writing them via its `if (ptr) ...`
guards), and not packed into the obs tensor. Override defaults with
`-DNETHACK_USE_<FIELD>=1`.

| Field        | Default | Bytes/element | Total (one env)              |
|--------------|--------:|--------------:|------------------------------|
| `chars`      |    1    | 1             | 1,659                        |
| `colors`     |    0    | 1             | 1,659                        |
| `specials`   |    0    | 1             | 1,659                        |
| `glyphs`     |    0    | 2 (le i16)    | 3,318                        |
| `blstats`    |    0    | 4 (i32, trunc)| 108                          |
| `message`    |    0    | 1             | 256                          |
| `inv`        |    0    | 1 (letters+oclasses) | 110                   |

`OBS_SIZE` is the sum of enabled fields.

## Action space (reduced, 23 actions)

```
 0  N            8  N_RUN         16  >  (down)
 1  S            9  S_RUN         17  <  (up)
 2  W           10  W_RUN         18  .  (wait)
 3  E           11  E_RUN         19  s  (search)
 4  NW          12  NW_RUN        20  \r (MORE)
 5  NE          13  NE_RUN        21  ESC
 6  SW          14  SW_RUN        22  ,  (pickup)
 7  SE          15  SE_RUN
```

The 8 cardinal/intercardinal moves use vi-keys (kjhl ynbu). Long
"run" versions are uppercase (KJHL YNBU). `\r` and `ESC` mostly exist
to dismiss menus but the policy can use them.

## Reward shaping (compile-time tunable)

```
reward = (blstats[SCORE] - prev_score)                 # game reward
       + NETHACK_DEPTH_BONUS * (new_depth)             # default 1.0 per new dungeon level
       + NETHACK_SCOUT_BONUS * (new_tile_this_level)   # default 0.1 per first visit
       + NETHACK_ILLEGAL_PENALTY * (illegal_action)    # default -0.5 per sub-prompt trigger
```

`illegal_action` fires when the agent's keystroke triggers an
`in_yn_function`, `in_getlin`, or message-ending-`?` sub-prompt that
the harness then ESCs out of. The auto-dismiss for benign `--More--`
prompts does *not* count as illegal.

## Auto-dismiss hook

After each agent action (and after the post-reset welcome screen) the
harness inspects `misc[]` (`in_yn_function`, `in_getlin`,
`xwaitingforspace`) plus a heuristic message-ends-in-`?` check. If a
prompt is detected we drain it with `\r`/`ESC` so the next c_step
lands at a real decision point. Capped at `NETHACK_AUTODISMISS_MAX=64`
iterations.

## Per-episode log entries

| Key                | Meaning                                                |
|--------------------|--------------------------------------------------------|
| `perf` / `score`   | Final NetHack score                                    |
| `depth`            | Final dungeon level                                    |
| `episode_return`   | Sum of shaped reward                                   |
| `episode_length`   | Number of c_steps                                      |
| `valid_moves`      | c_steps where NetHack's turn counter actually advanced |
| `illegal_actions`  | c_steps where the agent triggered a sub-prompt         |
| `new_tiles`        | Unique tiles entered this episode                      |

## Standalone driver subcommands

```bash
./nethack                                  # interactive 50-step render
./nethack 200                              # interactive N-step
./nethack record OUT.txt 200 [random|wait] # ASCII trajectory log
./nethack bench N [random|wait]            # quick throughput bench
./nethack resets N                         # reset-only bench
./nethack profile OUT.json N [policy] [seed]  # full profiler dump
```

Policies: `random`, `wait` (`.`), `north` (`k`), `safe` (NSWE cycle).
The profile subcommand requires a build with `EXTRA_CFLAGS=-DNETHACK_PROFILE=1`.

## Experiments

Each `experiments/exp_NNN_*/` folder contains a `NOTES.md` documenting
hypothesis → result → decision, plus the raw JSON profile output.

Summary so far (see `RETROSPECTIVE_1.md`):
- Single-thread harness ceiling: **~228 k c_steps/sec** with no resets
  (`north` policy). NLE's `fn_step` is the floor at ~3-20 µs.
- Reset cost: ~217 ms (180 ms `dlopen` + 36 ms `nle_start`). Dominates
  wall time for random play (~85 %) because deaths are frequent.
- Multi-process scaling: linear to ~16 procs, ~70 % eff at N=32, ~40 %
  at N=128 (login node, contended).
- Aggregate ceiling on this 128-core node: **~11 M c_steps/sec** at
  N=128 north policy.
- 1 M valid_moves/sec is achievable: at trained `valid/c_step ≈ 0.6`
  needs ~32 cores; at random `≈ 0.37` needs ~64 cores.

The single ongoing question is the **reset duty cycle during training**
— how often does a learning agent actually die? That gates whether
the reset-hiding worker pool is required or "nice to have".

## Build

See `SETUP.md`.
