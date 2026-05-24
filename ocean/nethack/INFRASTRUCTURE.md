# NetHack on PufferLib — engineering reference

How NetHack 3.6 — a 30-year-old single-process terminal game with hundreds
of file-scope globals — was wrapped into PufferLib's vectorized RL training
harness so that 1024+ envs run in parallel under a single Python process
without corrupting each other.

Reading order: §1 gives the high-level shape; §2–§8 dive into each
subsystem; §9 is the operational stuff (build, test, debug).

---

## 1. The shape

PufferLib's harness, per env step, does roughly this:

```
for t in 0..horizon:
    policy_net(obs)                 -> actions on GPU
    DtoH(actions)                   -> CPU
    #pragma omp parallel for        -> one c_step per env per outer iter
        c_step(envs[i])
    HtoD(observations, rewards, terminals)
```

Each env's C-side state lives in a `struct Nethack` (one per env, in a
contiguous array owned by the harness). Each `struct Nethack` carries:

- A pointer to one `nle_ctx_t` — the per-env NetHack game state (§2).
- Scratch arrays for the observation channels (chars, colors, glyphs,
  blstats, message, inventory).
- Per-episode bookkeeping (tick, prev_score, prev_depth, visited bitmap
  for the scout-bonus reward term, etc.).
- Three function pointers (`fn_start`, `fn_step`, `fn_end`) into
  libnethack. In production these resolve to the libnethack symbols
  directly (statically linked into the puffer extension). Standalone
  test binaries `dlopen` libnethack instead, so the symbols can be
  swapped at runtime.

`c_step(env)` does, in order:

1. Map the agent's discrete action index → NetHack keystroke (`hjkl…`).
2. Call `fn_step(ctx, &obs)` — the actual game tick (§3).
3. Detect if NetHack yielded at a sub-prompt (yn / getlin / xwait / a
   message ending in `?`). If so, inject ESC / Enter and re-step until
   we're back at the top-level command prompt. One c_step can mean
   several inner game ticks.
4. Compute shaped reward = score-delta + descent-bonus + scout-bonus
   + illegal-penalty (per `config/nethack.ini` coefficients).
5. Pack the observation channels into the harness's contiguous
   `observations[N, OBS_SIZE]` array.
6. If the env reported `done`, call `c_reset(env)` which tears down
   the env and starts a fresh game.

That's the whole binding contract. The rest of this document is "why
each of those pieces takes the shape it does".

---

## 2. Per-env game state (`nle_ctx_t`)

The hardest problem. NetHack 3.6 was written assuming one game per
process. State that's logically "per game" lives at file or process
scope:

- The dungeon map (`dlevel_t level`, ~470 KB).
- The player struct (`struct you u`).
- Flags structs (`flags`, `iflags`, `sysflags`).
- Monster chains (`fmon`, head of the level's monster list).
- The save/restore stream FILE pointer.
- The DLB data-file index.
- The current window-port's state.
- The terminal emulator (TMT) buffer.
- The fcontext coroutine stack pointer (§3).
- … hundreds more.

For two envs to step in parallel without corrupting each other, every
one of these has to be per-env.

**The mechanism**: define one big struct, `nle_ctx_t` (in
`vendor/nle/src/include/nle.h`), that holds *all* per-env mutable state.
~75 KB total. At every entry into libnethack we set a thread-local
pointer:

```c
__attribute__((tls_model("initial-exec")))
__thread nle_ctx_t *current_nle_ctx;
```

`initial-exec` keeps this to a single segment-relative load on every
read; the alternative `global-dynamic` would be a function call.

Every former-global is reached via a macro at the top of its `.c` file:

```c
#define u      (*current_nle_ctx->u_ptr)
#define level  (*current_nle_ctx->s_level_p)
#define moves  (current_nle_ctx->nle_moves)
…
```

So `u.uhp` reads `current_nle_ctx->u_ptr->uhp`, automatically.

Some symbols are non-`static` cross-TU globals (`extern boolean m_using;`
in one file, definition in another). For those, every `extern`
declaration in every file gets replaced with the same `#define`.

### Two foot-guns that bit us

**Naming collisions with struct fields.** `read.c` had a global
`boolean known;`. After `#define known (current_nle_ctx->s_known)`, every
`obj->known` member access in the same file *also* got rewritten — a
collision with a real struct member that the compiler didn't catch
gracefully. Fix: rename the global, leave struct members alone.

**`sizeof` on pointer-aliased arrays.** If a former `static char arr[80*21]`
becomes `#define arr (current_nle_ctx->s_arr_p)` (a pointer), then
`sizeof arr` silently changes from 1680 to 8. NetHack's save/restore
code is full of `bwrite(fd, arr, sizeof arr)`. Post-migration the
writer writes 8 bytes; the reader expects 1680; the on-disk record is
short, save/restore breaks. Fix: explicit byte counts at every save
site (`COLNO * ROWNO * sizeof(schar)`), and `_Static_assert` checks
at both ends of save/restore to catch struct-size drift at compile
time.

### When the struct outgrows L2

The 75 KB-per-env struct mostly works, but it's bigger than you'd
want for cache locality. At 1024 envs / 128 cores = 8 envs per core,
each core's working set is ~600 KB — bigger than L2 (1 MB but shared
with other data). PufferLib's harness drives one step per env per OMP
iteration, so the cache fills cold on every step. This is the dominant
cost remaining in our SPS curve (§7). Two ways to fix it are listed
as future work.

