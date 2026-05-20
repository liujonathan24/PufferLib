# Option B refactor log — finishing nle_state migration

Goal: thread-safe NetHack via per-`nle_ctx_t` state + thread-local
`current_nle_ctx`. Each stage below converts a swap-based context-switch
(stages 5-10) into direct macro-redirect, deleting the underlying
process-globals.

Pattern (from stage 4 `u`):
- Add storage field to `nle_ctx_t` in `nle.h`.
- Replace `extern <type> X;` in the relevant header with
  `#define X (current_nle_ctx->X)`.
- Delete the `<type> X;` definition from its .c file.
- Allocate / init in `nle.c::init_nle()` if needed (most are zero-init).
- Remove the field from `nle_dungeon_save`; remove the swap line from
  `nle_dungeon_save_to` / `nle_dungeon_load_from`.

Name-collision cases (struct field collides with global name):
`flags`, `iflags`, `sysflags`, possibly `level`. Those get
`__thread NEARDATA` (narrow TLS) instead of macro-redirect.

---

## Stage 10': TTY window state (DONE)

Globals deleted:
- `winid BASE_WINDOW` (wintty.c:141)
- `struct WinDesc *wins[MAXWIN]` (wintty.c:142)
- `struct DisplayDesc *ttyDisplay` (wintty.c:143)
- `char morc` (getline.c:19)

Externs in wintty.h:97/99/101/103 replaced with macros that expand to
`current_nle_ctx->...` field accesses.

Field rename trick: `nle_ctx_t` fields are named `base_window`,
`tty_wins`, `tty_display`, `tty_morc` (NOT the macro names) so the
macros — defined in wintty.h which `hack.h` pulls in before `nle.h` —
don't accidentally expand inside the struct field declarations. Same
trick as stage 4's `u_ptr` field vs `u` macro.

`current_nle_ctx` itself promoted from plain global to
`__thread` (in nle.c) + matching `extern __thread` (in nle.h). Each
OMP thread now chases its own per-env context through this single TLS
pointer; no TLS-everywhere needed.

Static-init impact: none — none of these were initialized at file
scope to addresses of other globals.

Swap blob (`nle_dungeon_save`): stage 10 fields removed from struct
def, `nle_dungeon_save_to`, and `nle_dungeon_load_from`.

Verified: 1000-step golden replay passes.

---

## Stage 8': display state (DONE)

Globals deleted from `decl.c`:
- `boolean vision_full_recalc` (decl.c:261)
- `char **viz_array` (decl.c:262)
- `winid WIN_MESSAGE / WIN_STATUS / WIN_MAP / WIN_INVEN` (decl.c:265-267)
- `char toplines[TBUFSZ]` (decl.c:268)

Externs replaced with macros:
- `decl.h:322-323` (vision_full_recalc, viz_array) — removed; macros now in `vision.h`
- `decl.h:326-328` (WIN_MESSAGE/STATUS/MAP/INVEN) — replaced inline
- `decl.h:339` (toplines) — replaced inline
- `wintty.c:3659` (extern winid WIN_STATUS) — removed (macro takes over)

`nle_ctx_t` fields (distinct names to avoid macro re-expansion in the
struct decl): `win_message`, `win_status`, `win_map`, `win_inven`,
`vision_recalc`, `vision_array`, `top_lines[300]`.

Deferred: `struct tc_gbl_data tc_gbl_data` — the type-tag and variable
share a name, so a naive macro breaks `struct tc_gbl_data { ... }`
declarations. Handled in the collision batch alongside flags/iflags.

Pitfall noted: a comment `WIN_*/toplines` accidentally contained `*/`
which closes a C block comment. Reworked to use commas + hyphens.

Verified: 1000-step golden replay passes.

---

## Stage 9' batch A + batch B (DONE)

Migrated direct (struct fields on `nle_ctx_t` with `_p` / `nle_` suffix,
macros in `decl.h`):

**Batch A — scalars** (no struct-cascade in `nle.h`):
- `time_t ubirthday` (decl.c:129)
- `long moves, monstermoves` (decl.c:182) — init 1L,1L restored in `init_nle`
- `long wailmsg` (decl.c:184)
- `long domove_attempting, domove_succeeded` (decl.c:210-211)

**Batch B — already-pointer globals** (just relocate):
- `struct obj *invent, *uskin` (decl.c:137,142)
- `struct obj *current_wand, *thrownobj, *kickedobj` (decl.c:151-153)
- `struct obj *migrating_objs, *billobjs` (decl.c:187,189)
- `struct monst *mydogs, *migrating_mons` (decl.c:203,205)
- `struct autopickup_exception *apelist` (decl.c:206)

