# NetHack 3.6.x Globals Audit (for nle_state Refactor)

**Source tree:** `/scratch/gpfs/ZHUANGL/jl0796/PufferLib/vendor/nle/src/` (NLE 0.9.1 / NetHack 3.6).
**Goal:** Inventory every piece of process-wide mutable state so that all globals can be moved into a single `struct nle_state*` threaded through every function, replacing the current `dlopen`-per-instance approach.

Sources inspected (with line counts):
- `src/decl.c` (365) — the bulk of the globals
- `src/allmain.c` (932) — very few file-scope vars; mostly functions
- `src/dungeon.c` (3111) — dungeon graph state + helpers
- `src/rnd.c` (242) — wraps ISAAC64 RNG
- `src/nle.c` (659) — NLE wrapper, `nle_ctx_t`, `settings`, `nle_seeds_init`
- `include/decl.h`, `extern.h`, `flag.h`, `dungeon.h`, `context.h`, `you.h`, `rm.h`
- Spot-checks of `vision.c`, `light.c`, `bones.c`, `timeout.c`, `worm.c`, `save.c`, `restore.c`, `options.c`, `pline.c`, `invent.c`, `muse.c`, `mklev.c`, `artifact.c`, `files.c`, `topten.c`, `objnam.c`, `hacklib.c`

(No `rndseed.c` or `flag.c` exist in this 3.6.x tree — the `flags`/`iflags`/`sysflags` definitions live in `decl.c` (line 136) and `options.c` (lines 10–14), and all RNG state is inside the `rnglist[]` array in `rnd.c`.)

---

## Executive Summary

NetHack stores essentially the entire running game in C globals. Approximate counts of distinct top-level state objects (skipping pure const tables):

| Subsystem | Approx # of named globals | Bulk of bytes lives in |
|---|---|---|
| RNG | 1 (`rnglist[2]`) + 1 (`nle_seeds[2]`) + 1 (`has_strong_rngseed`) | `isaac64_ctx` ~2 KB each |
| Player (`u`, `youmonst`, `urealtime`, `ubirthday`, body slots) | ~25 | `struct you u` (hundreds of fields) |
| Dungeon topology | ~10 (`dungeons[]`, `sp_levchn`, `dungeon_topology`, branches, mapseen chain, level_info, n_dgns) | `dungeons[MAXDUNGEON]` |
| Current level (`level`) | ~3 | `dlevel_t` (~ `COLNO*ROWNO * sizeof(struct rm + 2 ptrs)` ≈ 80×21 grid × ~24 B ≈ 40 KB) |
| Flags / options | 3 structs (`flags`, `iflags`, `sysflags`) + `Cmd` | huge `instance_flags` |
| Time/turn | `moves`, `monstermoves`, `wailmsg`, `multi`, `occtime`, `domove_attempting/succeeded`, `ubirthday` | scalars |
| Inventory / item lists | ~20 obj pointers (`invent`, `uwep`, `uarm…`, `uchain`, `uball`, `migrating_objs`, `billobjs`, `current_wand`, `thrownobj`, `kickedobj`, `ftrap`) | linked lists |
| Monster lists | `youmonst`, `mydogs`, `migrating_mons`, `mvitals[NUMMONS]`, per-level `monlist`, `wheads/wtails/wgrowtime` worms | |
| Display / message | `toplines`, `WIN_*` winids, `viz_array`, `viz_clear`, `could_see`, `tc_gbl_data`, `saved_plines[]`, `prevmsg`, `pline_flags`, top-ten window | viz arrays |
| File I/O / lock | `SAVEF[]`, `SAVEP[]`, `lock[]`, `fqn_prefix[]`, `fqn_filename_buffer`, `lockfd`, `lockptr`, `hackpid`, `program_state`, `restoring` | |
| NLE wrapper | `current_nle_ctx` (thread-local-ish pointer), `settings`, `nle_seeds_init`, `nle_seeds[]`, `has_strong_rngseed`, `linux_flag_console`, `erase_char/intr_char/kill_char` | inside `nle_ctx_t` |
| Misc | rooms[], doors[], `bhitpos`, `tbx/tby`, `m_shot`, `quest_status`, `spl_book[]`, `apelist`, `plinemsg_types`, etc. | |

**Rough top-level total: ~120–150 distinct mutable global names**, dominated in bytes by `level` (the current map, ~40 KB), `flags`+`iflags` (~few KB of options), and the `dungeons[]` array.

**Hardest subsystem to refactor:** the **`flags` / `iflags` macro substitution layer**. `flag.h` (lines 28–32, 459–469) `#define`s `wizard ≡ flags.debug`, `discover ≡ flags.explore`, `use_color ≡ wc_color`, etc.; `decl.h` defines ~30 macros like `oracle_level ≡ dungeon_topology.d_oracle_level`. There are also ~20 macros that hide field access of `level` (`levl`, `fobj`, `fmon`, `level.flags.has_X`, etc., in `rm.h` line 618 onward). Every one of these is sprinkled across thousands of call sites; touching the storage of `flags`/`level`/`dungeon_topology` requires re-routing these macros to take a state pointer — a sweeping mechanical change.

