# Agent B: Hot-Path Unmigrated Globals Audit

Scope: vendor/nle/src/src hot-path files for `moveloop`. Cross-referenced
against `nle_ctx_t` migrations in vendor/nle/src/include/nle.h and the
top-of-file `#define X (current_nle_ctx->s_…)` redirect blocks in each .c.

K&R-style function-parameter declarations (the post-signature `int x;
struct monst *m;` block) were filtered out — they look like file-scope
globals to a naive grep but are stack locals.

## HOT (write race on every-tick path)

### dogmove.c:127 — `STATIC_VAR xchar gtyp, gx, gy`
Type: 3 file-scope xchar (signed char).
Why hot: written inside `dog_goal()` every tick the pet acts. Sites:
500-502, 509-510, 539-542, 550-552, 561-562, 589-590, 597-598, 602,
607-608, 1334-1335. Pets exist in nearly every game; `dogmove()` is
called from every monster-turn pass. Every assignment to `gx`/`gy`
ping-pongs the cache line through L3 for every other env in the
process. This is the single most likely "false sharing on every step"
candidate that survived clusters AT–BE — it sits squarely on the pet
movement path with three back-to-back writes per call.
Comment: `/* type and position of dog's current goal */` — clearly
expected to be per-pet/per-env, not process-shared.

