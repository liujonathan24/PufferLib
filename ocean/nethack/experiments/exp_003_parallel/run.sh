#!/bin/bash
# Scale-out microbench: N independent ./nethack profile processes in parallel.
# Each writes its own JSON. We sum valid_moves and divide by max wall.
#
# Usage: bash run.sh OUTDIR STEPS POLICY N1 [N2 ...]
set -eu
OUTDIR=$1; shift
STEPS=$1; shift
POLICY=$1; shift

mkdir -p "$OUTDIR"
# Always run from the repo root so ./nethack and ./vendor/nle/* resolve.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
cd "$REPO_ROOT"

for N in "$@"; do
    SUBDIR="$OUTDIR/N${N}"
    mkdir -p "$SUBDIR"
    rm -rf /tmp/nle-*
    echo "=== N=$N STEPS=$STEPS POLICY=$POLICY ==="
    START=$(date +%s.%N)
    pids=()
    for i in $(seq 0 $((N-1))); do
        ./nethack profile "$SUBDIR/p${i}.json" "$STEPS" "$POLICY" "$((0xC0FFEE + i))" \
            > "$SUBDIR/p${i}.log" 2>&1 &
        pids+=($!)
    done
    for p in "${pids[@]}"; do wait "$p"; done
    END=$(date +%s.%N)
    echo "    wall=$(awk -v s=$START -v e=$END 'BEGIN{printf "%.3f", e-s}')s"
done
