# NetHack env — setup

## Prerequisites

The modified NLE source lives in a separate repo and must be cloned
into `vendor/nle/`:

```bash
# From the PufferLib root:
git clone https://github.com/liujonathan24/NetHack.git vendor/nle
```

No pip install of NLE is needed — it builds from source.

## Build libnethack.so from source

```bash
# First time: run cmake
cd vendor/nle/src && mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cd ../../../..

# Build
make -C vendor/nle/src/build nethack -j$(nproc)
```

This produces `vendor/nle/src/build/libnethack.so` and `vendor/nle/src/build/dat/nhdat`.

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
# Live viewer (interactive play / random agent / replay)
clang -O2 -I vendor/nle/src/include -I vendor/nle/src/build/include \
    -I vendor/nle/src/third_party/deboost.context/include \
    -DDEFAULT_WINDOW_SYS=\"rl\" -DDLB -DNLE_ALLOW_SEEDING \
    -DNLE_PER_ENV_FILES=1 -DNLE_PER_ENV_FLAGS=1 -DNLE_USE_ARENA_FREE=1 \
    -DNLE_USE_TILES -DNOCLIPPING -DNOCWD_ASSUMPTIONS -DNOMAIL -DNOTPARMDECL \
    -DNETHACK_USE_BLSTATS=1 \
    ocean/nethack/live_view.c -o live_view \
    -L./vendor/nle/src/build -lnethack -lm -lbz2 -lpthread \
    -Wl,-rpath=$(pwd)/vendor/nle/src/build
```

## Environment variables

| Variable | Default | Purpose |
|---|---|---|
| `NETHACKDIR` | `$(pwd)/vendor/nle/src/build/dat` | Path to directory containing `nhdat` |
| `USER` | (from env) | Required by NetHack for save file naming |

## Quick smoke test

```bash
# Interactive play
NETHACKDIR=$(pwd)/vendor/nle/src/build/dat ./live_view -i

# Random agent
NETHACKDIR=$(pwd)/vendor/nle/src/build/dat ./live_view --random --steps 200

# Watch one env run
# Quick training test
puffer train nethack \
    --vec.total-agents 64 --vec.num-buffers 1 --vec.num-threads 4 \
    --train.gpus 1 --train.total-timesteps 1000000 --train.minibatch-size 64
```