**Easiest:** RNG. `rnd.c` already wraps state in a tiny `rnglist[]` array (line 22) with public init/draw entry points (`init_isaac64`, `rn2`, `rn2_on_display_rng`, `set_random`). Moving the two `isaac64_ctx` instances plus `nle_seeds[2]` and `has_strong_rngseed` into `nle_state` is local: only `rnd.c`, `hacklib.c` (lines 855–878), and `nle.c` (lines 367–547) need to learn about the state pointer. **Start here.**

---

## Subsystem-by-Subsystem Tables

### 1. RNG

| name | type | file | size / notes |
|---|---|---|---|
| `rnglist[]` (`static`) | `struct rnglist_t[2]` (`isaac64_ctx rng_state` + `fn` + `init`) | `src/rnd.c:22` | The actual RNG state. `CORE` (gameplay) and `DISP` (display). `isaac64_ctx` ~2 KB each. |
| `nle_seeds[]` | `unsigned long[2]` | `src/hacklib.c:855` | Last seeds set via `set_random()`; read by `nle_get_seed()`. |
| `has_strong_rngseed` | `boolean` (NEARDATA) | decl in `decl.h:223`, def `decl.c:123` | Used to drive `reseed_random()` behavior. |
| `seed` (in `rn2_on_display_rng` non-ISAAC path) | `static unsigned` | `src/rnd.c:93` | Only compiled when `USE_ISAAC64` is off; ignore for NLE. |

Lifetime: per-game (re-seeded at start), persisted via savefile.

### 2. Player state (`u`, body slots)

| name | type | file | size / notes |
|---|---|---|---|
| `u` | `struct you` NEARDATA | `decl.c:141`, decl in `decl.h:257` | The hero. Hundreds of fields — see `include/you.h` line 274. Per-game. |
| `youmonst` | `struct monst` NEARDATA | `decl.c:134` | "Monster shape" of the hero (polymorph). |
| `ubirthday` | `time_t` NEARDATA | `decl.c:142` | Wall-clock birth time. |
| `urealtime` | `struct u_realtime` NEARDATA | `decl.c:143` | Real-time-played counters. |
| `plname[PL_NSIZ]` | char array | `decl.c:53` | Player name. |
| `pl_character[PL_CSIZ]`, `pl_race`, `pl_fruit[PL_FSIZ]`, `tune[6]` | char/arrays | `decl.c:54-60` | Identity / Castle tune. |
| `dogname/catname/horsename[PL_PSIZ]`, `preferred_pet` | char arrays | `decl.c:211-214` | Pet name preferences. |
| `quest_status` | `struct q_score` | `decl.c:43` | Quest progress (per-game). |
| `spl_book[MAXSPELL+1]` | `struct spell` array | `decl.c:193` | Learned spells. |
| `mvitals[NUMMONS]` | `struct mvitals` array | `decl.c:222` | Birth/death/extinct counters per species. |
| body-slot pointers: `invent, uwep, uarm, uswapwep, uquiver, uarmu, uskin, uarmc, uarmh, uarms, uarmg, uarmf, uamul, uright, uleft, ublindf, uchain, uball` | `struct obj *` (NEARDATA) | `decl.c:149-161` | Currently worn/wielded objects. |
| `current_wand, thrownobj, kickedobj` | `struct obj *` (NEARDATA) | `decl.c:163-166` | Item-in-action pointers. |
| `migrating_objs, billobjs` | `struct obj *` | `decl.c:200-202` | Objs moving levels / unpaid. |
| `done_money` | long NEARDATA | `decl.c:51` | Gold owned at death. |
| `killer` | `struct kinfo` NEARDATA | `decl.c:50` | Killer linked list. |

Lifetime: mostly per-game; many are saved/restored.

### 3. Dungeon topology

| name | type | file | size / notes |
|---|---|---|---|
| `dungeons[MAXDUNGEON]` | `dungeon[]` NEARDATA | **`decl.c:104`** | The dungeon graph (Mines, Sokoban, Hell, Quest, Gehennom…). Per-game. |
| `sp_levchn` | `s_level *` NEARDATA | `decl.c:105` | Linked list of special levels. |
| `dungeon_topology` | `struct dgn_topology` | `decl.c:41` | Cached d_levels (`oracle_level`, `medusa_level`, …). 27 `d_level` fields. Accessed everywhere through macros in `decl.h:80-109`. |
| `n_dgns` | int | **`dungeon.c:28`** | Count of active dungeons; used in mklev.c, do.c. |
| `branches` | `static branch *` | `dungeon.c:29` | Linked list of dungeon branches. |
| `mapseenchn` | `mapseen *` | `dungeon.c:31` | "Dungeon overview" mapseen chain — one node per visited level. |
| `level_info[MAXLINFO]` | `struct linfo[]` | `decl.c:89` | Per-level metadata: VISITED / LFILE_EXISTS flags. `MAXLINFO = MAXDUNGEON*MAXLEVEL`. |
| `upstair, dnstair, upladder, dnladder, sstairs` | `stairway` NEARDATA | `decl.c:106-110` | Stair endpoints for *current* level (per-level, not per-dungeon). |
| `updest, dndest` | `dest_area` NEARDATA | `decl.c:111-112` | Level-change destination areas. |
| `inv_pos` | `coord` NEARDATA | `decl.c:113` | Last invisible-monster pos? (per-level scratch.) |