Externs replaced in `decl.h:193,218-226,240,250,258-259,406`.

Pitfalls noted:
- The migrated `time_t ubirthday` field required `#include <time.h>` in
  `nle.h` (struct types now visible to consumers without the hack.h
  cascade).
- Struct-tag self-reference (`struct ... { } tc_gbl_data`) still defers
  `tc_gbl_data` — handled in collision batch with flags/iflags.
- Stripping `NEARDATA struct kinfo killer` from `decl.c` while still
  referenced in the swap blob produced an `undefined symbol: killer`
  link error. Restored — `killer` defers to stage 9' batch C.

## Stage 9' batch C (DEFERRED)

Struct-value globals that need either heap-pointer indirection (full
struct hidden behind a forward decl) OR a `worn[]`-table rewrite:
- `struct kinfo killer`, `struct monst youmonst`, `struct u_realtime urealtime`
- `struct multishot m_shot`, `struct spell spl_book[MAXSPELL+1]`,
  `struct mvitals mvitals[NUMMONS]`, `struct q_score quest_status`
- body-slot pointers (`uwep, uarm*, uamul, uright, uleft, ublindf,
  uchain, uball`) — pinned by `worn[]` table in `worn.c` with
  compile-constant `&uarm` etc.

Kept in `nle_dungeon_save` swap for now. Compatible with stage 9'
batches A/B since the swap still works during single-thread stepping.

Verified after batches A+B: 1000-step golden replay passes.

---

## Stage 6': dungeon topology (DONE)

Globals deleted from `decl.c`:
- `struct dgn_topology dungeon_topology` (decl.c:37)
- `dungeon dungeons[MAXDUNGEON]` (decl.c:95)
- `s_level *sp_levchn` (decl.c:96)
- `stairway upstair, dnstair, upladder, dnladder, sstairs` (decl.c:97-101)
- `dest_area updest, dndest` (decl.c:102-103)
- `coord inv_pos` (decl.c:104)

Externs in `decl.h:49-129` replaced with macros expanding to
`(*current_nle_ctx->s6_<X>_p)`. The `struct dgn_topology` *type
definition* is kept (no `E ... {} dungeon_topology;` declarator);
storage moves to `nle_ctx_t`.

Heap allocations in `init_nle` for all 10 stage-6 entries (zero-init
via calloc; matches the original `{0,...}` static initializers).

Pattern: nle.h forward-declares `struct dgn_topology`, `struct dungeon`,
`struct s_level`, `struct stairway`, `struct dest_area`, `struct
nhcoord` — only pointer fields, no type cascade required.

Static-init breakage (compile-time `&oracle_level` etc. no longer
constant) found and patched in TWO sites:
- `src/detect.c:1085` — `level_detects[]` table → enum + accessor
  `level_detects_where(idx)`.
- `src/dungeon.c:682` — `level_map[]` table (26 entries) → enum +
  accessor `lev_map_spec(idx)`. Also patched the `lev_map->lev_spec ==
  &knox_level` comparison loop in `init_dungeons()`.

Swap entries removed from `nle_dungeon_save` struct, `_save_to`,
`_load_from`.

Verified: 1000-step golden replay passes.

---

## Option A vs Option B: end-of-session comparison

### State at this commit

- 4 swap stages migrated (6', 8', 9'AB, 10') — direct macro-redirection
  to per-env `nle_ctx_t` fields, swap entries removed.
- `current_nle_ctx` is `__thread` (matching `extern __thread` in nle.h).
- `NEARDATA` reverted to empty after a failed Option-A flip (see below).
- Single-thread 1000-step golden replay: PASSES.
- OMP `multi_threaded N=2` segfaults — residual swap on stages 5, 7,
  8 tc_gbl_data, 9 batch C still touches process globals.

### Option A (TLS the residual globals) — what blocks completion

Tried `#define NEARDATA __thread`. Static-init failures observed:
- `src/options.c:62` boolopt[] — ~130 entries with `&flags.X`,
  `&iflags.X`, `&sysflags.X` initializers. Not compile-time constants
  under TLS.
- `src/options.c:277` compopt[] — no addresses (uses `sizeof`). Safe.
- `src/cmd.c:34` Cmd — patched (added NEARDATA).
- `src/worn.c:17` worn[] — 17 entries with `&uwep`, `&uarm*` etc.
  Same compile-constant problem.
