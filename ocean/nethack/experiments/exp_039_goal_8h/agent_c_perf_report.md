# Agent C: perf-record profile of puffer NetHack training

## Setup

- Wrapper: `ocean/nethack/experiments/exp_038_goal_8h/run_perf.sh`
- Command: `perf record -F 99 -e cpu-clock -g --call-graph dwarf` (software event; `/proc/sys/kernel/perf_event_paranoid = 2`, so hardware PMU events refused, software events work without root)
- Workload: `puffer train nethack --vec.total-agents 128 --vec.num-buffers 1 --vec.num-threads 1 --train.gpus 1 --train.horizon 64 --train.minibatch-size 8192`, timeout 35 s
- Steady-state SPS during capture: ~38 K (Env share = 92% of step time per the puffer dashboard)
- Run ended with the known short-read segfault around t≈9–10 s (still expected for N=128)
- Capture: `perf.data` 20.2 MB, 2369 cpu-clock samples
- Binary: `libnethack.so` is RelWithDebInfo (build-id present, debug info present, not stripped). User-only samples (`cpu-clock:u`).

## DSO-level breakdown (% of user CPU)

| DSO | % |
|-----|---|
| _C.cpython-311 (puffer C extension) | 42.7% |
| libnethack.so | 23.1% |
| libc.so.6 | 11.7% |
| libscipy_openblas64_.so | 9.7% |
| python3.11 | 3.8% |
| ld-linux (`__tls_get_addr`) | 3.7% |
| libcuda.so | 3.6% |
| libtorch_cpu / libtorch_python | 0.8% |
| libiomp5 / libgomp | < 0.1% |

OpenMP runtime overhead from libgomp/libiomp itself is negligible (< 0.1%). Puffer's own
`static_omp_threadmanager` (worker spin/dispatch) is 2.2% self.

## Top 20 symbols (flat / self time)

| # | Self | DSO | Symbol |
|---|------|-----|--------|
| 1  | 39.93% | _C.cpython-311 | `static_vec_omp_step` (puffer step dispatcher; self time = inlined env work it absorbs) |
| 2  | 9.67%  | libscipy_openblas64 | `blas_thread_server` (BLAS workers spinning, see below) |
| 3  | 3.29%  | ld-linux | `__tls_get_addr` (thread-local lookup; nle_ctx_t / `__thread` globals) |
| 4  | 3.08%  | libc | `printf_positional` (called from `sprintf` in `tty_status_update`) |
| 5  | 2.20%  | _C.cpython-311 | `static_omp_threadmanager` (puffer OMP worker manager) |
| 6  | 2.15%  | libnethack.so | `tty_status_update` |
| 7  | 2.07%  | libnethack.so | `recalc_mapseen` |
| 8  | 1.44%  | python3.11 | `_PyEval_EvalFrameDefault` |
| 9  | 1.18%  | libc | `__parse_one_specmb` (printf format parser) |
| 10 | 1.18%  | libc | `__strchrnul_avx2` (printf internals) |
| 11 | 1.10%  | libc | `_IO_default_xsputn` (printf internals) |
| 12 | 1.06%  | libnethack.so | `mfndpos` (monster pathfinding helper) |
| 13 | 0.80%  | libc | `__strlen_avx2` |
| 14 | 0.80%  | libc | `__vfprintf_internal` |
| 15 | 0.76%  | libnethack.so | `eval_notify_windowport_field` (botl render dispatch) |
| 16 | 0.68%  | libnethack.so | `can_reach_location` |
| 17 | 0.59%  | libnethack.so | `nle_putchar` |
| 18 | 0.55%  | libnethack.so | `nethack_rl::NetHackRL::fill_obs` |
| 19 | 0.55%  | libnethack.so | `rn2` (RNG) |
| 20 | 0.51%  | libnethack.so | `vision_recalc` |

## Top inclusive (children) callers — what the time rolls up into

| Inclusive | DSO | Symbol |
|-----------|-----|--------|
| 36.43% | libnethack.so | `moveloop` (full env tick) |
| 14.86% | libnethack.so | `bot` → `bot_via_windowport` (status bar render) |
|  9.41% | libnethack.so | `movemon` → `dochug`/`dochugw` (monster AI driver) |
|  8.91% | libc          | `sprintf` (all inside `tty_status_update` / `render_status`) |
|  7.98% | libnethack.so | `eval_notify_windowport_field` |
|  7.81% | libnethack.so | `m_move` (monster movement) |
|  7.77% | libnethack.so | `NetHackRL::rl_status_update` |
|  6.80% | libnethack.so | `domove`/`domove_core` (player action handling) |
|  6.42% | libnethack.so | `tty_status_update` (children) |
|  4.43% | libnethack.so | `dog_move` |
|  3.21% | libcuda       | `cuStreamSynchronize` (pytorch→GPU) |
|  2.62% | libnethack.so | `dog_goal` |
|  2.41% | libnethack.so | `vpline` (message printing) |
|  2.28% | libnethack.so | `recalc_mapseen` |

