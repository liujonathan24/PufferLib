# NetHack reward shaping — recipe and reference

This document is the canonical reference for the NetHack reward signal in
PufferLib. Read it before adding a new reward term. The header file
`ocean/nethack/nethack.h` is the source of truth for behavior; this doc
explains *why* the code is shaped the way it is and gives a copy-paste
recipe for adding the next term.

All line-number citations in this doc match the current `nethack.h` as of
the commit that introduced the runtime-coef refactor. If you move things,
update the references.

---

## 1. Where the reward lives

There is exactly one place where `env->rewards[0]` is written per step:
**`c_step` in `nethack.h`**, line **895**.

Everything above that line is bookkeeping (action injection, prompt drain,
turn counter, score/depth/position read). Everything below is "did the
episode end" + observation pack.

### 1.1 The signal sources

The reward is built from NLE's `blstats` array, which lives at
`env->blstats[]` (if `NETHACK_USE_BLSTATS` is on) or `env->hook_blstats[]`
(the always-on hook copy). The indexes are defined in
`vendor/nle/include/nleobs.h`:

| Field          | NLE index            | What it is                                      |
|----------------|----------------------|-------------------------------------------------|
| score          | `NLE_BL_SCORE` (9)   | NetHack's running score (kills + depth + gold)  |
| HP             | `NLE_BL_HP` (10)     | current hit points                              |
| HP max         | `NLE_BL_HPMAX` (11)  |                                                 |
| dungeon depth  | `NLE_BL_DEPTH` (12)  | absolute depth, 1 = entry level                 |
| gold           | `NLE_BL_GOLD` (13)   | gold pieces carried                             |
| energy         | `NLE_BL_ENE` (14)    | current mana                                    |
| AC             | `NLE_BL_AC` (16)     | armor class (lower is better)                   |
| XP level       | `NLE_BL_XP` (18)     | character experience level                      |
| XP points      | `NLE_BL_EXP` (19)    | experience points                               |
| time           | `NLE_BL_TIME` (20)   | turn counter; advances on valid moves           |
| hunger         | `NLE_BL_HUNGER` (21) | 0=satiated, 1=normal, 2=hungry, 3=weak, ...     |
| dungeon number | `NLE_BL_DNUM` (23)   | which branch (main, mines, gnomish, …)          |
| dlevel         | `NLE_BL_DLEVEL` (24) | level within branch                             |
| condition mask | `NLE_BL_CONDITION` (25) | bitmask: stoned, slimed, confused, ...       |

The current reward reads `score`, `depth`, and the player position
(`NLE_BL_X`, `NLE_BL_Y`) at lines **849–855**.

### 1.2 Why `current_nle_ctx` does not appear

NLE-side internals (e.g. `u.uhp`, `u.uconduct.killer_count`, `u.uz.dnum`)
live inside the per-env `nle_ctx_t` struct. We do **not** dereference
that pointer directly from the wrapper — NLE exposes only what it serializes
into `nle_obs` (the blstats array, message buffer, misc flags, glyphs,
chars, inv). If you want a reward that needs a variable not in blstats,
you have two choices:

1. **Preferred:** find an existing field in blstats / misc / message
   that proxies for what you want (e.g. score-delta covers monster kills
   indirectly).
2. **Last resort:** extend NLE's observation builder
   (`vendor/nle/src/win/rl/winrl.cc` `fill_obs`) to publish the desired
   variable into one of the obs buffers, then read it from the wrapper.
   This requires rebuilding `libnethack.so` and is out-of-scope for typical
   shaping work.

There is no per-env `current_nle_ctx` global — each `Nethack` instance
holds its own `env->ctx` (line **247**). Multiple envs run safely in
parallel because every NLE per-game global has been migrated into
`nle_ctx_t` (see project memory note "NetHack refactor goal").

---

## 2. The step lifecycle

### 2.1 Reset (per episode)

`c_reset` → `nethack_reset_bookkeeping` (lines **619–646**) clears all
per-episode state including the reward caches:

```
env->prev_score      = 0
env->prev_depth      = blstats[NLE_BL_DEPTH] if BLSTATS bound else 1
env->episode_return  = 0
env->episode_length  = 0
env->rewards[0]      = 0
env->terminals[0]    = 0
```