Lifetime: `dungeons[]`, `sp_levchn`, `branches`, `mapseenchn`, `level_info`, `dungeon_topology`, `n_dgns` are **per-game**. `upstair/dnstair/sstairs/updest/dndest/inv_pos` are **per-level**.

### 4. Current level (`level`)

| name | type | file | size / notes |
|---|---|---|---|
| `level` | `dlevel_t` (not NEARDATA) | **`decl.c:132`**, type `rm.h:592-609` | The active level map. `struct rm locations[COLNO][ROWNO]` + parallel `struct obj *objects[][]` + `struct monst *monsters[][]` arrays + `objlist/buriedobjlist/monlist/damagelist/bonesinfo/flags`. **~40 KB**. Re-loaded from level file each time the player goes to a new level. |
| `lastseentyp[COLNO][ROWNO]` | `schar[][]` | `decl.c:145` | Memory map of last-seen terrain. |
| `ftrap` | `struct trap *` | `decl.c:133` | Head of trap linked list for current level. |
| `rooms[(MAXNROFROOMS+1)*2]` | `struct mkroom[]` NEARDATA | `decl.c:128` | Room records for current level. |
| `subrooms` | `struct mkroom *` | `decl.c:129` | Alias into `rooms[]`. |
| `upstairs_room, dnstairs_room, sstairs_room` | `struct mkroom *` | `decl.c:130` | Pointers into `rooms[]`. |
| `doors[DOORMAX]` | `coord[]` | `decl.c:126` | Door positions on current level. |
| `nroom, nsubroom, doorindex, smeq[MAXNROFROOMS+1]` | int (NEARDATA) | `decl.c:26-27, 46-47` | Counts and equivalence classes used during level building/runtime. |
| `bhitpos` | `coord` NEARDATA | `decl.c:125` | Position of last bolt impact. |
| `tbx, tby` | `schar` NEARDATA | `decl.c:98` | mthrowu target. |
| `vault_x, vault_y, made_branch` | `static` | `mklev.c:41-42` | Set during current-level creation. |
| Aliases via `rm.h:618+`: `levl ≡ level.locations`, `fobj ≡ level.objlist`, `fmon ≡ level.monlist`, etc. | macros | — | Pervasive. |

Lifetime: per-level (re-loaded from disk on stair traverse).

### 5. Flags / options

| name | type | file | size / notes |
|---|---|---|---|
| `flags` | `struct flag` NEARDATA | `decl.c:136` (and `options.c:10` linkage stub) | The big saved-with-game options struct. See `flag.h:18-165`. Macro aliases `wizard`, `discover`, etc. |
| `iflags` | `struct instance_flags` NEARDATA | `decl.c:140` (and `options.c:14`) | Per-process "instance" flags — paradoxically named, since today they're already process-wide. `flag.h:235-451`. Includes `getloc_*`, `wc_*` window-cap settings, `last_msg`, `purge_monsters`, `window_inited`, `vision_inited`, `sanity_check`, `save_uswallow/uinwater/uburied` (hangup-save cache), `returning_missile` (`genericptr_t`), `mines_prize_type`, etc. |
| `sysflags` | `struct sysflag` NEARDATA | `decl.c:138` (`#ifdef SYSFLAGS`) | Platform-specific; mostly empty in NLE. |
| `Cmd` | `struct cmd` NEARDATA | declared `flag.h:619`, defined elsewhere (cmd.c) | Command parsing state; includes `const struct ext_func_tab *commands[256]` — see Risks. |
| `program_state` | `struct sinfo` NEARDATA | `decl.c:91`, type `decl.h:147-164` | gameover/stopprint/panicking/exiting/in_moveloop bits. |
| `restoring` | `boolean` | `restore.c:85` | True while loading a save file. |
| `chosen_windowtype[WINTYPELEN]` | char[] | `decl.c:20` | Windowport name. |
| `bases[MAXOCLASSES]` | int[] NEARDATA | `decl.c:22` | Object class base indices. |
| `multi, multi_reason, occtime, occtxt, occupation, afternmv, nomovemsg, save_cm, in_doagain, in_mklev, stoned, unweapon, mrg_to_wielded, defer_see_monsters, in_steed_dismounting, ransacked, vision_full_recalc` | int/bool/pointer NEARDATA | `decl.c:8-9, 24-28, 35, 52, 115-123, 274` | Engine modal flags. Per-game / per-step. |
| `m_shot` | `struct multishot` NEARDATA | `decl.c:102` | Throwing-shot state. |
| `apelist` | `struct autopickup_exception *` | `decl.c:219` | Autopickup regex chain. |
| `plinemsg_types` | `struct plinemsg_type *` | `decl.c:349` | Pline message-type regex chain (callbacks!). |
| `menu_colorings` | `struct menucoloring *` | `decl.c:231` | Menu color regex chain. |
| `need_redraw, mapped_menu_cmds[], n_menu_mapped, initial, from_file` | static | `options.c:469-542` | Options-parsing state. |

### 6. Time and turn