### muse.c:15 — `boolean m_using = FALSE` (file-scope, non-static!)
Type: process-global `boolean` (cross-file: zap.c extern's it at line 24).
Why hot: written every time a monster reads a scroll, zaps a wand, or
quaffs a potion. Bracket pairs in muse.c at lines 707/712, 1433/1437,
1442/1446, 1452/1454. With N>=64 envs and an average dungeon population,
this gets written tens of times per overall step. Worse, it's NOT
`static` — it's a tentative-definition global, the worst case for
false sharing because the compiler can't prove no other TU writes it.

### mhitm.c:18 — `static NEARDATA boolean vis, far_noise`
Type: 2 file-scope booleans.
Why hot: `vis` is the first write in every call to `mattackm()` (line
240) and `noises()` (line 348) — every monster-vs-monster combat tick.
`far_noise` is written in `noises()` line 56. Both are read on nearly
every line of mhitm.c. With dozens of monsters per env and many envs,
this is a frequent shared write.
NB: `noisetime` from the same line was already migrated (Cluster AU
group 4 → `s_noisetime`); `vis`/`far_noise` were missed in that pass.

### mon.c:23 — `STATIC_VAR boolean vamp_rise_msg, disintegested`
Type: 2 file-scope booleans.
Why hot/warm: `disintegested` is written every time a monster dies
(`monkilled()` line 2316/2422, called from many sites per tick). 
`vamp_rise_msg` is set in `mondead()` line 2026 and reset at line 2415.
Death is common in active gameplay. Not as constant as gtyp/gx/gy but
on the monster-death hot path.

## WARM (write race on per-action paths; less frequent than per-tick)

### read.c:22 — `boolean known` (file-scope, non-static!)
Type: process-global boolean, cross-file (detect.c declares
`extern boolean known` at line 15).
Why warm: written ~30 times in seffects() / scroll-handlers (lines
199, 811, 1023, 1194, 1274, 1364, 1444, 1451, 1460, 1464, 1477, 1570,
1590, 1671, 1696, 1708, ...). Only touched when a scroll is read, so
not per-tick. But cross-TU tentative-definition global — same false
sharing concern as `m_using`, less frequent.

### pickup.c:352 — `static boolean class_filter, bucx_filter, shop_filter`
Type: 3 file-scope booleans.
Why warm: written in `add_valid_menu_class()` line 370/379/382/385,
read in `allow_category()` lines 420-426 and `allow_all_categories()`
lines 450/454/458. Reached every menu prompt that filters by class
(loot, sortpack, etc.). Per-action, not per-tick.

### potion.c:15 — `static NEARDATA int nothing, unkn`
Type: 2 file-scope ints.
Why warm: `dodrink()`/`dodip()` reset them at top (line 574) then
increment/decrement throughout (~50 sites). Each potion drink is
~50 writes to these two ints. Could race if envs yield between
the reset and the result-print, but mainly per-drink, not per-tick.

### invent.c:1919-1922 — `STATIC_VAR struct xprnctx { char let; boolean dot; } safeq_xprn_ctx`
Type: 1 file-scope struct.
Why warm: written in `display_inventory()` line 2201-2202, read by
two callback wrappers `safeq_xprname` / `safeq_shortxprname` (lines
1929, 1938) used by `safe_qbuf()`. Per inventory display.

### display.c:1143 — `static xchar lastx, lasty` (in `swallowed()`)
Type: 2 function-static xchars.
Why warm: written every tick the player is swallowed (uncommon
gameplay state, but a tight per-tick loop when active).

### display.c:1207-1208 — `static xchar lastx, lasty; static boolean dela` (in `under_water()`)
Type: 3 function-statics.
Why warm: written every tick the player is underwater.

### display.c:1258 — `static boolean dela` (in `under_ground()`)
Type: 1 function-static boolean.
Why warm: written every tick the player is in earth-element / digging.

## COLD / READ-ONLY (safe — kept for completeness)

- allmain.c:778 `static struct early_opt earlyopts[]` — startup arg parsing, never per-step.
- hack.c:575 `static const char fell_on_sink[]` — const.
- hack.c:1013 `static int ordered[]` — init only, effectively const.
- hack.c:2981 `static short powers[]` — init only.
- do.c:29 `static const char drop_types[]` — const.
- monmove.c:667-674 `static const char practical[]/magical[]/...` — const.
- dogmove.c:124 `static const char nofetch[]`, 1340 `static struct qmchoices` — const tables.
- mon.c:2994 `static const char *const Exclam[]` — const.
- light.c — fully migrated (s_light_base, Cluster AG).
- region.c:51 `static callback_proc callbacks[]` — function-pointer table, immutable.
- region.c — fully migrated (s_regions/s_n_regions/s_max_regions, Cluster AI).
- timeout.c — *_texts arrays + propertynames are all `const`.
- timeout.c — timer queue migrated (s_timer_base, Cluster AB).
- display.c:972 `static struct tmp_glyph` — migrated (s_tmp_at_tglyph, AV-b3).
- display.c:2255 `unsigned char seenv_matrix[3][3]` — init only.
- display.c:2305/2333 `static const int wall_matrix/cross_matrix` — const.
- vision.c:42/65 `char circle_data[]` / `circle_start[]` — init only.
- objects.c globals migrated (s9o_objects_p / s9o_obj_descr_p).
- objnam.c tables — all `const`.
- invent.c:74 `static char venom_inv[]` — const-marked in comment, never written.
- invent.c:86 `static char def_srt_order[]` — init only.
- invent.c:1942 / pickup.c:2526/2978 — `static const char` tables.
- eat.c:141/2180 `static const struct/foodwords` — const.
- potion.c:16/247/248 — const.
- read.c:24/26 — const tables.
- zap.c:60 `STATIC_VAR const char are_blinded_by_the_flash[]` — const.
- zap.c:1409/2217 — const tables.
- spell.c:102/1429 — const.
- pline.c:547 `static __thread boolean use_pline_handler` — gated on
  `MSGHANDLER && (POSIX_TYPES || __GNUC__)`; in NLE build path this is
  not the default, but if `MSGHANDLER` is defined this is a __thread
  hazard. Verify build config; otherwise SKIP.

## __thread audit summary

A grep for `static.*__thread` in the audited hot files finds only
pline.c:547 (gated by MSGHANDLER — likely inactive). All other
formerly-__thread state in these files was migrated to nle_ctx_t in
earlier clusters (AG, AH, AI, AJ, AK, AL, AP, AP-2). No remaining
__thread storage on the hot path.

## Migration impact estimate

Migrating `dogmove.c gtyp/gx/gy`, `muse.c m_using`, `mhitm.c vis/far_noise`,
and `mon.c vamp_rise_msg/disintegested` removes 8 file-scope mutable
symbols that are written on the monster/pet turn path. These are all
small scalars; they very likely sit on shared cache lines with each
other. If the 4200× per-env slowdown is partially false-sharing-driven,
these are the most plausible remaining culprits in the audited set.