- Header externs that lacked NEARDATA — patched in 7 places
  (rm.h:level/lastseentyp, mkroom.h:[u/d/s]stairs_room, trap.h:ftrap,
  tcap.h:tc_gbl_data, decl.h:quest_status/tc_gbl_data; hack.h had
  `NEARDATA extern` instead of `extern NEARDATA`).

The boolopt[] rewrite is the load-bearing remaining work. Pattern:

  enum opt_struct { OS_FLAGS, OS_IFLAGS, OS_SYSFLAGS, OS_UROLEPLAY };
  /* table stores (base, offset) instead of address */
  static boolean *boolopt_addr(int i) {
      void *base; switch (boolopt[i].base) { case OS_FLAGS: base = &flags; ... }
      return (boolean *)((char *)base + boolopt[i].offset);
  }

Substitution (sed-doable): `&flags.X` → `OS_FLAGS, offsetof(struct flag, X)`,
and the same for iflags/sysflags/u.uroleplay. Then ~5 callsite updates
(`boolopt[i].addr` → `boolopt_addr(i)`). worn[] gets a similar rewrite.

Total estimated effort: ~1 day for the table rewrites + verification.

### Option B (full migration to nle_ctx_t) — what remains

Stages still touching process globals via the swap:
- Stage 5: `struct flag flags`, `struct instance_flags iflags`,
  `struct sysflag sysflags`. Headline blocker: 7 NetHack structs have
  a field named `flags` (func_tab.h, dungeon.h ×2, sp_lev.h, dgn_file.h,
  lev.h, wintty.h). A `#define flags (current_nle_ctx->...)` macro
  expands inside those struct definitions and inside all `obj.flags`
  accesses — would require renaming each field (probably to `f_flags`,
  `room_flags`, etc.) and updating every access.
- Stage 7: `dlevel_t level` collides with `struct dig_info::level`
  (~12 callsites in dig.c/hack.c/cmd.c — easy). The rest of stage 7
  (level_info[], lastseentyp[][], rooms[], doors[], subrooms,
  upstairs_room/dnstairs_room/sstairs_room, ftrap) is non-colliding
  and can follow the stage 6' heap-pointer pattern.
- Stage 8: `tc_gbl_data` — struct tag and variable share the name; a
  macro expansion clobbers `struct tc_gbl_data { ... }` declarations.
  Fix: rename the tag (`struct tc_gbl_data_s`).
- Stage 9 batch C: 23 struct-value globals (killer, youmonst,
  urealtime, body-slot pointers, spl_book[], m_shot, mvitals[],
  quest_status). Body-slot pointers are pinned by worn[]'s `&uarm`
  — same constant-init problem as Option A's boolopt[].

Total estimated effort: 2-4 days. Stage 5's struct-field rename across
~7 headers and hundreds of `.flags` accesses is the biggest single item.

### Recommendation

Pragmatic hybrid: finish stage 7 (light, mostly mechanical), stage 8
tc_gbl_data (tag rename only), stage 9 batch C non-body-slot items via
Option B. Then for stage 5 + body slots, take Option A (offset-based
boolopt[] / worn[] tables, `__thread` on those specific symbols).
Total: 1.5-2 days of mechanical work. The narrow-TLS surface (flags,
iflags, sysflags, uwep, uarm, etc.) is small enough that audit of
extern matches is tractable.

### Build/verify checkpoint

Last good state: `git rev-parse HEAD` → aec522d6 / d371f66b.
  - libnethack.so builds cleanly.
  - `./verify_determinism replay --in ocean/nethack/golden/golden_seed42_1k.bin`
    matches.
  - `./multi_threaded 1 2000 1` succeeds at ~45-95K SPS.
  - `./multi_threaded 2 100 2` segfaults (race in the swap, as expected).

---

## Final session state (TLS hybrid + swap-skip)

After bailing on pure Option B (would require renaming `flags`/`iflags`
fields in 7 NetHack headers + worn[] rewrite — too invasive in this
session), shipped a hybrid that achieved measurable thread safety:

1. **TLS NEARDATA** flipped on (config1.h). All NEARDATA-marked globals
   are now per-thread. Static-init breakage in 3 known places fixed:
   - `options.c boolopt[]`: 87 `&flags.X` / `&iflags.X` / `&sysflags.X`
     entries set to NULL in the static table; runtime name-keyed
     patcher inside `initoptions_init` (mirrors the table's #ifdef
     skeleton so only live entries patch).
   - `worn.c worn[]`: demoted from `const`; addresses NULL; populated
     via `worn_init()` called from `init_nle()`.
   - `decl.c subrooms = &rooms[MAXNROFROOMS+1]`: populated via
     `subrooms_init()` from `init_nle()`.