| name | type | file | size / notes |
|---|---|---|---|
| `moves` | `long` NEARDATA | `decl.c:195` | Player turn counter. |
| `monstermoves` | `long` NEARDATA | `decl.c:195` | Diverges from `moves` when Fast. |
| `wailmsg` | `long` NEARDATA | `decl.c:197` | Timer for banshee wail. |
| `multi`, `occtime`, `in_doagain` | int NEARDATA | `decl.c:24, 28, 35` | Multi-turn action counters. |
| `domove_attempting`, `domove_succeeded` | `long` NEARDATA | `decl.c:223-224` | Per-turn movement bookkeeping. |

### 7. Inventory / item lists

Already enumerated under §2 (player) plus:

| name | type | file | size / notes |
|---|---|---|---|
| `ffruit` | `struct fruit *` NEARDATA | `decl.c:58` | Linked list of named fruits (per-game). |
| `warn_obj_cnt` | int NEARDATA | `decl.c:45` | Count of warn-class objects in invent. |
| `obufs[NUMOBUF][BUFSZ], obufidx` | `static char[][]` | `objnam.c:81-82` | Round-robin buffers returned by `xname`/`doname`. **Hidden process-wide state**: callers compare returned pointers for equality, so this can't be freely thread-localized. |
| `distantname` | static int | `objnam.c:240` | doname-mode flag. |
| `mkot_trap_warn_count` | static int | `artifact.c:2104` | One-shot warning state. |
| `lastinvnr` | static int | `invent.c:42` | Last invent letter assigned. |
| `sortlootmode` | static unsigned | `invent.c:300` | sortloot scratch. |
| `cached_pickinv_win` | static `winid` | `invent.c:2541` | Cached menu window id. |
| `this_type`, `only` | static | `invent.c:3098, 4417` | Pickup-filter scratch. |
| `venom_inv[]`, `removeables[]`, `readable[]`, `beverages[]`, `nofetch[]`, etc. | static const | various | constant tables — **skip**. |
| `artiexist[]` | `static boolean[]` | `artifact.c:48` | Which artifacts have been generated. **Per-game**, saved. |

### 8. Monster lists

| name | type | file | size / notes |
|---|---|---|---|
| `youmonst` | `struct monst` NEARDATA | `decl.c:134` | See §2. |
| `mydogs` | `struct monst *` NEARDATA | `decl.c:216` | Pets that came with you on stair traverse. |
| `migrating_mons` | `struct monst *` NEARDATA | `decl.c:218` | Monsters in transit to other levels. |
| `level.monlist` (alias `fmon`) | `struct monst *` | `rm.h:618+` | Monsters on current level. |
| `mvitals[NUMMONS]` | array | `decl.c:222` | Per-species stats (see §2). |
| `wheads[MAX_NUM_WORMS], wtails[MAX_NUM_WORMS], wgrowtime[MAX_NUM_WORMS]` | arrays | `worm.c:70-71` | Long-worm segments. Per-game; saved. |
| `ustuck_id, usteed_id` | static unsigned | `save.c:75` | Save/restore monster-id scratch. |
| `id_map, n_ids_mapped` | static | `restore.c:75-76` | Restore-time monster-id remap. |
| `oldfruit, omoves` | static | `restore.c:86-87` | Restore-time scratch for bones. |

### 9. Display / message buffers

| name | type | file | size / notes |
|---|---|---|---|
| `WIN_MESSAGE, WIN_STATUS, WIN_MAP, WIN_INVEN` | `winid` NEARDATA | `decl.c:278-280` | Windowport handles. |
| `toplines[TBUFSZ]` | char[] | `decl.c:281` | Last message printed (TBUFSZ ~256). |
| `prevmsg[BUFSZ]`, `pline_flags` | static | `pline.c:13-14` | Repetition-suppression state. |
| `saved_plines[DUMPLOG_MSG_COUNT], saved_pline_index` | `char *[]` | `pline.c:24-25` | Ring buffer for dumplog. |
| `you_buf, you_buf_siz` | static | `pline.c:264-265` | Grow-buffer for `You()` formatting. |
| `tc_gbl_data` | `struct tc_gbl_data` | `decl.c:283` | Tty AS/AE/LI/CO. CO and LI are written by NLE in `nle_start`. |
| `viz_array` | `char **` NEARDATA | `decl.c:275` | Row-pointer array into viz arrays. |
| `viz_rmin, viz_rmax` | char* | `vision.c:81` | Current vision column bounds per row. |
| `could_see[2][ROWNO][COLNO]`, `cs_rows0/1`, `cs_rmin0/1`, `cs_rmax0/1`, `viz_clear[ROWNO][COLNO]`, `viz_clear_rows`, `left_ptrs, right_ptrs` | `static char[…]` | `vision.c:85-94` | LOS scratch buffers (a few KB total). |
| `light_base` | `static light_source *` | `light.c:45` | Linked list of active light sources. |
| `toptenwin` | `static winid` | `topten.c:88` | Top-ten window handle. |
| `final_fpos` | static long | `topten.c:27` | Top-ten file scratch. |
| `soundmap` | `static audio_mapping *` | `sounds.c:1113` | Sound-effect map. |
| `chosen_windowtype` | char[] | `decl.c:20` | See §5. |