Anything you cache for a delta-reward must be zeroed (or reset to its
initial NLE value) here. The reward-shaping coefficients themselves
(`score_coef` etc.) are **not** reset — they are set once in `my_init`
from kwargs (line **538**) and persist across episodes.

### 2.2 Step

`c_step` (lines **818–911**) does, in order:

1. Inject the agent's action (`env->obs.action`) and call `fn_step`.
2. Drain sub-prompts via ESC (lines **854–873**), marking `illegal=1`.
3. Read `score`, `depth`, `(px, py)` from blstats (lines **849–857**).
4. Compute the reward (lines **864–891**).
5. Update caches: `env->prev_score = score; env->prev_depth = depth;`
   (lines **892–893**).
6. Write `env->rewards[0] = reward;` and accumulate
   `env->episode_return += reward;` (lines **895–896**).
7. If `env->obs.done`: set terminal, call `nethack_add_log` (which logs
   the cumulative score/depth/episode_return), pack obs, recursively call
   `c_reset` (lines **898–908**).

### 2.3 Episode end (`done`)

When NLE reports `obs.done`, `nethack_add_log` (line **581**) folds the
final stats into `env->log`. Notably it does **not** include the reward
itself — it logs `episode_return` separately. If you add a new term and
want it visible in the Puffer UI, add a counter to `Log` (line **215**)
and `my_log` in `binding.c`.

---

## 3. Recipe — adding a new reward term

Every reward term follows the same five-step pattern.

### Step 1. Identify the NLE variable

Find the field in `blstats` (or extend NLE; see §1.2). Confirm it
updates **every step** — some NLE-side fields are recomputed lazily
(e.g. `NLE_BL_HUNGER` only changes every ~20 turns).

### Step 2. Cache the previous value in the `Nethack` struct

Add a field to `Nethack` (nethack.h around line **300**). Example:

```c
long prev_gold;
```

### Step 3. Reset the cache in `nethack_reset_bookkeeping`

Add the zeroing to `nethack_reset_bookkeeping` (line **619**):

```c
env->prev_gold = 0;
// Or: env->prev_gold = (NETHACK_USE_BLSTATS ? env->blstats[NLE_BL_GOLD] : 0);
```

### Step 4. Compute the delta in `c_step` and add `coef * delta` to reward

Read the variable just after lines **849–857**:

```c
long gold = NETHACK_USE_BLSTATS ? env->blstats[NLE_BL_GOLD]
                                : env->hook_blstats[NLE_BL_GOLD];
```

Then *after* the existing reward terms (line **891**) but *before*
`env->prev_score = score;` (line **892**), add:

```c
if (gold > env->prev_gold) {
    reward += env->gold_coef * (float)(gold - env->prev_gold);
}
env->prev_gold = gold;
```

Use `>` (one-shot delta on increase) rather than raw signed delta unless
you want to penalize gold loss too. Reward hacking note: pure delta
without a floor can let the agent farm reversible state changes — see §6.

### Step 5. Expose the coefficient

Three files:

**5a.** `nethack.h` struct — add the field next to `score_coef` (around
line **316**):

```c
float gold_coef;   // default 0.001 — gold delta per step
```

**5b.** `nethack.h init()` — set the default (around line **539**):

```c
env->gold_coef = 0.001f;
```

**5c.** `binding.c my_init` — read the kwarg:

```c
nethack_read_coef(kwargs, "gold_coef", &env->gold_coef);
```

**5d.** `config/nethack.ini` `[env]` section:

```ini
gold_coef = 0.001
```

Once added, `puffer train nethack --env.gold-coef 0.005` works
automatically (argparse converts `_` → `-` and prefixes the section name).

---

## 4. Currently wired terms

| Term            | CLI flag                 | Default | Where computed (`nethack.h` line) |
|-----------------|--------------------------|--------:|-----------------------------------|
| score-delta     | `--env.score-coef`       |    0.01 | 866                               |
| descent         | `--env.descent-coef`     |    1.0  | 876                               |
| scout (new tile)| `--env.scout-coef`       |    0.1  | 886                               |
| illegal action  | `--env.illegal-penalty`  |   -0.5  | 891                               |

Set any coefficient to `0` (or `0.0`) to disable that term entirely.
Defaults live in `init()` (line **538**) so partial config files still
build; the `[env]` section in `config/nethack.ini` overrides them at
training time.

