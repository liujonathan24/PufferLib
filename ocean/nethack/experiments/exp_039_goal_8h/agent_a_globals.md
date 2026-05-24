# Agent A: Globals Audit in save/restore/level-build files

Scope: file-scope mutable statics & non-static globals in
`vendor/nle/src/src/{save,restore,bones,dungeon,files,topten,mklev,mkmaze,sp_lev,nle_fast_reset}.c`.
Cross-referenced against `nle_ctx_t` in `vendor/nle/src/include/nle.h`.

Methodology: line-by-line scan of each file for `^static`, `^[A-Z][a-z_]*…=…;`
top-of-file definitions; awk pass with crude brace-depth tracking; manual
cross-reference of each find with extern decls in headers and the existing
`#define X (current_nle_ctx->s_X)` redirect blocks at the top of each .c.

K&R-style function-parameter declarators (post-signature `int x;\nstruct y *p;`)
filtered out — they look like file-scope vars to naive grep but are stack
locals.

## Findings (UNMIGRATED file-scope mutable globals)

### sp_lev.c:217-218 — `static NEARDATA xchar xstart, ystart; static NEARDATA char xsize, ysize;`
Type: 4 file-scope signed chars.
On save/restore chain? **NO** — used only during `load_special` / mkmaze
construction, never written by save.c/restore.c.
Race risk: HIGH for level-gen correctness under N>=64 (cross-env yield
through pline mid-build), but does NOT explain DEF_MREAD_SHORT.

### sp_lev.c:220-222 — `char *lev_message = 0; lev_region *lregions = 0; int num_lregions = 0;`
Type: **NON-static** file-scope mutable globals (tentative-definition).
Cross-file references: mkmaze.c declares `extern lev_region *lregions; extern int num_lregions;`
and frees them at mkmaze.c:648-650.
On save/restore chain? **NO** — set at level-load time, freed when
the level is realized. Race risk: HIGH under N>=64 (env A's pending
level-region table gets freed by env B's level realization),
but does NOT explain the eshk short-read directly.

### mkmaze.c:1410 — `static struct bubble *bbubbles, *ebubbles;`
Type: 2 file-scope pointers (head & tail of water-bubble list).
On save/restore chain? **NO** — water-level state only.
Race risk: HIGH on water/air levels under N>=64.

### mkmaze.c:1413 — `static int xmin, ymin, xmax, ymax;`
Type: 4 file-scope ints (water-level bounding box).
Same as bbubbles — water/air level only.

### save.c:34 — `#ifdef MFLOPPY long bytes_counted;`
Inactive on UNIX (MFLOPPY not defined). Not relevant.

### nle_fast_reset.c:63-65 — arena tracking (line 63 has `static __thread`)
Already wrapped in `#ifndef NLE_USE_ARENA_FREE` and we have
`-DNLE_USE_ARENA_FREE=1` — these `static` lines are inactive.

### files.c:3269 — `static boolean chosen_symset_start, chosen_symset_end`
Type: 2 file-scope booleans for symbol-set parsing state.
On save/restore chain? **NO** — parsed only at config-load time
(once per env at init); never touched on the eshk read path.

### restore.c:399, 1288, 1313 — three `static const` (zerotrap, zerofruit, havestate)
- Two `static const`: immutable, no race.
- `static boolean havestate = TRUE` at line 399 (inside `nh_compress`):
  set only on MFLOPPY ins_chkpt path; inactive on UNIX.

## Summary

No mutable file-scope global from the audited set lies on the
`restmonchn → restmon → mread(struct eshk)` path that explains
the observed short-read. The five candidates above are real races
(level-build paths) but cannot truncate a *level file* that the
writer has already fflushed/fsynced/fclosed cleanly.

Three race candidates I'd nominate for migration next regardless:
1. `sp_lev.c` `lregions`/`num_lregions`/`lev_message` (cross-TU
    non-static globals — worst case for cache contention).
2. `sp_lev.c` `xstart/ystart/xsize/ysize` (level-build geometry).
3. `mkmaze.c` `bbubbles/ebubbles` + `xmin..ymax` (water-level state).

None of the above unblocks the N>=64 short-read; documenting only
per the brief.
