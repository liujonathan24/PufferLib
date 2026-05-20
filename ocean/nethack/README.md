## NetHack NLE environment for Pufferlib


Observation space:
| Key | Shape | dtype |
|---|---|---|
| `glyphs` | (21, 79) | int16 |
| `chars` | (21, 79) | uint8 |
| `colors` | (21, 79) | uint8 |
| `specials` | (21, 79) | uint8 |
| `blstats` | (NLE_BLSTATS_SIZE,) | int64 |
| `message` | (256,) | uint8 |
| `inv_glyphs` | (55,) | int16 |
| `inv_strs` | (55, 80) | uint8 |
| `inv_letters` | (55,) | uint8 |
| `inv_oclasses` | (55,) | uint8 |

## NLE `blstats` Layout

**Coordinate Data (2)**
- `0` — Hero's *x* coordinate
- `1` — Hero's *y* coordinate

**Character Stats (23)**
- `2` — Strength percentage
- `3` — Strength value
- `4–8` — Dexterity, Constitution, Intelligence, Wisdom, Charisma
- `9` — Score
- `10–11` — Current Hitpoints, Max Hitpoints
- `12–14` — Depth, Gold, Energy (Power / Mana)
- `15–16` — Max Energy, Armor Class (AC)
- `17–18` — Monster level, Experience level
- `19–20` — Experience points, Time (turns elapsed)
- `21–24` — Hunger state, Carrying capacity, Dungeon number, Level number

Action space:


Rewards:
