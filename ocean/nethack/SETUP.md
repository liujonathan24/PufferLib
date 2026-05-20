# NetHack env — setup

The NetHack environment binds to `libnethack.so` from facebookresearch/NLE
0.9.1 via dlopen-per-instance (so each env owns its own copy of NetHack's
many globals). The `.so` and game-data are large/binary and not committed
to the repo. To populate `vendor/nle/` on a fresh checkout:

```bash
# Requires `nle==0.9.1` already installed (e.g. via `uv pip install nle`).
# Locate the wheel install and its sdist (the sdist has the public headers).
WHEEL=$(python -c "import nle, os; print(os.path.dirname(nle.__file__))")
SDIST=$(find $HOME -path "*/sdists-v9/pypi/nle/*/src/include/nleobs.h" -print -quit 2>/dev/null)
SDIST=${SDIST%/include/nleobs.h}

mkdir -p vendor/nle/include vendor/nle/lib
cp $WHEEL/libnethack.so vendor/nle/lib/
cp $SDIST/include/nleobs.h vendor/nle/include/
ln -sfn $WHEEL/nethackdir vendor/nle/nethackdir
```

The committed header `vendor/nle/include/nleobs.h` is provided so the
above only needs to overwrite if there's a version mismatch.

## Build

Standalone (debug, no Python):
```bash
bash build.sh nethack --local              # with sanitizers
bash build.sh nethack --fast               # release
EXTRA_CFLAGS="-DNETHACK_PROFILE=1" \
  bash build.sh nethack --fast              # release with profiler
```

Python `_C.so` (needs `intel/2024.2` for libiomp5):
```bash
module load intel/2024.2
source /opt/intel/oneapi/compiler/2024.2/env/vars.sh
bash build.sh nethack --cpu                 # or default (CUDA) if available
```

## Observation selection (compile-time)

Default: chars-only (1659-byte ByteTensor). Override with `-D`:
```bash
EXTRA_CFLAGS="-DNETHACK_USE_CHARS=1 -DNETHACK_USE_BLSTATS=1 -DNETHACK_USE_MESSAGE=1" \
  bash build.sh nethack --fast
```
Available flags: `NETHACK_USE_{CHARS, COLORS, SPECIALS, GLYPHS, BLSTATS, MESSAGE, INV}`.
Fields with `=0` are not allocated, not bound (NLE skips writing them),
and not packed into the observation tensor.

## Standalone subcommands

```bash
./nethack [N]                               # interactive driver, N steps with rendering
./nethack record OUT.txt N [random|wait]    # ASCII trajectory log
./nethack bench N [random|wait]             # quick throughput bench
./nethack resets N                          # reset-only bench
./nethack profile OUT.json N [random|wait]  # full profiler dump (needs PROFILE=1 build)
```

## Experiments

`experiments/exp_NNN_*/` — one folder per profiling experiment, each
contains `NOTES.md` (hypothesis, result, decision) plus the raw JSON.