### 10. File I/O / lock / process state

| name | type | file | size / notes |
|---|---|---|---|
| `hackpid` | int | `decl.c:13` | Process id; used in lock filenames. |
| `locknum` | int (UNIX/VMS) | `decl.c:15` | Multi-user-limit. |
| `hname` | `const char *` | `decl.c:12` | argv[0]. |
| `SAVEF[SAVESIZE]`, `SAVEP[SAVESIZE]` | char[] | `files.c:111-113` | Save file paths. |
| `lock[PL_NSIZ + 14 / 17 / 25]` | char[] | `files.c:72-84` | Per-game lock filename prefix. |
| `fqn_prefix[PREFIX_COUNT]` | `char *[]` | `decl.c:285` | Per-prefix path roots (10 entries). NLE rewrites these in `nle.c:182-191`. |
| `fqn_filename_buffer[FQN_NUMBUF][FQN_MAX_FILENAME]` | `static char[][]` | `files.c:67` | Round-robin path buffer (~5 KB). |
| `wizkit[WIZKIT_MAX]` | static char[] | `files.c:129` | Wizkit content. |
| `lockptr` | static int | `files.c:136 / 146` | File-descriptor of current lock. |
| `config_section_chosen, config_section_current` | `static char *` | `files.c:224-225` | Config-file parse state. |
| `nesting, lockfd` | static int | `files.c:1659, 1662` | Lock-file recursion. |
| `program_state` | struct | `decl.c:91` | See §5. |
| `restoring` | bool | `restore.c:85` | See §5. |
| `dotcnt, dotrow, bytes_counted, count_only` | int/long | `save.c:17-22` | Save progress dots. |
| `sfcap, sfsaveinfo, sfrestinfo` | `struct savefile_info` | `decl.c:303-326` | Savefile compatibility caps. |
| `chosen_windowtype` | char[] | `decl.c:20` | See §5. |

### 11. NLE wrapper layer (`nle.c`, `hacklib.c`)

| name | type | file | size / notes |
|---|---|---|---|
| `current_nle_ctx` | `nle_ctx_t *` | `include/nle.h` (def at file scope) | The thread-local-ish "current instance" pointer — used by `nle_putchar`, `nle_yield`, `nle_fflush`, `nle_done`, etc. **This is exactly what the refactor needs to eliminate / generalize.** |
| `nle_ctx_t` contents | `fcontext_stack_t stack`, two `fcontext_t`, `FILE *ttyrec`, `TMT *vterminal`, `outbuf[BUFSIZ]`, write ptrs, optional `void *ttyrec_bz2`, `boolean done`, `nle_obs *observation` | `include/nle.h:18-35` | Allocated by `init_nle()`; tied to one running game. |
| `settings` | `nle_settings` | `nle.c:147` | Copy of last-supplied settings (hackdir, scoreprefix, wizkit, options, ttyrecname, spawn_monsters). **Shared across all instances** — clobbered by each new `nle_start()` call. |
| `nle_seeds_init` | `nle_seeds_init_t *` | `nle.c:367` | Pointer to seed-init struct supplied by caller. Set in `nle_start`, cleared after first use. |
| `erase_char, intr_char, kill_char` | char | `nle.c:569` | Stubbed tty controls. |
| `linux_flag_console` | int | `nle.c:608` | Linux-tty detection. |
| `nle_seeds[2]` | `unsigned long[]` | `hacklib.c:855` | The seeds last set (one per RNG). See §1. |

### 12. Misc / other

| name | type | file | size / notes |
|---|---|---|---|
| `occupation`, `afternmv` | function pointers | `decl.c:8-9` | Current multi-turn task callback. Persisted via savefile *by name lookup*. |
| `context` | `struct context_info` NEARDATA | `decl.c:135`, type `context.h:108-143` | A grab-bag of mid-game state: digging progress, eating victual, tin/spbook in progress, takeoff queue, polearm aim, splittable obj record, novel-passage tracking, `rndencode` (escape introducer), warnlevel, monster `ident` counter, run-direction (0..8), travel-target flags, botl/botlx redraw flags, `next_attrib_check`, `stethoscope_*`. ~25 fields incl. embedded sub-structs. |
| `tune[6]`, `quest_status`, `spl_book[]`, `ffruit`, `mvitals` | already covered | | |
| `bases[MAXOCLASSES]` | int[] | `decl.c:22` | Object-class base ids assigned at game start. |
| `x_maze_max, y_maze_max, otg_temp` | int | `decl.c:31, 33` | Constants-ish; `otg_temp` is per-call scratch. |
| `multi_reason, nomovemsg, save_cm, occtxt` | `const char *` | `decl.c:25, 52, 48, 63` | Pointers into static strings; per-game state. |
| `yn_number` | long | `decl.c:70` | Last yn-prompt numeric answer. |
| `nhUse_dummy` | unsigned | `decl.c:356` | Lint side-channel; ignorable. |
| `ARGV0` (`PANICTRACE`) | `const char *` | `decl.c:352` | Not compiled in NLE typically. |
| `c_color_names, c_common_strings, c_obj_colors, materialnm, zapcolors, shield_static, xdir/ydir/zdir, quitchars, vowels, yn*chars, disclosure_options, def_oc_syms, def_monsyms, monexplain, oclass_names, zeroobj, zeromonst, zeroany` | mostly `const` | `decl.c` various | **Skip (const)** — except `oc_syms[MAXOCLASSES]` and `monsyms[MAXMCLASSES]` which **are** mutable (current symbol set, see `decl.h:236, 238`). |
| `pline_flags`, `prevmsg`, `you_buf`, `you_buf_siz` | static | `pline.c` | See §9. |
| `obufs/obufidx` | static | `objnam.c` | See §7 — sharp identity hazard. |
| Top-ten / bones / save scratch | various | | See §8, §10. |

