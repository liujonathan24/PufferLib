# NetHack env — setup

## Prerequisites

`vendor/nle/` contains the vendored NLE source tree with our
`nle_ctx_t` per-env refactor. It builds from source — no pip install
of NLE is needed.

## Build libnethack.so from source

```bash
make -C vendor/nle/src/build nethack -j16
```

This produces `vendor/nle/src/build/libnethack.so`.

## Build the PufferLib extension

Standalone (debug, no Python):
```bash
bash build.sh nethack --local              # with sanitizers
bash build.sh nethack --fast               # release
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

## Build standalone tools

```bash
# Live viewer
clang -O2 -Wall -std=gnu11 -I./vendor/nle/include -I./ocean/nethack \
    ocean/nethack/live_view.c -o live_view \
    -L./vendor/nle/src/build -lnethack \
    -Wl,-rpath=$PWD/vendor/nle/src/build \
    -ldl -lpthread -lm

# OMP throughput bench
clang -O2 -Wall -fopenmp -std=gnu11 -I./vendor/nle/include -I./ocean/nethack \
    ocean/nethack/multi_threaded.c -o multi_threaded \
    -L./vendor/nle/src/build -lnethack \
    -Wl,-rpath=$PWD/vendor/nle/src/build \
    -ldl -lpthread -lm

# Determinism harness (built automatically by verify_determinism_all.sh)
clang -O2 -Wall -std=gnu11 -I./vendor/nle/include -I./ocean/nethack \
    ocean/nethack/verify_determinism.c -o verify_determinism \
    -L./vendor/nle/src/build -lnethack \
    -Wl,-rpath=$PWD/vendor/nle/src/build \
    -ldl -lpthread -lm
```

## Environment variables

| Variable | Default | Purpose |
|---|---|---|
| `NETHACKDIR` | `./vendor/nle/nethackdir` | Path to NetHack data files |
| `USER` | (from env) | Required by NetHack for save file naming |

## Quick smoke test

```bash
# Determinism (should print "16/16 OK, all OK")
USER=$USER NETHACKDIR=$(pwd)/vendor/nle/nethackdir \
    bash ocean/nethack/verify_determinism_all.sh

# Watch one env run
USER=$USER NETHACKDIR=$(pwd)/vendor/nle/nethackdir \
    ./live_view --random --steps 200

# OMP env-loop ceiling
USER=$USER NETHACKDIR=$(pwd)/vendor/nle/nethackdir \
    ./multi_threaded 1024 3000 128
```
