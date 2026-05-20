# Golden trajectories for the nle_state refactor

Canonical seeded trajectories of the current (dlopen-based, pre-refactor)
NetHack env. The refactored env must produce byte-identical hash streams
for the same inputs.

## File: `golden_seed42_1k.bin`

- seed: 42
- action_seed: 99
- n_steps: 1000
- obs_size: 1767 (compiled with `NETHACK_USE_BLSTATS=1` + default chars)
- captured: 2026-05-20 against `vendor/nle/src/build/libnethack.so` (NLE 0.9.1)

Replay:
```
./verify_determinism replay --in ocean/nethack/golden/golden_seed42_1k.bin
```

## Why only 1000 steps?

Current env is bit-deterministic for the first ~1480 steps and then
diverges across runs even with identical NLE seeds. Root cause:
NetHack sets `hackpid = getpid()` and `urealtime.start_timing = getnow()`
from system sources that are not covered by `nle_seeds_init_t`. The
divergence at step 1480 likely corresponds to a game event that touches
one of these (`paniclog` flush, message timestamp, time-gated check).

Sources (vendored at `vendor/nle/src/`):

- `sys/unix/unixmain.c:96` — `hackpid = getpid();`
- `src/allmain.c:645` — `urealtime.start_timing = getnow();`
- `src/end.c:1244`, `src/save.c:295`, `src/restore.c:614` — endgame `urealtime` updates
- `src/files.c:3661` — `paniclog` timestamps
- `src/hacklib.c:917` — `getnow()` wrapper around `time()`

1000 steps is below the divergence point and is enough to exercise
movement, monster spawning, room transitions, message generation, and
inventory operations — sufficient as an oracle for the RNG / `flags` /
`level` refactor subsystems.

## Future: extending the golden trajectory

To go beyond 1000 steps, patch `unixmain.c` to take `hackpid` from a
fixed sentinel (e.g. 1234) instead of `getpid()`, and add a NLE-level
`now_override` that `getnow()` consults. Once those patches land, we can
record `golden_seed42_100k.bin` and verify multi-episode trajectories.

For each refactor subsystem (RNG, flags, level, ...) re-record the
golden after the refactor lands and confirm the new file is bit-identical
to the pre-refactor one. If they differ, the refactor introduced a
behavioral change — bisect to find which subsystem.
