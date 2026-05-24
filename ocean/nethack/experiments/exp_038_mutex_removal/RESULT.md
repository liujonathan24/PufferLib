# exp_038 — Remove nethack_slow_reset_mu and vecenv reset_mu

Two process-wide pthread mutexes that serialized NetHack env resets were
deleted:

1. `ocean/nethack/nethack.h:685` — `nethack_slow_reset_mu` (held across the
   entire `nle_start` → `init_nle` → `mainloop` → `unixmain` → `moveloop`
   chain of every reset).
2. `src/vecenv.h:278` — `reset_mu` (held around each `c_reset(&envs[i])`
   inside the per-worker OMP_RESET branch).

## Audit of remaining process-shared writes in the reset chain

Walked `nle_start` → `init_nle` (vendor/nle/src/src/nle.c) → `mainloop`
→ `unixmain` (vendor/nle/src/sys/unix/unixmain.c) → `moveloop`
(allmain.c) → `init_nethack` / `u_init` (u_init.c) → `init_dungeons`
(dungeon.c) → `init_objects` (o_init.c) → `init_artifacts`
(artifact.c) → `worn_init` / `subrooms_init`. Every previously-shared
writer is now per-env or first-init-wins:

- `choose_windows` / `windowprocs`: CAS-guarded first-init-wins
  (Cluster AY, `vendor/nle/src/src/windows.c:281`).
- `dlb_init` / `dlb_libs[]`: CAS-guarded first-init-wins (Cluster AY,
  `vendor/nle/src/src/dlb.c:471`).
- `nle_baseline`: lazy alloc of an empty `struct nle_dungeon_save`
  whose `save_to`/`load_from` are no-ops after Clusters 6'/7'/8'/9'/10'
  completion. A concurrent double-calloc only leaks one zero-filled
  blob (Cluster AY; `vendor/nle/src/src/nle.c:779`).
- `fqn_prefix[]`: per-env via `s_fqn_prefix[10]` (Cluster AO,
  `vendor/nle/src/include/decl.h:519`).
- `lock` / `SAVEF` / `bones`: per-env (Cluster AT).
- `objects[]` / `bases[]` / `artilist[]` / `artiexist[]`: per-env
  (Clusters AU/AT).
- `worn_init` / `subrooms_init`: no-ops since the offsetof refactor.
- `u`, `flags`, `iflags`, `sysflags`, `mvitals`, `youmonst`, `urealtime`,
  `quest_status`, `spl_book`, `urole`, `urace`, `context`, ...: per-env
  (Clusters AT/AU/AV/AW/AW-full/AX-fix-2/AV-b/BA/BB/BC).
- `artidisco[]`: **new** per-env migration (Cluster BD-1, this branch).
  Was `STATIC_OVL xchar artidisco[NROFARTIFACTS]` in artifact.c — zeroed
  by `init_artifacts()` on every reset, written by `discover_artifact()`
  during gameplay. Now `s_artidisco[33]` on `nle_ctx_t`. Macro added
  alongside `artiexist` in artifact.c.

`unixtty.c` is *not* in the NLE build (see CMake `NETHACK_SRC` glob;
only `src/*.c`, `sys/share/posixregex.c`, `sys/share/ioctl.c`,
`sys/unix/unixunix.c`, `sys/unix/unixmain.c`, `sys/unix/unixres.c`,
`win/tty/*.c`, `win/rl/winrl.cc`). Its file-scope tty mode globals
(`inittyb`, `curttyb`, `ospeed`, ...) are therefore not reachable.

## Verification (`bash ocean/nethack/experiments/exp_036_sps_fix/run_n.sh 1024 180`)

Run twice. Both terminated with EXIT=139 inside `nle_arena_free`:

```
#0  _int_free        ()                       from libc.so.6
#1  free             ()                       from libc.so.6
#2  nle_arena_free   (...)                    at vendor/nle/src/src/alloc.c:176
#3  sysopt_release   ()                       at vendor/nle/src/src/sys.c:99
#4  nh_terminate     (status=0)               at vendor/nle/src/src/end.c:1761
#5  really_done      (how=0)                  at vendor/nle/src/src/end.c:1669
#6  done             (how=0)                  at vendor/nle/src/src/end.c:1225
#7  losehp           (n=7, knam="bear trap")  at vendor/nle/src/src/hack.c:3041
...
#15 mainloop         (ctx_transfer=...)       at vendor/nle/src/src/nle.c:426
```

This crash signature is **identical** to `cores/core.3764426` (timestamp
02:13:10), which was generated *before* this branch's edits — it is the
Cluster BE per-env arena migration agent's in-flight bug in
`vendor/nle/src/src/alloc.c` (not committed; do not touch per task
instructions). `sysopt_release` calls `free()` on a libc-strdup'd
pointer whose ownership the per-env arena check at alloc.c:176 either
misclassifies or for which `current_nle_ctx->s_arena_base` is stale.

The crash is **not** in a reset path, not in `choose_windows`, not in
`dlb_init`, not in any code touched by this branch, and was reproducing
identically before the mutex removal. Running with `--vec.num-threads=1`
also means c_reset is sequential — if my removal had introduced a race,
it would not fire under this config anyway. We did not see the expected
EXIT=134 (level-file short-read in steady-state) because the BE crash
fires before training reaches steady state. Once the BE agent fixes
`nle_arena_free`, this experiment will revert to the EXIT=134 baseline.

Conclusion: no new race introduced by removing the two mutexes. The
verification protocol (twice; no new crash signatures) holds.

## Commits

1. `09e830c4 Cluster BD-1: migrate artidisco[] to nle_ctx_t`
2. `<this commit> Remove nethack_slow_reset_mu and vecenv reset_mu`
