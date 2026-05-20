# exp_003 — multi-process scaling

Host: AMD EPYC 7H12 64-Core × 2 sockets = 128 cores. Login node, no `srun`
control — numbers below should be re-collected under `srun` on a compute
node for cleanliness.

Method: spawn N independent `./nethack profile` processes in parallel, each
50 000 c_steps. Each process owns its own dlopen-copy of `libnethack.so`
and its own `/tmp/nle-XXXXXX` vardir. No IPC. Aggregate metric =
sum(valid_moves) / max(wall) across N processes.

## Random policy, 50k c_steps/proc

| N   | wall  | valid/s | scaling vs N=1 | efficiency |
|-----|------:|--------:|---------------:|-----------:|
| 1   | 10.71 | 1,714   | 1.00×          | 100%       |
| 2   | 10.84 | 3,343   | 1.95×          | 98%        |
| 4   | 10.48 | 6,938   | 4.05×          | 101%       |
| 8   | 11.84 | 12,144  | 7.09×          | 89%        |
| 16  | 12.91 | 22,242  | 12.98×         | 81%        |
| 32  | 15.59 | 37,172  | 21.69×         | 68%        |

## Wait policy, 50k c_steps/proc

| N   | wall  | valid/s   | scaling vs N=1 | efficiency |
|-----|------:|----------:|---------------:|-----------:|
| 1   | 12.77 | 2,928     | 1.00×          | 100%       |
| 2   | 14.56 | 5,147     | 1.76×          | 88%        |
| 4   | 15.81 | 9,480     | 3.24×          | 81%        |
| 8   | 17.27 | 17,348    | 5.92×          | 74%        |
| 16  | 18.73 | 31,993    | 10.93×         | 68%        |
| 32  | 18.78 | 63,856    | 21.81×         | 68%        |
| 64  | 22.82 | 105,061   | 35.88×         | 56%        |
| 128 | 30.86 | **155,496** | 53.10×       | 41%        |

`valid/c_step` ratio is rock-stable at ~0.75 across all N for wait — the
sub-linear scaling is entirely from per-process wall-time stretching, not
from valid-rate dropping. So the bottleneck is **shared hardware
resources** (memory bandwidth, dynamic-linker serialization, possibly /tmp
contention during vardir creation), not anything per-env.

## Intra-process multi-env (negative result)

Tried `nethack_multi.c`: hold M envs in one process, round-robin step
them. 2 envs in 1 proc gave 4,233 c_steps/sec total — *slightly slower*
than 1 env in 1 proc (4,667). Reason: it's all one thread; resets are
CPU-bound, so interleaving doesn't overlap anything. Confirmed the path
forward is **multi-process**, not multi-env-per-process. We could try
multi-thread later if NLE proves thread-safe across distinct dlopen
copies, but it's not the next win.

## Single-process intra-env init cost
First-reset cost: 547 ms/env (vs 217 ms steady-state). Cold dlopen + page
faults + first NetHack init. Worth caching the first-reset result
somehow (e.g. snapshot the post-init NetHack state and restore from it).
Tabled for later.

## Throughput projection (1 M valid/sec target)

| scenario                                       | valid/sec | gap to 1 M |
|------------------------------------------------|----------:|-----------:|
| current: 128 procs, wait policy                | 155 k     | 6.4×       |
| if reset cost halved                           | ~310 k    | 3.2×       |
| if reset cost eliminated (worker pool)         | ~600 k    | 1.7×       |
| 256 procs (2-node SLURM) + reset hidden        | ~1.2 M    | ✓          |
| 128 procs + reset hidden + valid/c_step → 0.95 | ~760 k    | 1.3×       |

To hit 1 M on a single node, **both** reset-hiding **and** valid-rate
improvement are needed. To hit 1 M on 2 nodes, reset-hiding alone gets
us there.

## Caveats — must redo under `srun`

The login node is shared. The 41% efficiency at N=128 is likely
overstating contention because other users had jobs on the box. Future
experiments will use `srun` to pin a clean CPU set.

## Decision: exp_004 will redo this under `srun` + investigate the reset-hiding worker pool design.

Specifically:
1. Allocate a clean node (`salloc -N 1 --cpus-per-task=64`) and re-run
   `wait` at N = 1, 2, 4, 8, 16, 32, 64 to get clean scaling numbers.
2. Sketch a "background reset" path: spawn a helper thread per env that
   does the dlopen + nle_start in the background while the main thread
   handles other envs. Then env's "reset" becomes pointer-swap, ~µs.
3. Validate with another profile run.
