# exp_007 — PufferLib VecEnv smoke test

## Goal
Verify our `binding.c` actually plugs into `pufferlib._C.create_vec`,
and that `vec.cpu_step` drives all N envs each tick.

## Setup
- Built `_C.so` with `EXTRA_CFLAGS=-DNETHACK_PROFILE=0` and
  `bash build.sh nethack --cpu` (intel oneAPI module loaded for
  `-liomp5`).
- Wrote `smoke.py`: creates `total_agents` envs, resets, steps N times
  with random actions, reads back the log dict.
- Set `NETHACK_LIBPATH` and `NETHACKDIR` to absolute paths so workers
  outside the repo cwd can still find the .so + nhdat.

## Result

| AGENTS | OMP threads | STEPS | reset time | c_steps/sec | per-env rate |
|-------:|------------:|------:|-----------:|------------:|-------------:|
|      8 |          1  |   200 |    1.61 s  |    3,253    |     407      |
|      8 |          8  |   200 |    -       |    5,498    |     687      |
|     64 |         32  |   500 |    -       |   17,209    |     269      |
|     64 |         64  |  2000 |   14.18 s  |    5,460    |      85      |

## Findings

1. **VecEnv works.** The binding compiles into `_C.so`, `create_vec()`
   builds N independent envs, and `cpu_step` iterates through them
   each tick. Episode lengths, valid_moves, illegal_actions, new_tiles
   all flow back via `vec.log()`.

2. **Resets do not parallelize across OMP threads.** At N=64 with 64
   threads, the initial reset still took 14 s — about the same as
   running 64 sequential 217 ms resets. The reason: `dlopen()` is
   process-global serialized inside the dynamic linker. Threads
   waiting on the loader lock contribute nothing.

3. **Step phase also doesn't scale through OMP at high frequencies of
   reset.** At N=64 OMP=64 we got only 5,460 c_steps/sec — the
   per-c_step reset rate dominates because random play kills the
   character every ~1,600 c_steps, so a 64-env batch hits ~80 reset
   events in 128k c_steps and they all serialize.

4. **At low reset rate, OMP step parallelism kicks in.** N=8 jumped
   from 3,253 (single-thread) to 5,498 (8 threads) c_steps/sec — a
   1.7× boost from 8 threads. The remaining gap is reset cost in the
   tail.

## Conclusion: vecenv-via-threads is **not** the parallelization path for this env

PufferLib's `cpu_vec_step` parallelism is good for envs with cheap
resets. NetHack's dlopen-heavy reset path is process-global serialized,
so threads collapse on the loader lock. We must use multi-process
(separate procs with their own dynamic linker state) to actually scale.

This implies the **PufferLib training pipeline needs `num_buffers > 1`**
or a multi-process worker pool layer above the vecenv. The
`bindings_cpu.cpp::create_vec` does take a `num_buffers` kwarg —
investigating how that interacts with the loader lock is the next step.

## Decision

Don't try to scale `total_agents=8192` in one vecenv process for this
env — the reset serialization will eat the gains. Either:
- Use multiple OS processes (run N copies of `puffer train --slowly`
  with smaller total_agents each), or
- Fix the loader serialization with a custom reset path that doesn't
  go through dlopen each time (preload K spare dlopen-copies at
  startup and hand them out on reset — that moves the dlopen cost to
  startup time, off the hot path).
