# Agent A Report: short-read investigation, exp_038

## Scope
Investigate the N>=64 `DEF_MREAD_SHORT` panic in libnethack level files
per the brief in exp_038. Specifically: (1) audit non-migrated mutable
globals in save/restore/level-build files; (2) add sizeof static_asserts
to test Hypothesis 1 from exp_037 REPORT.md; (3) one concrete fix.

## What I found

### Hypothesis 1 (sizeof asymmetry across TUs): **RULED OUT**
Added compile-time probes (`char probe[sizeof(struct X)] = {0};`) to
both `save.c` and `restore.c`, then read symbol sizes from the .o files
via `readelf -s`. Result:

| Struct        | save.c | restore.c |
| ------------- | ------ | --------- |
| `struct eshk` | 4936   | 4936      |
| `struct monst`|  144   |  144      |
| `struct obj`  |   96   |   96      |

`_Static_assert(sizeof(struct eshk) == 4936)` and matching asserts for
monst/obj are now present in BOTH save.c (`vendor/nle/src/src/save.c:38-49`)
and restore.c (`vendor/nle/src/src/restore.c:102-108`). They pass clean.
The two TUs see the same struct layout — sizeof drift is not the bug.

**Gotcha worth recording**: my first attempt put the asserts inside
`#ifdef ZEROCOMP` in save.c. ZEROCOMP is NOT defined on this build
(verified with `cc -dM -E`), so the asserts silently compiled out.
The asserts now live above the ZEROCOMP block.

### Globals audit
Five file-scope mutable globals in the audited set are NOT migrated to
`nle_ctx_t` (full table in `agent_a_globals.md`):
- `sp_lev.c:217-218` xstart/ystart/xsize/ysize (level geometry)
- `sp_lev.c:220-222` **non-static** lev_message/lregions/num_lregions
- `mkmaze.c:1410` bbubbles/ebubbles
- `mkmaze.c:1413` xmin/ymin/xmax/ymax (water-level)
- `files.c:3269` chosen_symset_start/end (config parse only)

None of these is on the `restmonchn → restmon → mread(eshk)` chain that
produces the panic. They're real races (level-build / config-load) but
cannot truncate an already-flushed level file. The non-static
`lev_message`/`lregions`/`num_lregions` triple at `sp_lev.c:220-222` is
the worst — process-shared cross-TU mutable globals — but its corruption
mode is a wrong level layout, not a short level-file.

### One concrete attempt: verify behavior at N=64
Rebuilt with the asserts only, ran the 60s N=64 smoke:
`bash ocean/nethack/experiments/exp_038_goal_8h/run_sps.sh 64 60 agent_a_fix`.
Result: short-read **still reproduces** within ~30s:
```
DEF_MREAD_SHORT pid=4177154 hackdir=/tmp/nle-fQ6D9f/ fd=49
                pos=17443 size=17443 expected=216 got=69 errno=2
```
This time `expected=216`, not 4936 — so it's a `mread(name_buf, buflen=216)`
mid-monst-chain (variable-length string), not the eshk record. `pos==size`
again: the file is exactly the length the writer claimed; reader expected
more bytes than were written.

## Conclusion
- sizeof drift is **NOT** the cause (Hypothesis 1 falsified).
- The audited globals don't explain it either.
- The bug remains: the level file is consistently 200-1800 bytes
  short of what the reader expects, but the writer reports a clean
  flush/sync/close. exp_037 already proved the buffered-write chain is
  honest end-to-end. Something between `def_bwrite` returning success
  and the file's final on-disk size is dropping a stream tail —
  candidates left: (Hyp 2) wild ESHK / variable-length-name pointer
  causing fwrite to short-copy via SIGSEGV-in-coroutine, or (Hyp 3) a
  second open+truncate of the same path between save and read (no
  ftruncate calls exist; would have to be a re-`creat` of the level
  file from another env's path — but `mkdtemp` per-env makes that
  near-impossible).
- The next high-confidence step is to instrument `def_bclose` to
  fstat the fd just before fclose and log `pos` + `final size`, and to
  log every `create_levelfile`/`open(O_TRUNC)` against the level path
  with `pid+hackdir+ledger`. If any create-with-truncate fires
  post-save against the same path, that's the smoking gun.

## Artifacts
- `vendor/nle/src/src/save.c:38-49` — `_Static_assert` block for
  sizeof(struct eshk/monst/obj), placed OUTSIDE the ZEROCOMP block
  (build flag does not define ZEROCOMP).
- `vendor/nle/src/src/restore.c:102-108` — matching asserts.
- `ocean/nethack/experiments/exp_038_goal_8h/agent_a_globals.md` —
  globals audit table.
- `ocean/nethack/experiments/exp_038_goal_8h/sps_agent_a_fix_n64.err`
  — reproduces the short-read at N=64 with the asserts active
  (confirming the asserts didn't introduce or hide anything).
- `ocean/nethack/experiments/exp_038_goal_8h/run_sps.sh` — recreated
  (the prior copy disappeared from the working dir; rebuilt from the
  brief).
