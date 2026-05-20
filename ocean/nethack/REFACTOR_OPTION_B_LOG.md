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
