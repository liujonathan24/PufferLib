# exp_030 — 1-hour NetHack production training validation

## Headline

**Run did NOT survive 1 hour.** Two attempts; both ended early before the wall-clock budget. Final `episode_return` never moved off the initial value of **1.100** — far below the standing goal of >1000. The training loop itself was healthy (SPS ~34K, GPU at 3%, no Python tracebacks); the failure was a libnethack.so segfault, plus an external SIGKILL on the first attempt that I have not fully attributed.

## Config

- Branch: `4.0` at HEAD `189ea34f` (nle.c: retire dead iflags/sysflags memcpy from swap_in/swap_out)
- `--vec.total-agents 512 --vec.num-buffers 1 --vec.num-threads 1`
- `--train.gpus 1 --train.total-timesteps 100000000`
- `--train.minibatch-size 32768 --train.horizon 64`
- libnethack.so: `vendor/nle/src/build/libnethack.so` (9.4 MB, mtime 22:46 same day)

## Attempt 1 (timeline 23:38 launch → killed ~23:51, ≈12m45s uptime)

Final dashboard snapshot before kill:

```
Steps             25.3-25.4M
SPS               34.3-34.4K
Epoch             773-774
Uptime            0d 0h 12m 22s
GPU               3%      VRAM 3.0/39G    RAM 1.0G
policy loss       0.025   value loss 0.152   entropy 0.002   total 0.040
kl                0.000   clipfrac   0.006
User Stats: episode_return 1.100, episode_length 4480, depth 0,
            valid_moves 11, illegal_actions 0, new_tiles 11
```

- **Termination:** killed externally (kernel `Killed` line on the wrapping `timeout 3600 bash -c '...'`). No segfault in dmesg attributable to this attempt's PIDs/timestamps. No OOM (`free -h` showed 754 GiB total, 402 GiB free). No Python traceback (run.err is 0 bytes throughout).
- Most likely cause: the parent bash session got SIGKILL'd at the harness/shell-session level when I scheduled a Monitor task; the `timeout` wrapper inherited the kill and tore down the training subprocess. Switching to `setsid` for attempt 2 prevented a repeat of this specific failure mode.

## Attempt 2 (timeline 23:51:30 launch → segfault 23:58:23, ≈6m53s uptime)

Final dashboard snapshot (written 23:56:08, ~2 min before the crash):

```
Steps              8.6M
SPS                34.6K
Epoch              261
Uptime             0d 0h 4m 15s
GPU                3%      VRAM 3.0/39G    RAM 1.0G
policy loss        0.013   value loss 0.325   entropy 0.002   total 0.046
kl                 0.001   clipfrac   0.004
User Stats: episode_return 1.100, episode_length 4480, depth 0,
            valid_moves 11, illegal_actions 0, new_tiles 11
```

- **Termination:** segfault in libnethack.so. dmesg line:

```
[Fri May 22 23:58:23 2026] puffer[2484970]: segfault at 34 ip 00001455839eb09c
  sp 00001454e66ef570 error 4 in libnethack.so[1455836c6000+35d000]
  likely on CPU 127 (core 63, socket 1)
Code: 00 e8 9e ee ee ff eb 01 90 48 8b 5d f8 c9 c3 55 48 89 e5 48 83 ec 20
      48 89 7d f8 48 89 75 f0 48 89 55 e8 89 4d e4 48 8b 45 f8
      <0f> b6 40 34 ...
```

- Offset within libnethack.so: `0x9eb09c - 0x6c6000 = 0x32509c`.
- `addr2line -e vendor/nle/src/build/libnethack.so 0x32509c -f` →
  `water_damage_chain` at `vendor/nle/src/src/trap.c:3695`.
- Faulting instruction is `movzbl 0x34(%rax),%eax` and the fault address is exactly `0x34`, so `%rax == NULL`. The trapped code is reading byte offset 0x34 of a NULL pointer — the local `otmp` walk inside `water_damage_chain` (loop body reads `otmp->oclass` etc.) is the obvious candidate.
- No worker-side Python error: `run.err` is 0 bytes; the C-level segfault tore the worker down before the parent ever got an exception.

## Throughput

- SPS ~34.3-34.6K, GPU only 3% (heavily env-bound: Env 92% of frame, GPU 2%, Train 4%). Consistent with N=512 + 1 thread saturating env step. Total steps across both attempts: 25.4M + 8.6M = 34M, ≈17 min of wall.

## episode_return analysis

User Stats was frozen at the same values (return 1.100, length 4480, valid_moves 11, depth 0, etc.) across every poll of both attempts. With `--train.horizon 64`, an "episode" rollout slice is 64 env steps; at 512 envs that's 32K steps per epoch, and we ran 773 epochs in attempt 1. So data was flowing — but no actual NetHack episode terminated long enough for fresh aggregate stats to land, or the env wrapper holds stats frozen until the first true game-over. Either way, in 17 cumulative minutes the agent did not complete a meaningful episode, so `episode_return > 1000` was not even approached. The numbers (length=4480 ≈ 70 × horizon; valid_moves=11) look like a single bootstrap-reset placeholder rather than learned play.

## Crash status

- **Attempt 1:** external SIGKILL (not a segfault, not OOM). Cause not fully attributed; switching to `setsid` to detach from the shell session prevented recurrence.
- **Attempt 2:** segfault in `libnethack.so:water_damage_chain` (trap.c:3695). NULL `otmp` walk inside the obj-chain water-damage loop. This is a real refactor-related bug, not a flaky env crash.

## Whether the goal was met

No. `episode_return` never exceeded **1.1** in 34M cumulative training steps. The standing goal is >1000. Even setting aside that target, the agent never visibly progressed (depth stayed 0, valid_moves stayed at 11).

## Recommendations / open items

1. **Investigate `water_damage_chain` (trap.c:3695) under N=512.** Likely candidates: an obj-chain pointer that was not migrated into `nle_ctx_t` (or that the swap_in/swap_out path leaves dangling). The earlier dmesg history (from prior sessions tonight) shows repeated `+0x35d000`-range crashes in libnethack.so, suggesting an obj/level pointer that is shared instead of per-env.
2. **Diagnose the User-Stats freeze.** With 8.6M-25M steps logged, at least one of 512 envs should have ended a game and refreshed `episode_return`. Worth confirming whether the wrapper's stats publish path is actually firing on episode termination, or whether episodes simply aren't terminating (deathless agent stuck in starting room — 11 valid moves matches a fresh @-on-stairs state).
3. **Rerun at N=64 or N=128** to disambiguate: was the segfault load-induced (N=512 specific) or would it have hit any large run? The latest stability sweep tested smaller N — N=512 is 4x beyond that.
4. **External-SIGKILL mitigation:** keep using `setsid` for long runs, or launch via SLURM `srun --pty` to avoid being tied to harness session.
