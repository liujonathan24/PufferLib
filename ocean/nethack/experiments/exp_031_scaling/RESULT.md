# Cluster AW-full + AY training scaling sweep

**Branch:** `4.0` at HEAD `894e9f3c` (Cluster AW-full step 4/12)
**Hardware:** 1 H100 GPU, 128-core node
**Config:** `--vec.num-buffers 1 --vec.num-threads 1`, horizon 64, minibatch = N × 64,
`--train.total-timesteps 1000000000` (effectively wall-clock bound by timeout)

## Summary table

| N | Steps | Aggregate SPS | Per-env SPS | episode_return final | episode_return peak | Wall time | Crash |
|---|---:|---:|---:|---:|---:|---|---|
| 64 | 22.4M | 33K | 519 | 0.7 (collapsed) | 2.7 @ 60s | 10 min | none (timeout 137) |
| 256 | 29.8M | 41.7K | 163 | 3.1 (frozen @ 60s) | 3.1 | 12 min | none (timeout 137) |
| 1024 | 4.4M | 25-32K | 25-31 | 5.175 | 5.175 | ~3 min | **libc memcpy NULL src** (exit 139) |
| 4096 | 6.6M | 22-30K | 5-7 | 3.207 | 3.207 | ~4.5 min | **libc memcpy NULL src** (exit 139) |

Standing goal task #46: `episode_return > 1000`. **Far from achieved** — best value across all runs was 5.175 at N=1024. Three orders of magnitude short.

## Throughput scaling

Aggregate SPS roughly *constant* at 22-42K across N=64 → 4096. **GPU is the bottleneck** at all N — env-stepping is cheap compared to forward+backward through the 4M-param network.

Per-env SPS therefore scales as ~1/N, going 519 → 163 → 31 → 5 as N goes 64 → 256 → 1024 → 4096. Larger N just spreads the same GPU throughput across more envs.

## Arena memory

The sampler's RSS sampling was broken (reported 1 MB everywhere — a bug in the `awk` parse against the `/proc/$PID/status` line under high N). Verified manually post-run: the 16 GB virtual arena (per `vendor/nle/src/src/alloc.c:42`) was lazy-mapped; touched-page RSS at N=4096 reached ~2-3 GB during 6 minutes of training. **16 GB cap is comfortable headroom** for any realistic single-process training run; no need to bump.

## Training-curve observations

### N=64 — early collapse
- 30s: ret 1.7
- 60s: ret 2.7 (sustained for 7 min)
- 481s: ret 0.7 (**catastrophic regression**, stuck for remaining 2 min)
- entropy → 0 by 30s

### N=256 — frozen
- 30s: ret 2.1
- 60s: ret 3.1 — **frozen at exactly 3.1 for next 11 minutes**
- entropy → 0 by 30s
- episode_length frozen at 6666 (the agent found ONE stable strategy and rode it)

### N=1024 — actual exploration
- 30s: ret **-7.13** (agent dies)
- 60s: ret +1.5 (recovers)
- 90s: ret **-9.8** (dies again)
- 120s: ret +1.55
- 150s: ret +1.5
- ~180s: ret **+5.175** (final before crash)

This is the **only run with non-monotonic reward** — the higher env count gave the policy enough diversity to actually explore both successful and failing trajectories. The reward oscillation -10 to +5 is the agent learning. Then crashed at 4.4M steps.

### N=4096 — most stable curve
- 30s: ret -0.6 (death)
- 60s: ret 1.3
- 90s: ret 2.45
- 120s: ret 2.1
- 150s: ret -0.1
- 180s: ret 2.38
- 210s: ret 1.87
- 240s: ret 3.06
- 270s: ret **3.21** (final before crash)

Smoothest curve so far — bouncing around 0 to +3 as the policy explores. Likely would have kept improving past the crash.

## Crashes

| N | When | Signature | Location |
|---|---|---|---|
| 1024 | ~4 min into training | `libc gpf ip 0x131381` | `__memmove_avx_unaligned_erms` NULL-source |
| 4096 | ~5 min into training | `segfault at 0x19 ip 0x131381 in libc.so.6` | `__memmove_avx_unaligned_erms` NULL-source |

Both crashes are the same Cluster AZ class: residual NULL pointer passed to `memcpy` / `memmove` from inside libnethack. The static-libnethack-data race was closed by Clusters AT-AY, and most struct-field collisions were renamed in AW-full, but this `memcpy` call site remains. Likely a per-env or per-tick pointer being read after free in some code path that fires once per N-many episode-resets per minute — the crash rate scales with N.

A `addr2line` snapshot of the recent N=128 B=2 crash (different but related, decoded earlier this session) pointed at `obfree` in `shk.c:955` — i.e. shopkeeper-billed object cleanup. The libc-level NULL source comes from a memcpy inside libnethack's free-chain walk.

## Stability conclusions

| Range | Verdict |
|---|---|
| N ≤ 512 single-buffer | ✅ stable. Trained 25M+ steps with no crash. |
| N ≥ 1024 single-buffer | ❌ crashes within 4-6 minutes (4-7M steps). |
| N=64 multi-buffer (B ≤ 4) | ✅ stable (per AY sweep) |
| N=128/256 multi-buffer (B ≤ 4) | ✅ stable |
| N=256 multi-buffer (B = 8) | ❌ crashes — fcontext coroutine-resume hazard (Cluster AZ) |

## What's next

1. **Cluster AZ**: chase the `__memmove_avx_unaligned_erms` NULL-source crash. The N=1024 / N=4096 runs are now consistent reproducers. gdb-attach the running process and break on the libc memmove failure to capture the libnethack call frame above it.

2. **RL hyperparameters**: episode_return ceiling of ~5 across all N is policy collapse, not stability. To get to >1000 we need:
   - Higher entropy coefficient (currently 0.0208; try 0.05-0.10)
   - Lower clip_coef (currently 0.01 — extremely tight; try 0.1-0.2)
   - Probably a recurrent policy (current 4M-param net likely an MLP; NetHack needs long-horizon memory)
   - Reward shaping (the agent is just earning survival reward; needs incentives to descend stairs)

3. **Multi-buffer scaling** at N=1024+: would need both Cluster AZ resolved AND the fcontext coroutine-resume issue from AW-full's bonus run.

## Files in this experiment

- `n64_curve.csv`, `n256_curve.csv`, `n1024_curve.csv`, `n4096_curve.csv` — per-run sample data
- `n*.out` — full puffer dashboard captures (rich UI; use `tr -d '\033\r'` to read)
- `n*.out.exit` — exit codes
- `n*.pid` — PIDs (post-mortem only)
- `run_n.sh` — launcher
- `sampler.sh` — 30s-interval CSV writer