---

## 5. High-value future reward candidates

Ranked roughly by signal/effort ratio.

| Term            | NLE var            | Coef guess | Shape                     | Notes |
|-----------------|--------------------|-----------:|---------------------------|-------|
| **XP-level**    | `NLE_BL_XP`        |       0.5  | one-shot on increase      | Most direct progress signal; gates whole game phases (XL 5 = able to fight). Resets to 1 on death so safe to delta-reward. |
| **XP-points**   | `NLE_BL_EXP`       |     0.001  | per-step delta (positive) | Smoother than XL but noisier; good auxiliary. |
| **HP loss penalty** | `NLE_BL_HP`    |     -0.01  | per-step delta when HP↓   | Discourages running into stronger monsters. Pair with descent so agent doesn't stall by sitting still (HP doesn't change → 0 penalty). |
| **Hunger**      | `NLE_BL_HUNGER`    |     -0.05  | per-step penalty if hunger ≥ 2 (Hungry+) | Encourages eating. Lazy update — value only changes every ~20 turns. |
| **Gold delta**  | `NLE_BL_GOLD`      |     0.001  | per-step delta (positive) | Cheap signal but already partially covered by score-delta. |
| **Depth × score** | `NLE_BL_DEPTH × NLE_BL_SCORE` | depends | one-shot at depth change, log(score) bonus | Combines exploration with combat — discourages descent-spam without engagement. |
| **Death penalty** | `obs.done && obs.how_done` | -10   | one-shot at episode end if died (not won) | Currently absent — episodes end "silently". Pair with descent to prevent reward hacking (see §6). |
| **Monster kills** | (extend NLE: `u.uconduct.killer` or `mvitals[]`) | 0.1 | one-shot per kill | Requires NLE-side patch; score-delta is a partial proxy. Skipped for now. |
| **Item identify** | (extend NLE: `u.uconduct` or scan inv) | 0.05 | one-shot per new oclass identified | Requires NLE patch. |
| **Level exploration** | already in scout_coef | already wired | per-step delta on new (row,col) | Done. To extend to inter-level: clear `env->visited` on dnum change as well as dlevel change (line 871 currently only checks depth). |

For each candidate the workflow is identical to §3 — five steps.

---

## 6. Common pitfalls

### 6.1 Reward hacking

- **Descent without death penalty.** An agent can learn to charge down
  stairs while critically wounded and die on dlvl 3. It still
  accumulates `+1` for each descent and starts a fresh episode. With no
  penalty for dying, this dominates other strategies. Mitigation: add a
  death penalty (`-10` is a reasonable starting point) at episode end.
- **Reversible state churn.** Avoid delta-rewards on variables that can
  go up *and* down with no game-state progress. Example: rewarding raw
  `score - prev_score` charges the agent on score decreases (e.g.
  cursed item drops). Currently `score_coef * (score - prev_score)`
  can go negative — that is intentional, but be aware when adding new
  delta terms.
- **Stay-alive farming.** Pure survival reward + no descent pressure =
  agent sits in dlvl 1 forever. The current set of coefs (descent +
  scout) breaks this, but if you set `descent_coef = 0` and
  `scout_coef = 0` you reproduce the old "ep_return = 5 ceiling"
  symptom (see `experiments/exp_032_hyperparam/RESULT.md`).

### 6.2 Per-env vs global

There is **no shared global state** for reward terms. Every `Nethack`
struct has its own `score_coef`, `prev_score`, `visited[]`. The wrapper
runs N envs through pthreads; if you accidentally introduce a
`static` cache in `c_step`, two envs will share it and the reward will
be wrong. (This is the same class of bug that motivated the
NetHack global → `nle_ctx_t` migration; see project memory
"NetHack refactor goal".)

### 6.3 Reset clears the cache

`nethack_reset_bookkeeping` (line **619**) zeros `prev_score`,
`prev_depth`, `episode_return`, the visited bitmap, etc. If you forget
to zero a new cache field, the first step of episode N+1 will see a
huge delta from the final state of episode N and emit a spurious
reward spike. Test this by running with `--vec.total-agents 1
--train.horizon 16` and watching `episode_return` after an early death.