---

## 3. Coroutines (fcontext)

NetHack's main loop expects to drive the game: it calls into the
window-port for "give me the next keystroke" whenever the game is
ready for input. RL inverts this: the agent supplies a keystroke per
step from outside.

**The bridge**: each env owns a Boost.Context fcontext stack (~64 KiB).
`nle_start` allocates the stack and schedules NetHack's `moveloop` on
it, then yields back to the caller. `nle_step` jumps onto the stack
(passing the agent's action); NetHack runs until it needs input, then
yields back. NetHack's call frames — including the outer `moveloop`
loop and every helper called from it — persist across yields,
naturally.

The stack lives in `nle_ctx_t->stack`. Nothing else needs to move with
it; the TLS `current_nle_ctx` pointer makes the rest of the per-env
state reachable from any frame that runs on the stack.

---

## 4. Per-env heap arena

NetHack allocates a lot — monsters, objects, level features. With 1024
envs all calling libc malloc on the hot path, glibc's per-arena mutex
becomes a real cost (we measured it). And at env teardown there's no
clean way to free everything an env allocated without walking object
chains.

**Solution**: each env's `nle_ctx_t` carries a private 64 MB anonymous
memory map (`MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE`).
NetHack's `alloc()` bumps a pointer in that map. At `nle_end`, the
whole map is `munmap`'d in one syscall.

`MAP_NORESERVE` is important: 1024 × 64 MB = 65 GB of virtual address
space. The kernel only commits pages as they're touched, so the actual
RSS is much smaller (typically a few MB per env after a short game).

**Caveat**: anything that has to outlive a particular env *must not*
use the arena. The DLB data-file index, signal handlers, the static
window-port jump table, etc. all have process lifetime — they have to
be `__libc_malloc`'d explicitly. We learned this the hard way: the
first env to tear down (`munmap` its arena) would take the DLB index
with it, and the next env's init would crash dereferencing freed
strings. Fix: classify each shared resource and route it through the
right allocator.

---

## 5. Save and restore

NetHack writes a level to disk when the player walks down stairs, and
reads it back when the player walks up. Several invariants:

- Writer and reader must agree on byte counts. After per-env migration
  some former-arrays are now pointer macros; `sizeof X` silently changes
  meaning. We audit all save/restore call sites and use explicit byte
  counts; `_Static_assert` declarations in `save.c` and `restore.c` pin
  the sizes of key structs (`eshk`, `monst`, `obj`) so any future
  divergence fails the build.

- The save/restore dispatch tables (function pointer arrays for "how
  to write a buflen" vs "how to write data") used to be process-global.
  Two envs writing simultaneously would race the dispatch. The tables
  are now per-env in `nle_ctx_t`.

- File paths are per-env via a unique `mkdtemp` directory per env
  (the env's "hackdir"). Make sure all path resolution uses
  `current_nle_ctx->s_fqn_prefix[HACKPREFIX]`, not a process-global.

---

## 6. The window-port (`winrl.cc`)

NetHack uses a windowport interface for output — a struct of function
pointers covering things like "print a status line", "clear this
window", "show this menu". The RL build provides a `rl` window-port
that captures everything the agent needs to read.

A few observations live in the in-memory windowport state and are
copied into the agent's obs buffer at every step:

- `chars_`, `colors_`, `glyphs_`, `specials_` (the 21×79 map grid)
- `blstats_[]` (HP, depth, score, etc.) — populated by a function
  that reads `u.uX` / `youmonst` directly and copies into the array.
- `last_msg` from each window — the message line.
- `inventory_` — vector of (letter, oclass, glyph, string).

**Two gates we apply**. Both observation-invariant — the agent sees the
same bytes either way:

- `iflags.status_updates` is disabled. This short-circuits NetHack's
  status-line renderer (a large sprintf chain that formats numbers
  into a TTY buffer nothing in this build reads) and an associated
  per-tick walk of the "discovered rooms" annotation list.
- The TTY-rendering side of `clear_nhwindow_method` is skipped (the
  in-memory window state is still cleared so observations remain
  correct).
- `nle_putchar` short-circuits when no `tty_chars`/`tty_colors`/
  `tty_cursor` observation channel is bound (the bytes would otherwise
  fill a buffer that's never read).

These three together account for roughly +30% SPS at N=128 and +15%
at N=1024. They are *load-bearing*; reverting any of them costs that
much.

**One trap we hit**: the function that populates `blstats_[]` was
itself reached via the status-line renderer's dispatch chain. When
we disabled the renderer (above), we accidentally also disabled the
stat-population. The agent silently saw zero for HP/DEPTH/AC/etc.
Fix: call the stat-update function directly from the obs-pack path,
unconditionally. The obs contract is now independent of any rendering
toggles.

---

## 7. Performance characteristics

Two cheap perf wins that didn't change behavior:

- **Cache prefetch.** At every `nle_step` entry, we issue four
  `__builtin_prefetch` hints on the first 256 bytes of the env's
  `nle_ctx_t` (the hot fields — flags pointer, level pointer, moves
  counter, etc., all packed at the front of the struct). The L1
  stream prefetcher fills the rest while the function preamble runs.
  ~30% SPS lift at N=1024.

- **Initial-exec TLS for `current_nle_ctx`.** Default
  `__thread` lookups compile to a `__tls_get_addr` function call.
  `__attribute__((tls_model("initial-exec")))` makes them a single
  segment-relative load. ~3% SPS recovered.

The remaining ceiling is the cache thrash from PufferLib's
round-robin OMP scheduling: one step per env per outer iter means
each thread cycles through ~8 different env states (75 KB each), so
each step pays a cold L2 fill. We measured this directly with a
controlled bench `multi_threaded_rr` (same OMP, same envs, same code
path as puffer's c_step, just isolated from Python/CUDA). It shows
~5× slower per env than the same code in the "loop over steps inside
the outer env loop" pattern that `multi_threaded` uses.

This gap is structural to the harness pattern. Two future fixes are
captured separately:

- **Hot/cold split** of `nle_ctx_t`. Only ~1 KB of the 75 KB is
  touched per step. A "hot" struct that fits in one or two cache
  lines plus a pointer to the "cold" remainder would close most of
  the gap. Multi-day refactor.

- **Batched stepping** at the binding/harness boundary. If the
  binding exposed a step-K function and the policy emitted K actions
  per yield, the cache penalty amortizes 1/K. Requires harness change.

---

## 8. Process-shared resources

A small number of resources are conceptually shared across all envs:

- The DLB data-file directory (read-only after init). One copy per
  process, libc-malloc'd, init-once via CAS.
- The window-port jump table (a `const struct window_procs`). One
  per process, statically allocated.
- Signal handlers (we use the default install).
- The static-vec OMP threadmanager (owned by PufferLib's harness).

Each of these has to *not* be allocated through the per-env arena
(§4), or the first env to tear down will pull it out from under the
others. We audit this explicitly.

---

## 9. Operational

### Build

```bash
make -C vendor/nle/src/build nethack -j16     # libnethack.so
./build.sh nethack                            # puffer C extension
```

The puffer extension statically links libnethack (no dlopen at
training time). Standalone test binaries (below) dlopen instead.

### Run training

```bash
puffer train nethack --vec.total-agents 1024 --vec.num-buffers 1 \
    --vec.num-threads 1 --train.gpus 1 --train.horizon 64 \
    --train.minibatch-size 65536
```

### Standalone tools

All in `ocean/nethack/`. Build with `clang -O2 -Wall -std=gnu11
-I./vendor/nle/include -I./ocean/nethack <name>.c -o <name> -ldl
-lpthread -lm` (plus `-fopenmp` for `multi_threaded`).

| Binary | Purpose |
|---|---|
| `multi_threaded` | Pure-C OMP env-loop bench. Each thread runs one env's steps_per_env consecutively. Best-case env throughput; no harness. |
| `multi_threaded_rr` | Same as above, but one step per env per outer iter (puffer's pattern). Used to attribute cache-thrash cost. |
| `train_bench` | Serial bench with reset-on-done. Best per-env throughput reference. |
| `live_view` | Single env, ANSI-colored chars/colors grid + blstats + message + inventory. Modes: `--random`, `--interactive`, `--replay FILE`. |
| `replay_view` | ASCII-only render of a golden trajectory (uses recorded action stream, dumps chars + blstats per step). |
| `verify_determinism` | Record N seeds × 1000 steps to byte-hashed files; replay to check byte-identical reproduction. Used by `verify_determinism_all.sh`. |

### Determinism check

```bash
USER=$USER NETHACKDIR=$(pwd)/vendor/nle/nethackdir \
    bash ocean/nethack/verify_determinism_all.sh
# expects: "16/16 OK, all OK"
```

Re-capture goldens after any change that touches obs (`record-multi`
mode of `verify_determinism`).

### Adding a new per-env field

5-minute recipe:

1. Pick a name. Add the field at the end of `nle_ctx_t` in
   `vendor/nle/src/include/nle.h`. Scalar fields go inline; structs
   ≥ ~100 B go behind a pointer so the parent struct stays compact.

2. If pointer-typed: allocate in `init_nle()` (`vendor/nle/src/src/nle.c`)
   with `calloc(...)`; `assert` the pointer. Free in `nle_end()` if
   it's libc-malloc'd (skip if it lives in the per-env arena which
   gets `munmap`'d wholesale).

3. At the top of every `.c` file that referenced the old global, add
   the macro:
   - Scalar: `#define foo (current_nle_ctx->s_foo)`
   - Pointer-to-struct: `#define foo (*(struct foo_t *) current_nle_ctx->s_foo_p)`
   - Pointer-to-array: `#define foo (current_nle_ctx->s_foo_p)` (and
     never use `sizeof foo` after this — see §2)

4. Remove the `static`/`extern` declarations of the old global.
   For cross-TU globals, every file that had `extern foo` needs the
   `#define` too.

5. Watch for naming collisions with struct members in the same file
   (§2). Rename the global if needed.

6. Build (`make -C vendor/nle/src/build nethack -j16`,
   `./build.sh nethack`). Sanity-check (`./multi_threaded 64 5000 64`).
   Determinism check (`bash ocean/nethack/verify_determinism_all.sh`).

### Invariants the codebase relies on

- No process-global mutable state on the hot path. All env state is
  reached through `current_nle_ctx`.
- No mutexes, spinlocks, or atomic-fetch-add on the hot stepping path.
  Init-once gates use CAS for first-init-wins, but the read side is
  a plain load.
- No dlopen in production training. The standalone test binaries
  dlopen, but the puffer extension statically links libnethack.
- `current_nle_ctx` is `initial-exec` TLS — don't change to plain
  `__thread` without re-benchmarking.
- `_Static_assert(sizeof(struct eshk) == 4936, ...)` (and friends)
  in both `save.c` and `restore.c`. Any future change that breaks
  cross-TU agreement fails at compile time.
- Per-env arena lifetime is exactly `init_nle` → `nle_end`. Anything
  that has to outlive a single env's lifetime must not be allocated
  in the arena.
- Observation contract is independent of the rendering-toggle options
  (`status_updates`, etc.). All obs fields are populated directly
  from the in-memory state at every step.
