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

---

## Followup attempt (commit df811867): writer-side close path ruled out

Strengthened the writer side to surface every previously-silent error:

| check                                         | result on N=1024 600s repro |
| --------------------------------------------- | --------------------------- |
| `def_bclose`: check `fclose` retval           | never fired                 |
| `def_bclose`: `fsync(bw_fd)` before `fclose`  | never fired                 |
| `def_bflush`: log on `fflush == EOF`          | never fired                 |
| `def_bwrite`: short-write log (pre-existing)  | never fired                 |
| `def_bufoff`: log when `fd != bw_fd`          | fires ONCE post-panic (fd=50, bw_fd=-1) — savebones/teardown noise, NOT pre-crash |

So with these checks active, the writer believes every byte was
buffered (`fwrite` ok), drained (`fflush` ok), synced (`fsync` ok),
and the fd closed cleanly (`fclose` rc=0). And yet the reader still
hits `DEF_MREAD_SHORT pos=18702 size=18702 expected=4936 got=3100`
on the SAME file. The level file is exactly the writer's claimed
length, and we've now proven the writer's claim is honest.

That means the bug is **NOT** in the buffered-write/close chain.
The data was written and synced; something else is making the file
shorter than expected. Three lines of investigation left:

### Hypothesis 1: `sizeof(struct eshk)` is different between writer and reader

The reader's `mread(fd, ESHK(mtmp), sizeof(struct eshk))` expects 4936.
The writer's `bwrite(fd, ESHK(mtmp), buflen)` writes `buflen = sizeof(struct eshk)`.
Same translation unit, same struct — should be identical. But `struct eshk`
in `vendor/nle/src/include/eshk.h` contains `struct bill_x bill[BILLSZ]`,
inline `char *shknam`, etc. If any sub-struct contains a flex array or a
post-`#pragma pack` change, sizeof can differ across compilation units.

Verify: add `_Static_assert(sizeof(struct eshk) == 4936, ...)` at top of
save.c and restore.c. Also assert `sizeof(struct monst) == X`, since
similar shenanigans there would shift the file layout.

### Hypothesis 2: ESHK pointer aliasing / wild pointer

`ESHK(mtmp)` returns `mtmp->mextra->eshk`. If `mtmp->mextra` was UAF'd
(realloc'd by another env's allocator?), `eshk` could point at an
allocation that is only 3100 bytes mapped. `fwrite(loc, 4936, 1, FILE)`
would call `memcpy(stdio_buf, loc, 4936)` which would SIGSEGV at the
unmapped page boundary. **BUT** if `loc` straddles into a guard page that
is mapped but read-protected, memcpy errors differently. And if it's
mapped-readable but the env's arena gave us a short tail allocation,
we'd write 4936 garbage bytes successfully and the issue would be
content-corruption, not size-truncation. So this hypothesis does NOT
explain `size == 18702 < 23638`.

Wait — actually it might. If `fwrite`'s underlying `_IO_default_xsputn`
path discovers SIGSEGV in the memcpy and the SIGSEGV is caught by the
fcontext stack (NetHack's coroutine), the signal handler might rewind
the stack and resume at a higher call site — leaving the FILE in an
inconsistent state with only 3100 bytes buffered. NLE's fcontext setup
is unusual; check whether SIGSEGV is masked or installed with a custom
handler that prevents normal core dumps. We'd see this as: writer thinks
fwrite succeeded; fflush flushes 3100 bytes; fclose returns 0; file
is 3100 bytes short. Matches the smoking gun.

Concrete test: in `def_bwrite`, before the `fwrite`, write `loc[0]` and
`loc[num-1]` (force the read) and assert no segv. Or run with
`MALLOC_CHECK_=3 LD_PRELOAD=libduma.so` or `valgrind --tool=memcheck`
(slow at N=1024, but N=64 may reproduce).

### Hypothesis 3: The file is being truncated AFTER write

Some path on the writer's env re-opens the level file with `O_TRUNC`
between the save and the matching `goto_level` restore. The level file
path is `<hackdir>/<plname>.<ledger>` and `create_levelfile` always uses
`O_TRUNC`. Search for any second `create_levelfile(ledger)` between the
first close and the read. In particular, `freelev_p`, `dosave0`,
`savebones`, `mklev` — does anything on the path re-open level files
for *write* (not read)?

Concrete test: instrument `create_levelfile` to log every call (path,
fd, pid, hackpid) and grep for the same level number being created
twice without an intervening delete_levelfile + open_levelfile(read).

### Recommendation for next session

Do Hypothesis 1 first (static_assert) — it's a one-line change with a
big payoff if wrong. Then Hypothesis 3 (create_levelfile log). Hypothesis 2
last (most invasive).

Also: budget bigger than 90 minutes — at N=1024 the repro takes 25-60s
to fire and rebuild is ~30s, so each iteration is a 1-2 min cycle. Plan
for 8-10 iterations.