### 6.4 Lazy-updated NLE variables

Not every blstats field updates every step:

- `NLE_BL_HUNGER` changes only every ~20 game turns.
- `NLE_BL_XP` (character level) only changes on level-up.
- `NLE_BL_DEPTH` only changes on stair-traverse.
- `NLE_BL_HP` updates every step (regen + damage).
- `NLE_BL_SCORE` updates whenever a scoring event fires (every move
  during normal play because of the per-turn score formula).

For lazy fields, the one-shot pattern (`if (new > old) reward += ...`)
is correct. For per-step delta patterns on lazy fields, you'll get
zero reward most steps and a spike once per change — equivalent to
one-shot, just more code.

### 6.5 Time-counter pitfall

`NLE_BL_TIME` does *not* advance every `c_step` — only on "valid moves"
(steps that don't dump you into a prompt). This is what
`env->episode_valid_moves` tracks. Don't normalize rewards by
`env->tick` if you mean game-turns; use `env->episode_valid_moves` or
`nethack_current_time(env)`.

### 6.6 Score-delta covers more than you think

`NLE_BL_SCORE` already encodes:
- monster kills (weighted by monster level)
- experience points
- depth bonuses
- gold pickup
- successful prayer

So `score_coef > 0` already gives the agent a (noisy) signal for all of
those. Resist the urge to layer narrow rewards on top without checking
whether score-delta already covers it.

---

## 7. How to test a new reward

1. **Build:**
   ```
   make -C vendor/nle/src/build nethack -j8
   bash build.sh nethack
   ```

2. **Smoke-test startup** (small config from
   `experiments/exp_032_hyperparam/run_hp.sh`):
   ```
   . /etc/profile.d/modules.sh; module load intel-oneapi/2024.2
   source .venv/bin/activate
   export CUDA_HOME=/usr/local/cuda-12.8
   export PATH=$CUDA_HOME/bin:$PATH
   export LD_LIBRARY_PATH=/opt/intel/oneapi/compiler/2024.2/lib:$CUDA_HOME/lib64:${LD_LIBRARY_PATH:-}

   puffer train nethack \
       --vec.total-agents 512 --vec.num-buffers 1 --vec.num-threads 1 \
       --policy.hidden-size 128 --policy.num-layers 2 \
       --train.clip-coef 0.20 --train.ent-coef 0.10 \
       --env.score-coef 0.01 --env.descent-coef 1.0 \
       --env.scout-coef 0.1 --env.illegal-penalty -0.5 \
       --train.gpus 1 --train.total-timesteps 1000000 \
       --train.minibatch-size 32768 --train.horizon 64
   ```
   Watch the first 30s: `episode_return` should be nonzero,
   `new_tiles` should grow, no segfaults.

3. **Curve logging.** Use `experiments/exp_032_hyperparam/run_hp.sh` as a
   template — it already streams `episode_return`, `depth`, `new_tiles`,
   `valid_moves`, `illegal_actions` to a `*_curve.csv` every 30 seconds.
   Make a new experiment dir, copy the script, point CSV at a new path,
   tweak the `--env.*` coefs, and run for an hour. Plot `depth` vs
   `episode_return` to confirm the new term is changing behavior, not
   just rescaling the same trajectory.

4. **Ablation.** Always run a control with the new term zeroed out
   (`--env.<your_coef> 0`) alongside the active version. Same seed
   ideally. Without an ablation, you can't tell whether the new term
   actually helped or whether the run lucked into a better seed.

---

## 8. Quick reference

```
nethack.h:215     struct Log              — per-episode logging fields
nethack.h:227     struct Nethack          — per-env state, holds coefs at line 316
nethack.h:516     init()                  — coef defaults at line 538
nethack.h:603     nethack_reset_bookkeeping — per-episode reset
nethack.h:818     c_step                  — reward computed at lines 864–891
nethack.h:895     env->rewards[0] = ...   — THE reward write
binding.c:11      my_init                 — reads kwargs (per-env, per-init)
config/nethack.ini  [env] section        — defaults; CLI overrides as --env.<key>
vendor/nle/include/nleobs.h  NLE_BL_*    — blstats field indices
```

Keep this file updated whenever you add a coef — the CLI, the ini, and
the table in §4 must all line up or future-you will be confused.
