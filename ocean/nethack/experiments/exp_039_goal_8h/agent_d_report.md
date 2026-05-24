# Agent D Report: short-read post-write-truncation hypothesis (exp_039)

## Hypothesis tested
Hypothesis 3 from exp_037 REPORT.md: the level file is truncated AFTER
successful write (some path reopens the level file with O_TRUNC between
save-close and reload-open).

## Instrumentation added
- `vendor/nle/src/src/files.c` `create_levelfile()`: logs every call with
  pid, hackdir, ledger, full path, fd. Every level-file open-for-write
  uses O_TRUNC/creat semantics, so this catches any spurious second
  creat() against the same path.
- `vendor/nle/src/src/files.c` `open_levelfile()`: logs pid, hackdir,
  ledger, path, fd, and the on-disk file size via fstat **at open time**
  (before any read). This is the reader's source-of-truth file size.
- `vendor/nle/src/src/save.c` `def_bclose()`: logs pid, hackdir, fd, and
  the on-disk size via fstat **right before fclose**. This is the
  writer's source-of-truth final size.

## Evidence (N=64, ~30s repro, sps_agent_d_n64.err)
```
CREATE_LEVELFILE pid=32310 hackdir=/tmp/nle-NOODor/ ledger=1 path=/tmp/nle-NOODor/.1 fd=49
DEF_BCLOSE_SIZE  pid=32310 hackdir=/tmp/nle-NOODor/ fd=49 size_before_close=18598
CREATE_LEVELFILE pid=32310 hackdir=/tmp/nle-EKYG6L/ ledger=1 path=/tmp/nle-EKYG6L/.1 fd=49
DEF_BCLOSE_SIZE  pid=32310 hackdir=/tmp/nle-EKYG6L/ fd=49 size_before_close=17443
CREATE_LEVELFILE pid=32310 hackdir=/tmp/nle-EKYG6L/ ledger=2 path=/tmp/nle-EKYG6L/.2 fd=49
DEF_BCLOSE_SIZE  pid=32310 hackdir=/tmp/nle-EKYG6L/ fd=49 size_before_close=17521
OPEN_LEVELFILE   pid=32310 hackdir=/tmp/nle-EKYG6L/ ledger=1 path=/tmp/nle-EKYG6L/.1 fd=49 size=17443
DEF_MREAD_SHORT  pid=32310 hackdir=/tmp/nle-EKYG6L/ fd=49 pos=17443 size=17443 expected=216 got=69
```

## Conclusion
**Hypothesis 3 is REFUTED.** The panicked file `/tmp/nle-EKYG6L/.1` is
created exactly once (`CREATE_LEVELFILE ... ledger=1`). At
`def_bclose` the writer fstats it as **17443** bytes. At
`open_levelfile` the reader fstats it as **17443** bytes — same number.
No second create/truncate touches that path. The DEF_MREAD_SHORT panic
fires at `pos=17443 size=17443`, meaning the reader has consumed every
byte the writer flushed. There is no post-close truncation.

The bug is therefore upstream of stdio: the writer's save loop terminates
at 17443 bytes while the reader's matching restore loop expects more
bytes. Specifically `expected=216 got=69` is a length-prefixed name field
where 216 bytes were promised in the size prefix but only 69 trailing
bytes exist on disk. Three remaining possibilities:

1. The matching `bwrite(MNAME, 216)` call was never made by the writer
   because the save chain terminated early (e.g., `savemonchn` exited the
   loop after writing the buflen prefix but before writing data — could
   be a missing `if (perform_bwrite(mode))` symmetry bug, or `fmon` /
   `mtmp->nmon` walking off a per-env shared pointer).
2. A globally-shared mutable in the save dispatch is corrupting the
   buflen value between `bwrite(&buflen,4)` and `bwrite(MNAME,buflen)`.
3. The reader's loop control (`restmonchn`) reads a stale buflen because
   of a stale `mread` length prefix that doesn't match writer's prefix.

(1) is the architectural fit and matches the "globals audit" remaining
candidates in `agent_a_globals.md` (`sp_lev.c:217-222`, `mkmaze.c`, etc.,
though those are level-build globals, not save/restore).

## Fix
**None applied** — the surfaced evidence rules out Hyp 3 but doesn't
pinpoint the actual writer-loop bug. Hyp 1 (sizeof drift) and Hyp 3
(post-write truncation) are now both off the table. The remaining work
is to instrument `savemonchn` / `restmonchn` to log buflen + count per
iteration, which is a separate ~90 min cycle.

## Commit
Instrumentation only (no fix). Commit hash: see git log immediately
after this report.