---

## Sketch: `struct nle_state`

```c
/* All process-wide game state, threaded as 's' through every NetHack function. */
struct nle_state {

    /* ---------- 1. RNG (start here — smallest, easiest) ---------- */
    struct {
        isaac64_ctx core;          /* gameplay RNG */
        isaac64_ctx disp;          /* display/cosmetic RNG */
        unsigned long seeds[2];    /* last seeds applied (CORE, DISP) */
        boolean has_strong_seed;
        boolean core_initialized;
        boolean disp_initialized;
    } rng;

    /* ---------- 2. Player (the "u" struct + body slots) ---------- */
    struct you            u;             /* ~hundreds of fields */
    struct monst          youmonst;
    time_t                ubirthday;
    struct u_realtime     urealtime;
    char                  plname[PL_NSIZ];
    char                  pl_character[PL_CSIZ];
    char                  pl_race;
    char                  pl_fruit[PL_FSIZ];
    char                  tune[6];
    char                  dogname[PL_PSIZ], catname[PL_PSIZ], horsename[PL_PSIZ];
    char                  preferred_pet;
    struct q_score        quest_status;
    struct spell          spl_book[MAXSPELL + 1];
    struct mvitals        mvitals[NUMMONS];
    long                  done_money;
    struct kinfo          killer;
    /* body-slot pointers (18) */
    struct obj           *invent, *uwep, *uswapwep, *uquiver, *uarm, *uarmu,
                         *uskin, *uarmc, *uarmh, *uarms, *uarmg, *uarmf,
                         *uamul, *uright, *uleft, *ublindf, *uchain, *uball;
    struct obj           *current_wand, *thrownobj, *kickedobj;
    struct obj           *migrating_objs, *billobjs;
    struct fruit         *ffruit;
    int                   warn_obj_cnt;
    boolean               artiexist[1 + NROFARTIFACTS + 1];  /* artifact.c */

    /* ---------- 3. Dungeon topology (per-game) ---------- */
    dungeon               dungeons[MAXDUNGEON];
    int                   n_dgns;
    s_level              *sp_levchn;
    branch               *branches;
    mapseen              *mapseenchn;
    struct dgn_topology   dungeon_topology;
    struct linfo          level_info[MAXLINFO];   /* MAXDUNGEON*MAXLEVEL */

    /* ---------- 4. Current level (per-level, swapped on stair) ---------- */
    dlevel_t              level;                   /* ~40 KB */
    schar                 lastseentyp[COLNO][ROWNO];
    struct trap          *ftrap;
    struct mkroom         rooms[(MAXNROFROOMS + 1) * 2];
    struct mkroom        *subrooms, *upstairs_room, *dnstairs_room, *sstairs_room;
    int                   nroom, nsubroom, doorindex;
    int                   smeq[MAXNROFROOMS + 1];
    coord                 doors[DOORMAX];
    stairway              upstair, dnstair, upladder, dnladder, sstairs;
    dest_area             updest, dndest;
    coord                 inv_pos, bhitpos;
    schar                 tbx, tby;
    /* vision */
    boolean               vision_full_recalc;
    char                **viz_array;
    char                 *viz_rmin, *viz_rmax;
    char                  could_see[2][ROWNO][COLNO];
    char                 *cs_rows0[ROWNO], *cs_rows1[ROWNO];
    char                  cs_rmin0[ROWNO], cs_rmax0[ROWNO];
    char                  cs_rmin1[ROWNO], cs_rmax1[ROWNO];
    char                  viz_clear[ROWNO][COLNO];
    char                 *viz_clear_rows[ROWNO];
    char                  left_ptrs[ROWNO][COLNO], right_ptrs[ROWNO][COLNO];
    light_source         *light_base;              /* from light.c */
    /* mklev scratch (per-level build only) */
    xchar                 vault_x, vault_y;
    boolean               made_branch;

    /* ---------- 5. Flags / options ---------- */
    struct flag           flags;
    struct instance_flags iflags;
#ifdef SYSFLAGS
    struct sysflag        sysflags;
#endif
    struct cmd            Cmd;                     /* command parser */
    struct sinfo          program_state;
    boolean               restoring;
    char                  chosen_windowtype[WINTYPELEN];
    int                   bases[MAXOCLASSES];
    /* engine modes (~15 small flags) */
    int                   multi, occtime, in_doagain;
    const char           *multi_reason, *nomovemsg, *occtxt;
    char                 *save_cm;
    boolean               in_mklev, stoned, unweapon, mrg_to_wielded,
                          defer_see_monsters, in_steed_dismounting,
                          ransacked;
    struct multishot      m_shot;
    long                  yn_number;
    /* regex/colour chains */
    struct autopickup_exception *apelist;
    struct plinemsg_type        *plinemsg_types;
    struct menucoloring         *menu_colorings;

    /* ---------- 6. Time and turn ---------- */
    long                  moves, monstermoves, wailmsg;
    long                  domove_attempting, domove_succeeded;

    /* ---------- 7. Monster lists ---------- */
    struct monst         *mydogs, *migrating_mons;
    struct wseg          *wheads[MAX_NUM_WORMS], *wtails[MAX_NUM_WORMS];
    long                  wgrowtime[MAX_NUM_WORMS];

    /* ---------- 8. Display / message ---------- */
    winid                 WIN_MESSAGE, WIN_STATUS, WIN_MAP, WIN_INVEN;
    char                  toplines[TBUFSZ];
    char                  prevmsg[BUFSZ];
    unsigned              pline_flags;
    char                 *saved_plines[DUMPLOG_MSG_COUNT];
    unsigned              saved_pline_index;
    char                 *you_buf;
    int                   you_buf_siz;
    struct tc_gbl_data    tc_gbl_data;
    winid                 toptenwin;
    long                  topten_final_fpos;

    /* ---------- 9. File I/O / lock / process ---------- */
    int                   hackpid;
    char                  SAVEF[SAVESIZE], SAVEP[SAVESIZE];
    char                  lock[PL_NSIZ + 25];
    char                 *fqn_prefix[PREFIX_COUNT];
    char                  fqn_filename_buffer[FQN_NUMBUF][FQN_MAX_FILENAME];
    char                  wizkit[WIZKIT_MAX];
    int                   lockptr, lockfd, nesting;
    char                 *config_section_chosen, *config_section_current;
    long                  bytes_counted;
    int                   count_only, dotcnt, dotrow;
    struct savefile_info  sfcap, sfsaveinfo, sfrestinfo;

    /* ---------- 10. Misc ---------- */
    int                 (*occupation)(void);
    int                 (*afternmv)(void);
    struct context_info   context;
    uchar                 oc_syms[MAXOCLASSES];
    uchar                 monsyms[MAXMCLASSES];
    /* objnam round-robin (must remain a stable address-comparable pool) */
    char                  obufs[NUMOBUF][BUFSZ];
    int                   obufidx, distantname;
    int                   lastinvnr;             /* invent.c */
    unsigned              sortlootmode;
    winid                 cached_pickinv_win;
    /* restore scratch */
    struct fruit         *oldfruit;
    long                  omoves;
    int                   n_ids_mapped;
    struct bucket        *id_map;
    unsigned              ustuck_id, usteed_id;

    /* ---------- 11. NLE wrapper ---------- */
    /* (the existing nle_ctx_t can either be embedded here or kept as a
       sibling.  current_nle_ctx must go away in favour of `state`.) */
    fcontext_stack_t      stack;
    fcontext_t            returncontext;
    fcontext_t            generatorcontext;
    FILE                 *ttyrec;
    TMT                  *vterminal;
    char                  outbuf[BUFSIZ];
    char                 *outbuf_write_ptr, *outbuf_write_end;
#ifdef NLE_BZ2_TTYRECS
    void                 *ttyrec_bz2;
#endif
    boolean               done;
    nle_obs              *observation;
    nle_settings          settings;
    nle_seeds_init_t     *seeds_init;       /* transient */
};
```

