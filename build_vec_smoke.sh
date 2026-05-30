#!/usr/bin/env bash
# Build the standalone vec_smoke repro for the NetHack vecenv against the
# (production) libnethack.so. Drives N envs through many reset/step cycles to
# stress the concurrent slow-reset path.
#
#   make -C vendor/nle/src/build nethack -j8      # (re)build libnethack.so
#   bash build_vec_smoke.sh                        # build ./vec_smoke
#   NETHACKDIR="$PWD/vendor/nle/src/build/dat" OMP_NUM_THREADS=8 \
#       ./vec_smoke 64 20000 2                      # N steps force_reset_every
#
# Run under valgrind/memcheck for memory-safety checking:
#   NETHACKDIR=... OMP_NUM_THREADS=2 valgrind --tool=memcheck ./vec_smoke 16 6000 2
set -euo pipefail
cd "$(dirname "$0")"
LIBDIR=vendor/nle/src/build
gcc -O1 -g -fno-omit-frame-pointer -Wall -Wno-unused-function -std=gnu11 \
    -I./vendor/nle/include -I./ocean/nethack -I./src \
    ocean/nethack/vec_smoke.c -o vec_smoke \
    -L"$LIBDIR" -lnethack -Wl,-rpath,"$LIBDIR" \
    -fopenmp -ldl -lpthread -lm
echo "built ./vec_smoke against $LIBDIR/libnethack.so"
