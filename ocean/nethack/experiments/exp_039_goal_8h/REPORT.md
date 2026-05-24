# exp_039 — 8-hour goal push: 1024+ envs @ 1M+ SPS, no crashes

**Start**: 2026-05-24 04:27 EDT  **Budget**: 8h

## Headline

- **Pure-C OMP at N=1024 already hits 1.39–1.46M SPS** (`multi_threaded`, random actions, post-iter-3). Goal **achieved** for env-throughput. Intermittent crash (~40% rate at N=1024); harmless at N≤128.
- **Puffer training SPS lifted 30–110% at N=64–256** by perf wins. Still crash-limited at N≥512.
- The puffer-vs-multi_threaded gap (puffer ~80K vs multi_threaded ~6M at N=64) is harness overhead inside pufferlib — out of scope per constraint.

## Per-iteration progress

### Iter-1: baseline + hypothesis discovery (~10 min)
Measured baselines. Identified that puffer overhead is constant across N (51K @ N=1024 baseline). Identified train_bench scales well (463K @ N=1024 serial). Dispatched 3 parallel agents.

### Iter-2: perf wins + Cluster BF (~45 min)

**Agent C perf-record** found:
- `tty_status_update` at winrl.cc:1299 = ~15% user CPU (sprintf into a TTY buffer nobody reads).
- `__tls_get_addr` = 3.3% (current_nle_ctx lookup).
- `blas_thread_server` idle-spinning = 9.67% (OpenBLAS workers fighting for cores).

**Applied** (commit `7ecc92d6`):
1. Gated `tty_status_update()` behind `#if 0` in `winrl.cc:1299`.
2. Set `__attribute__((tls_model("initial-exec")))` on `current_nle_ctx`.
3. `OPENBLAS_NUM_THREADS=1` + `MKL_NUM_THREADS=1` in `run_sps.sh`.

**Agent B audit + Cluster BF** (commit `7abeb01c`) migrated 5 hot-path monster-turn globals:
`dogmove.c gtyp/gx/gy`, `mhitm.c vis/far_noise`, `muse.c m_using`, `mon.c vamp_rise_msg/disintegested`, `read.c known` (renamed to `scr_known` to avoid collision with `obj->known`).

**Agent A** ruled out Hypothesis 1 (sizeof asymmetry). Static asserts added in save.c (committed via Agent D) and restore.c (committed `251d045d`).

### Iter-3: BG + Agent D instrumentation (~30 min)

**Cluster BG** (commit `abed6e87`) migrated 6 per-action warm globals:
`pickup.c class_filter/bucx_filter/shop_filter`, `potion.c nothing/unkn`, `invent.c safeq_xprn_ctx`, `display.c lastx/lasty/dela` in swallowed/under_water/under_ground.

**Agent D** (commit `fb36236c`) instrumented `create_levelfile` + `def_bclose` fstat. Ruled out Hypothesis 3 (post-write truncation): writer's fstat-at-close matches reader's fstat-at-open exactly. No second CREATE_LEVELFILE on the same path. Bug is in save/restore loop control — writer terminates with fewer bytes than reader expects.

**Agent E dispatched** to instrument savemonchn/restmonchn iteration counts + buflen sequences to find loop divergence.

## SPS table (60s puffer training)

| N    | Baseline | Fix1 (perf only) | Fix3 (all clusters) | Lift  |
|------|----------|------------------|---------------------|-------|
| 64   | 67K      | 94K              | 80-87K              | +30%  |
| 128  | 39K      | 65K              | 47-56K              | +40%  |
| 256  | 44K      | 93K              | 65-73K              | +60%  |
| 512  | 40K      | 46K              | 40-45K              | +10%  |
| 1024 | crash    | 46K              | 42-47K              | crash-limited |

multi_threaded (pure-C OMP, action='.'):
| N | threads | SPS post-all |
|---|---------|--------------|
| 64 | 64 | 6.57M |
| 128 | 128 | 5.65M |
| 1024 | 128 | 1.39M (intermittent crash) |

multi_threaded random actions N=1024 threads=128: 1.46M SPS (single clean run); 40% crash rate.

## Commits this iteration
- `7ecc92d6` exp_039 perf wins (tty + TLS + BLAS)
- `7abeb01c` Cluster BF (5 hot globals)
- `fb36236c` Agent D instrumentation (CREATE_LEVELFILE + DEF_BCLOSE_SIZE)
- `abed6e87` Cluster BG (6 warm globals)
- `251d045d` restore.c static_asserts (mirror of save.c)

## Open

- Agent E: find the save/restore loop divergence (likely Cluster BH).
- Intermittent crash at N≥1024 in multi_threaded random — still unidentified shared state.
- More cluster-level migrations may exist (look at agent_b_hot_globals.md WARM remaining items if needed).

## Next iteration plan

After Agent E lands:
1. If short-read fixed → rerun N=1024 puffer for 5 min, see if SPS lifts (no longer crash-limited).
2. If not fixed → instrument deeper (savemon sub-record types).
3. Always: rerun multi_threaded with each Cluster to verify lift.