## Interpretation

**Where the env-side CPU is actually going.** The single biggest user of CPU inside the env
is *status-bar rendering* — the chain `bot → bot_via_windowport → eval_notify_windowport_field
→ rl_status_update → tty_status_update → sprintf` accounts for ~15% inclusive, with about
~9% of that being raw `sprintf` (`printf_positional`, `__parse_one_specmb`, `__strchrnul_avx2`,
`_IO_default_xsputn`, `__strlen_avx2`, `__vfprintf_internal`). This is a side-effect of the
RL window-port: `winrl.cc:1299` calls `tty_status_update(...)` unconditionally inside
`NetHackRL::rl_status_update` (under `#ifdef STATUS_HILITES`, which is on). The RL policy gets
blstats directly via `NetHackRL::update_blstats()` / `fill_obs`, so the formatted ANSI status
line is never observed. **No NetHack core gameplay routine breaks 3% self.** `recalc_mapseen`
(2.07%), `mfndpos` (1.06%), `vision_recalc` (0.51%), `m_move` / `dog_move` / `dochug` (monster
AI, ~7–9% inclusive combined) are the next tier. The actual NLE pythonland binding
(`fill_obs`, observation memcpy) is only 0.55% — observation copy-out is not the bottleneck.

**Threading / sync / memory.** `libgomp` / `libiomp5` self time is < 0.1%, so OMP barriers
are *not* the dominant tax — workers aren't stalled at the join. However `blas_thread_server`
is 9.67% of *user* CPU: openblas64 starts a thread pool sized to the machine, and those
workers spin while puffer's OMP workers do the env stepping. That is pure waste and competes
for cores. `__tls_get_addr` is 3.29% self — a measurable cost of the nle_ctx_t /
`current_nle_ctx` cluster refactor, which routes every "global" through TLS. memcpy/memset/
page-fault traffic is small (< 0.2% combined `__memset_avx2*`, no significant `__memmove`),
so there is no evidence of memory-bandwidth saturation. `cuStreamSynchronize` is 3.2%,
i.e. the GPU forward path is small but non-zero; it is overlapped poorly with rollouts.

**Where to spend optimization effort (vendor/nle + ocean/nethack only).**
1. **Kill or gate `tty_status_update` inside the RL window-port.** Removing the
   `tty_status_update(...)` call at `vendor/nle/src/win/rl/winrl.cc:1299` (and the matching
   `tty_curs` traffic from `flush_screen`, ~2.5% inclusive) should remove ~10–14% of total
   user CPU. The blstats array that the agent actually reads is filled by
   `update_blstats`, which does not depend on this call. This is the single highest-impact
   change available within the allowed scope.
2. **Suppress the openblas thread pool.** A 9.67% spinning-pool tax is gratuitous when puffer
   already manages worker parallelism. Setting `OPENBLAS_NUM_THREADS=1` (and `OMP_NUM_THREADS=1`
   for nested BLAS) at the top of `ocean/nethack/experiments/exp_038_goal_8h/run_sps.sh`
   should reclaim ~10% of user CPU. (Pufferlib harness is off-limits to edit, but env vars
   set from our own launch script are fair game and don't require changing puffer code.)
3. **Reduce TLS pressure from nle_ctx_t.** 3.3% in `__tls_get_addr` is non-trivial.
   Hot inner functions (`mfndpos`, `recalc_mapseen`, `dochug`, `m_move`) likely re-resolve
   `current_nle_ctx` many times per call. Caching `current_nle_ctx` into a local at function
   entry, or marking `current_nle_ctx` as `__attribute__((tls_model("initial-exec")))` if not
   already, should knock most of it out.

Beyond those, no individual gameplay routine is a fat target. The end-to-end 9× gap between
`train_bench` and the puffer harness is *not* explained by env code being slow under puffer —
it is explained by (a) ~15% wasted on TTY status formatting, (b) ~10% wasted by the BLAS
spin pool, (c) ~3% TLS overhead, plus puffer's own dispatcher / OMP scheduling overhead
(~5–7% in `static_vec_omp_step` not attributable to env children, plus `static_omp_threadmanager`).
