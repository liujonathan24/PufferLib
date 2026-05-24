# exp_038 — 8-hour goal push: 1024+ envs @ 1M+ SPS, no crashes

**Start**: 2026-05-24 04:27 EDT
**Budget**: 8h
**Baseline**: 51K SPS @ N=1024 (puffer), N=1024 crashes ~25s in with short-read panic.

## Iter-1: baseline + hypothesis discovery

### Baseline measurements (60s puffer training, 1 buffer, 1 vec.threads)
| N    | Puffer SPS | train_bench serial SPS | gap | crash |
|------|------------|------------------------|-----|-------|
| 64   | 56-67K     | 425K (6.6K/env)        | 7×  | EXIT=139 (SEGV), 1× DEF_MREAD_SHORT |
| 128  | 34-39K     |                        |     | EXIT=139 |
| 256  | 42-44K     |                        |     | EXIT=134 (panic), expected=459008 (sizeof level?) |
| 512  | 37-40K     |                        |     | EXIT=134 |
| 1024 | (running)  | 463K (452/env)         | 9×  | EXIT=134 from prior runs |

**Key insight #1**: train_bench scales nearly flat (425K → 463K from N=64 → N=1024). Puffer barely changes (60K → 51K). Whatever's hurting puffer is **constant overhead** independent of N — most likely OMP barrier or shared-cache thrash.

**Key insight #2**: short-read happens at all N≥64, not just N=1024. The bug is endemic, not scale-related. Earlier hypothesis (writer buffer race) is suspect because saveprocs already migrated.

**Key insight #3**: per-env in train_bench DOES collapse: 1.9M @ N=1 → 452 @ N=1024 (4200× slower per env). This points to:
- L3-cache thrashing as working set blows past cache (expected: ~1 MB / env × 1024 = 1 GB)
- OR remaining shared mutable globals causing coherence ping-pong (a single shared cache line bouncing across 128 cores is ~100ns/access; at 1M env steps/sec / env, that's 100ms/sec wasted per env)

## Iter-2 in progress (3 agents dispatched, parallel)
- **Agent A**: Exhaustive audit of save/restore process-globals + filename buffer collisions; static_assert sizeof(eshk) across TUs. Target: kill the short-read panic.
- **Agent B**: Audit unmigrated mutable file-scope statics on the hot stepping path (allmain/monmove/dogmove/hack/mon/light/timeout). Target: identify the false-sharing candidates.
- **Agent C**: Perf-record a 30s N=128 puffer training run, find dominant CPU cost. Target: data-driven optimization priority.

## Hypotheses (iter-1 status)
- H-S1 (OMP scaling): **strong** — train_bench scales, puffer doesn't.
- H-S2 (per-env arena cold pages): **deprioritized** — would affect init only, not steady state.
- H-S3 (ptrbuf process-global): **FALSE alarm** — only used in debug/impossible() paths.
- H-CRASH (saveprocs migration): **partial** — bw_FILE/saveprocs/restoreprocs are migrated, but short-reads still happen → there's another shared write path.

## Iter-3 candidates (post-agents)
- If Agent B finds hot shared globals → fanout migrations.
- If Agent A identifies short-read cause → fix.
- If Agent C finds non-obvious hotspots → targeted optimizations.

## Commits this iteration
(none yet — investigative phase)
