# exp_037 — N=1024 level-file short-read: smoking gun

## Instrumentation hit

`vendor/nle/src/src/restore.c::def_mread` was instrumented to log `(pid,
hackdir, fd, pos, fstat size, expected, got, errno)` on short-read.
`vendor/nle/src/src/save.c::def_bwrite` was instrumented to log on
short-write. Reproducer: `bash run_instr.sh 1024 300`. Aborted at ~25 s
(EXIT=134), captured these two lines BEFORE the panic chain:

```
DEF_MREAD_SHORT pid=3593979 hackdir=/tmp/nle-X1jQJq/ fd=49 pos=18702 size=18702 expected=4936 got=3100 errno=2 (No such file or directory)
DEF_MREAD_SHORT pid=3593979 hackdir=/tmp/nle-X1jQJq/ fd=51 pos=17958 size=17958 expected=4936 got=1854 errno=2 (No such file or directory)
```

Backtrace (both reads):
```
panic("Error reading level file.")  end.c:605
  def_mread fd=49 expected=4936     restore.c:1719
  restmon                            restore.c:400  ← mread(ESHK(mtmp), sizeof(struct eshk))
  restmon (outer)                    restore.c:375
  restmonchn                         restore.c:435
  getlev fd=49 lev=1                 restore.c:1111
  goto_level                         do.c:1468
  prev_level                         dungeon.c:1246
  doup ()                            do.c:1148      ← player walking upstairs
```

## What it means

1. **`pos == size` for both crashes**: the file is *exactly* the length
   the writer produced. **Not externally truncated, not deleted.**
2. **No `DEF_BWRITE_SHORT` ever printed**: the writer never reported a
   failure. `fwrite(loc, num, 1, bw_FILE)` returned 1 for every record
   it tried to write.
3. **`expected = 4936` = `sizeof(struct eshk)`** (shopkeeper extra).
   The crash is at `restore.c:400`, reading the eshk sub-record of a
   carried monster (OMONST — corpse / statue / mimic).
4. **`errno=2 (ENOENT)` is stale** from a prior syscall in the thread
   (read(2) doesn't set errno on partial). Something in this env did a
   stat()/open() of a missing path immediately before the read —
   probably a bones-file probe.

## Symmetric save/restore — but still broken

```c
// save.c:1158-1161 (writer)
buflen = ESHK(mtmp) ? (int) sizeof (struct eshk) : 0;
bwrite(fd, &buflen, sizeof(int));
if (buflen > 0)
    bwrite(fd, ESHK(mtmp), buflen);   /* 4936 bytes if eshk present */

// restore.c:397-400 (reader)
mread(fd, &buflen, sizeof(buflen));
if (buflen > 0) {
    neweshk(mtmp);
    mread(fd, ESHK(mtmp), sizeof(struct eshk));   /* expects 4936 */
}
```

The branches are symmetric, BUT the file is short. So the writer's
`fwrite(ESHK(mtmp), 4936, 1, bw_FILE)` must have returned 1 despite
fewer than 4936 bytes hitting disk. Three ways that can happen:

A. **`bw_FILE` is closed/replaced mid-save.** Buffered stdio writes
   stash bytes in a per-FILE buffer; if the FILE* is replaced (e.g.,
   another env's `def_bopen` reassigns `bw_FILE`) before `fflush`/
   `fclose` fires, the in-buffer bytes are dropped silently. Were
   `bw_FILE`/`bw_fd`/`buffering` ever process-global? Audit them.

B. **`ESHK(mtmp)` is a dangling/wild pointer.** `fwrite` reads 4936
   bytes from `ESHK(mtmp)`; if those bytes straddle an unmapped page,
   you'd get SIGBUS not a partial. If they straddle a partially-mapped
   page that recently shrank, glibc fwrite might cope and write only
   the readable prefix. Unlikely path but possible.

C. **savemon wrote eshk for a monster whose mextra was created but
   eshk was never allocated, and then the in-memory eshk pointer was
   set to something via UAF.** Hard to validate without ASan.

(A) is the architectural fit: `bw_FILE` / `bw_fd` / `buffering` /
`bytes_counted` / `count_only` look like classic process-global statics
in `save.c`. If two envs ever step concurrently and one calls `def_bopen`
on its own savelev fd, the other's mid-save bw_FILE pointer gets
overwritten — the matching `def_bwrite` then `fwrite`s into the wrong
FILE's buffer or returns "1" against a stale handle. Bytes go nowhere.

## Concrete next step

Audit `bw_FILE`, `bw_fd`, `buffering`, `bytes_counted`, `count_only` in
`vendor/nle/src/src/save.c` and migrate any process-global to
`nle_ctx_t`. The migration recipe is identical to clusters BA/BB/BC.

Files to look at first:
- `vendor/nle/src/src/save.c` lines 60-180 (struct dispatch globals)
- search: `grep -n "static.*\<bw_\|static.*buffering\|static.*bytes_counted" vendor/nle/src/src/save.c`

After fix, rebuild and re-run `bash run_instr.sh 1024 600` twice. If
both clean for 10 minutes, the bug is closed.

## Files

- `run_instr.sh` — repro script
- `n1024.out`, `n1024.err`, `n1024.out.exit`
- `cores/core.3593979` (1.9 GB, on /scratch)
- `REPORT.md` (this file)