The struct as drawn is well under 100 KB (dominated by `level` ~40 KB and the vision LOS buffers ~25 KB).  N instances at this size are trivially shareable in one process.

---

## Risks / Surprises

1. **Macro storms over storage names.** Hundreds of macros in `decl.h`, `flag.h`, `rm.h`, `dungeon.h`, `you.h` rewrite simple identifiers to field accesses on globals: e.g.
   - `decl.h:80-109` — 27 `..._level` macros that expand to `dungeon_topology.d_..._level`.
   - `flag.h:29` `#define wizard flags.debug`; `flag.h:32` `#define discover flags.explore`; `flag.h:459-468` `#define use_color wc_color`, `#define hilite_pet wc_hilite_pet`, etc.
   - `rm.h:618+` `#define levl level.locations`, `#define fobj level.objlist`, `#define fmon level.monlist`, plus `level.flags.has_X` accessors.
   - `decl.h:113-122` `xdnstair`, `ydnstair`, `xupstair`, `yupstair`, etc.
   Touching the underlying storage means every macro must either gain a state parameter (impossible without re-spelling thousands of call sites) or be re-pointed at `s->flags.debug` etc. The latter is workable but only if a per-translation-unit pointer `s` is in scope at every macro use site — typically by making every function take `struct nle_state *s` as first arg, then `#define wizard (s->flags.debug)`. This is mechanical but enormous.

2. **`Cmd.commands[256]` holds function pointers** into static tables (`flag.h:615`). These point into the same `libnethack.so` in all instances — fine after refactor, but on save/restore they're persisted by *name lookup* (`extern.h` ext_func_tab), not by address. Worth verifying that the save path doesn't accidentally serialize raw pointers.