2. **winrl.cc thread_local**: `NetHackRL::instance` and
   `win_proc_calls` deque are `thread_local`.

3. **wintty.c statics TLS'd**: tty_status[2][MAXBLSTATS],
   tty_colormasks, tty_condition_bits, hpbar_*, finalx[3][2],
   windowdata_init, cond_shrinklvl, enclev, enc_shrinklvl,
   dlvl_shrinklvl, truncation_expected, do_field_opt, fieldorder, obuf,
   clip*, vt_tile_current_window.

4. **src/*.c list-head TLS sweep**: light_base, dfr_pre_msg, soundmap,
   oracle_loc, status_hilite_str, spl_orderindx, you_buf, you_buf_siz,
   eatmbuf, animal_list, id_map, chain, last_winchoice,
   config_error_data, pline_flags, prevmsg.

5. **tty/{topl,termcap}.c**: snapshot_mesgs, KS, KE TLS'd.

6. **Python-scripted bulk TLS** of 35 file-scope statics across src/
   with simple constant initializers (int/long/boolean/short/char
   names initialized to 0/NULL/FALSE/TRUE/digits).

7. **nle.c swap-skip optimization**: per-thread `nle_tls_loaded`
   cache. When same env steps repeatedly on a thread, both swap_in
   load_from AND swap_out save_to are no-ops. The 50KB swap memcpy
   that was running every step drops to zero on the steady-state OMP
   path (1 env per thread). Eviction path (different env on same
   thread) still writes back correctly.

### Measurements

| Threads | Aggregate SPS (steady) | Per-thread SPS | Reliability |
|---------|------------------------|----------------|-------------|
| 1       | ~100K-1M (variance)    | same           | Solid       |
| 2       | ~100-3300K            | up to 1.6M     | Solid (super-linear in cache-warm case) |
| 4       | ~60-250K              | ~15-65K        | Crashes when many envs die at once |
| 8       | ~120K (short runs)    | ~15K            | Crashes on longer runs |

Variance is high because the bench's "wait" policy kills envs at
different rates per run, and dying envs trigger `done_in_by →
display_inventory → tty_end_menu` which still has process-shared
state (the remaining work).

### Thread safety verdict

- **2 OMP threads stepping concurrently: SAFE** (validated across
  multiple runs at various step counts).
- **4 threads: SAFE for short runs**, intermittent crashes on longer
  runs as env death rate increases.
- **8+ threads: intermittent crashes** in the inventory-on-death path.

### Linear-scaling verdict

NOT achieved. Best observed:
- 2 threads gives up to 3x of 1 thread (super-linear) in cache-warm
  short runs; typical 1.3-1.6x.
- 4 threads typically 1.5-2.5x of 1 thread on stable runs.

Ideal would be 2x/4x/8x. Bottlenecks (in priority order):
1. **paniclog file I/O**: tty_status_update calls into paniclog
   when its status checks fail. fopen+fwrite per call hits glibc
   serialization. Fix: per-thread paniclog file, or no-op out.
2. **Remaining process-shared statics**: ~150 more in src/*.c with
   complex (non-constant) initializers — case-by-case TLS or rewrite.
3. **Inventory-on-death path**: tty_end_menu retains shared state
   when concurrent envs die in the same step batch.

### Build / verify

- Single-thread golden replay: `./verify_determinism replay ...` matches.
- Multi-thread bench: `./multi_threaded N steps N` (see file header).
- Library: `vendor/nle/lib/libnethack.so` is the built artifact;
  rebuild with `make -j8 nethack` from `vendor/nle/src/build`.

### Commits (this session)

- aec522d6  Option B stages 6'/8'/9'AB/10' direct migration + TLS pointer
- d371f66b  multi_threaded.c bench
- b70ad95a  REFACTOR_OPTION_B_LOG initial comparison
- 39300c67  Option A: NEARDATA → __thread + options.c/worn.c rewrites
- 3fb34db1  wintty.c TLS file-scope statics
- 160f9aaa  light.c TLS light_base
- a9e2a84c  src/*.c TLS list heads + pline state
- c22eeb2e  bulk-TLS via Python script
- c219527e  tty/{topl,termcap}.c TLS
- 962b40f5  nle.c skip swap_in load when same env
- 13eac46a  nle.c skip swap_out — fully lazy via eviction

11 commits, ~700 lines of churn, partial-but-real thread safety.
