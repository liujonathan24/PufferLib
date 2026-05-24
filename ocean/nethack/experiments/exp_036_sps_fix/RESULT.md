# exp_036 — SPS fix + N=1024 abort root-cause capture

## Setup

- libnethack rebuilt as `RelWithDebInfo` (`-O2 -g -DNDEBUG`) instead of
  `Debug` (`-g`, no `-O`).
- `vendor/nle/src/win/rl/winrl.cc`: `WinProcDeque` switched from
  `LibcDeque<LibcString>` to `LibcDeque<const char *>`. All 38 ScopedStack
  call sites pass string literals; nothing reads the contents (pure
  scope-tracking deque), so no LibcString construction per push.
- Launcher: `run_n.sh` with `ulimit -c unlimited`, `cd cores/` before
  launching `puffer train`, absolute NETHACKDIR.

## SPS — fixed (2x at N=1024)

| Run             | First 30 s | Steady (60-120 s) | Notes               |
| --------------- | ---------- | ----------------- | ------------------- |
| exp_031 pre-AZ  | 31.5 K     | 26.5 K            |                     |
| exp_035 post-AZ | 26.4 K     | 22.3 K            | regression          |
| **exp_036 fix** | **31.4 K** | **49–50 K**       | **best ever at N=1024** |

Curve is `n1024.out`. After 20 s, SPS climbs to a sustained 48–50 K. That's
~60 % above the pre-AZ baseline; the `-O2` rebuild is the bigger
contributor (the BA TLS-macros were the actual cost — `ScopedStack` was
also helping by avoiding per-call SSO-overflow malloc).

## Abort — STILL reproduces (caught a core this time)

Run aborted at ~76 s with `EXIT=134`. Core captured at
`cores/core.3396943` (1.9 GB, on `/scratch`).

### Root cause: short read on level file

Aborting thread (LWP 3397247, one of the OMP step workers running a
coroutine):

```
#0  __pthread_kill_implementation
#1  raise
#2  abort
#3  NH_abort                        end.c:224
#4  panic ("Error reading level file.")
                                    end.c:605
#5  def_mread (fd=51, ...)          restore.c:1660   ← short read inside panic handler
#6  restobj                         restore.c:237
#7  restobjchn                      restore.c:286
#8  restmonchn                      restore.c:452
#9  getlev (fd=51)                  restore.c:1106
#10 dosave0                         save.c:274        ← panic handler tries to save
#11 panic ("Error reading level file.")
                                    end.c:648         ← FIRST panic
#12 def_mread (fd=49, ...)          restore.c:1660   ← short read on level file
#13 restmon (fd=49, mtmp=…)         restore.c:377   ← carried monster
#14 restmon (mtmp=…, fd=49)         restore.c:370
#15 restobj                         restore.c:245
#16 restobjchn                      restore.c:286
#17 restmonchn                      restore.c:452
#18 getlev (fd=49, lev=1)           restore.c:1106
#19 goto_level                      do.c:1468
#20 prev_level                      dungeon.c:1246
#21 doup ()                         do.c:1148        ← player walked upstairs
#22 rhack                           cmd.c:4952
#23 moveloop                        allmain.c:446
#24 unixmain                        unixmain.c:355
#25 mainloop                        nle.c:410
#26 make_fcontext                                     ← boost::context coroutine entry
```

Two nested `def_mread` short-reads:

- **First failure (frame 12)**: `read(fd=49, …)` returned fewer bytes
  than requested while restoring a carried monster (`restmon` inside
  `restobj` inside the object chain of level 1). This is on the env's
  *own* level file at the per-env `/tmp/nle-XXXXXX/lock.1` (each env has
  its own mkdtemp'd vardir; `fqn_prefix[LEVELPREFIX] = s->hackdir` is
  per-`nle_ctx_t`).
- **Second failure (frame 5)**: the panic handler (`end.c:648`) invokes
  `dosave0`, which itself reads back level files; that read on fd=51
  also short-reads. So *both* of this env's serialized levels are
  truncated/corrupt on disk.

### Why this is per-env, not config

- Each env has its own `mkdtemp("/tmp/nle-XXXXXX")` vardir. 1024 envs
  with 6 random base62 chars → collision probability ~1e-5 per run.
  Path collision is not zero but is rare.
- `lock`, `hackpid`, `fqn_prefix[]` are all `nle_ctx_t` fields. No
  shared global writes the path string.
- The crash is on a deep coroutine, mid-game (player at level 1 going
  up), not at startup. So it's not the REDIAG NETHACKDIR config bug.
- 3 of 4 N=1024 runs survived 3–5 min without this crash, then this
  4th run hit it at ~76 s. **Looks like a sub-1 % per-step rate** —
  consistent with a low-probability race.

### Plausible mechanisms (ranked)

1. **Process-global state in the save/restore path.** `restore.c`
   uses a `restoreprocs` struct (line 1650: `restoreprocs.mread_flags`)
   that may not be per-env. If env A is mid-restore and `mread_flags`
   gets set to `1` ("return anyway") by env B's concurrent restore,
   env A silently short-reads instead of panicking, *then later
   re-reads a misaligned offset*, producing corrupt restored data.
   But here the panic DID fire, so this isn't the silent path. Still
   worth auditing whether `restoreprocs` is shared.

2. **fd collision via process-global FD table.** If env A opens
   level file → gets fd=49 → seeks/reads → yields the coroutine
   (e.g. via `rl_yn_function`) → env B runs → env B closes some fd,
   opens its own level file, gets fd=49 (kernel reuses lowest), reads
   → coroutine returns to env A → env A continues using fd=49 which
   now points at env B's file. The "short read" is then because env
   A's expected offset is past env B's file end. **This is the most
   architecturally consistent with the symptoms.** Audit: do
   savelev/getlev hold fds across yield-points?

3. **Disk-side truncation from a previous crash.** Previous training
   runs aborted with cores left in `cores/`; if any env was killed
   mid-`savelev` and the same tempdir gets re-used (it shouldn't,
   mkdtemp creates fresh), we'd see truncated files. Unlikely given
   fresh dirs per env.

4. **`/tmp` full.** No — `n1024.err` is empty except the benign
   `NETHACK_USE_GDB` warning. ENOSPC would print a clear error.

5. **Memory corruption upstream of the disk write** (e.g. savelev
   writing junk values for `mtmp->mextra` flags, causing restmon to
   try to read sub-records that aren't there). Could not rule out
   from the bt alone.

### Next investigation

- `grep -n "restoreprocs" vendor/nle/src/include/*.h vendor/nle/src/src/restore.c` —
  is it `static` / per-env?
- Search for `open(fq_lock, …)` and any `close(fd)`+`open` pattern
  inside coroutine-yieldable code paths.
- Diff `lock`, `hackpid`, `SAVEF`, `fqn_prefix[]`, `currentlevel.dlevel`,
  `level_info[]` for "process-global" vs "per-env".
- Instrument `def_mread` to print `(hackpid, lev, expected_len, got_len)`
  on short read — when this fires next we'll see if the file path was
  ever shared.

### Confidence

MEDIUM-LOW that #2 is the cause; HIGH that the crash is a "wrong bytes
read from disk" event (the bt is unambiguous).

## Files

- `n1024.out`, `n1024.err`, `n1024.out.exit`
- `cores/core.3396943` (1.9 GB — on /scratch, can be kept)
- `run_n.sh`