3. **`occupation` and `afternmv` are function pointers** (`decl.c:8-9`) that are saved/restored by name (`save.c`/`restore.c` have explicit name→pointer tables). Same concern — needs to keep working post-refactor.

4. **`menu_colorings`, `plinemsg_types`, `apelist`** each hold a `struct nhregex *` — compiled regex objects from the windowport. They point into windowport-allocated memory; copying state between instances naively will double-free. Treat as opaque, instance-owned, never shared.

5. **`obufs[NUMOBUF][BUFSZ]` in `objnam.c:81`** is a round-robin pool. Callers of `xname()`/`doname()` sometimes compare returned `char *` for identity (e.g. "did this string survive across calls?"). Moving this pool into the state struct is safe; just don't shrink or eliminate it.

6. **`fqn_prefix[]` is rewritten by `nle.c:182-191` on every `nle_start()`.** Today, since each instance is in its own `dlopen`'d copy, this is fine. After refactor, multiple instances must each have their own `fqn_prefix[]` — already covered above, but the `nle.c` code path needs to set `s->fqn_prefix` instead of the global.

7. **`current_nle_ctx` (`nle.h`).** Used by `nle_putchar`, `nle_fflush`, `nle_yield`, `write_ttyrec_data`, `nle_get_obs`, `nle_done`, the TMT callback, etc. After refactor these become `s->...` accesses but the TMT callback and `fcontext` jumps still need to pass `s` across context switches — the existing `nle_vt_callback` already receives `p = nle` as a void pointer (`nle.c:74`), so this is straightforward.

8. **`hacklib.c` defines `nle_seeds[]`** at line 855 *outside* `rnd.c`. The RNG state is split between two TUs. Refactor needs to keep these together (move `nle_seeds` into `state.rng.seeds`).

9. **NetHack has very rich `static` state inside many `.c` files** — every file in `vision.c`, `light.c`, `worm.c`, `pline.c`, `objnam.c`, `topten.c`, `restore.c`, `save.c`, `mklev.c`, `files.c`, `options.c`, `artifact.c`, `invent.c`, `muse.c`, `dogmove.c`, `polyself.c`, etc. holds a handful of `static` scratch variables that the refactor must also surface (or assert are safe per-instance). I sampled ~20 files; expect a dozen-or-so more globals to be discovered per file when work begins.

10. **Embedded sub-structs in `context_info`** (`context.h:108-143`): `digging`, `victual`, `tin`, `spbook`, `takeoff`, `warntype`, `polearm`, `objsplit`, `tribute`, `novel`. Each is itself a small struct of mid-action state — they're flat by value inside `context`, so they ride along when you move `context` into `state`.

11. **`level` is referenced by raw struct symbol from external windowports** (e.g. `winrl.cc`, the NLE C++ glue). Anything that includes `decl.h` / `rm.h` and types `level.locations[x][y]` will break unless those references are also rewritten to `s->level.locations[x][y]`. Audit all `vendor/nle/win/` consumers.

12. **`program_state` is read at `nle.c:504`** during `nle_end`. The refactor must keep `s->program_state.panicking` available during cleanup, including in the panic/abort paths.

13. **Hangup-save fields inside `iflags`** (`save_uswallow`, `save_uinwater`, `save_uburied`, `flag.h:436-438`) duplicate `u` fields temporarily. Per-instance is fine; just noting they aren't independent options.

14. **`Cmd.spkeys[NUM_NHKF]`** (special-key remap) is loaded from a config file and used during command parsing. Per-instance is fine; the config-file parser writes to it via static helpers in `cmd.c` / `options.c` that will need state plumbing.

15. **The `flags` struct itself includes `int *opt_booldup` / `int *opt_compdup`** (`flag.h:280-281`) — pointers into option-parser tables. Already process-wide; refactor should make these per-instance and ensure the option parser writes to `s->iflags.opt_booldup`.

---

## Suggested refactor ordering (sanity check)

1. **RNG** — move `rnglist[]`, `nle_seeds[]`, `has_strong_rngseed` into `state.rng`. Thread `s` through `rn2`, `rnd`, `rne`, `rnz`, `d`, `rnl`, `init_isaac64`, `set_random`, `reseed_random`. Maybe 10–20 functions to update; only `rnd.c`, `hacklib.c`, `nle.c` need state-aware signatures.
2. **NLE wrapper** — fold `nle_ctx_t` into `nle_state`; make `current_nle_ctx` a temporary alias (or remove it).
3. **`context`** — small grab-bag, well-encapsulated.
4. **`flags`/`iflags`/`sysflags`** — touch every macro in `flag.h`; biggest "macro storm" surface but no algorithmic complexity.
5. **`u`, body-slot pointers, `youmonst`, `urealtime`, `quest_status`, `mvitals`, `spl_book`** — the player.
6. **`dungeons[]`, `sp_levchn`, `branches`, `mapseenchn`, `dungeon_topology`, `level_info`, `n_dgns`** — the dungeon graph.
7. **`level` + per-level scratch + vision + light + worms** — the current level. Biggest blob of state. Many macros.
8. **File I/O, save/restore, top-ten** — pure plumbing.
9. **Display/message ring buffers, options parser, regex chains** — low-traffic but lots of static fields per `.c`.

End of audit.
