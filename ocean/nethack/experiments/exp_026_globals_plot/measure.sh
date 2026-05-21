#!/bin/bash
# Iterate commits in this refactor branch, rebuild libnethack at each,
# and record the sizes of the writable-global sections
# (.data + .bss + .tdata + .tbss). Read-only sections (.rodata,
# .data.rel.ro) are excluded — they're shared-safe.
#
# Output: a CSV at $OUT_CSV with one row per commit.

set -u
cd "$(git rev-parse --show-toplevel)"

OUT_DIR=ocean/nethack/experiments/exp_026_globals_plot
OUT_CSV="$OUT_DIR/sizes.csv"
mkdir -p "$OUT_DIR"

# Commit range — first refactor commit (and its parent for the baseline)
# through HEAD.
FIRST=aec522d6
RANGE="$FIRST^..HEAD"
HEAD_BEFORE=$(git rev-parse HEAD)

# Resolve the parent as the baseline commit
BASELINE=$(git rev-parse "${FIRST}^")

declare -a COMMITS=("$BASELINE")
while read -r line; do
    COMMITS+=("$line")
done < <(git log --format='%H' --reverse "$RANGE")

echo "iterating ${#COMMITS[@]} commits, current HEAD = $HEAD_BEFORE"

echo "idx,sha,short,data,bss,tdata,tbss,total,subject" > "$OUT_CSV"

# Ensure cmake's CMakeFiles/nethack.dir/{src,win,sys}/ dirs exist
# (a previous run may have deleted them; --clean-first won't recreate
# the subdirs that store the .o.d depfiles).
mkdir -p vendor/nle/src/build/CMakeFiles/nethack.dir/src \
         vendor/nle/src/build/CMakeFiles/nethack.dir/win/tty \
         vendor/nle/src/build/CMakeFiles/nethack.dir/win/rl \
         vendor/nle/src/build/CMakeFiles/nethack.dir/sys/share \
         vendor/nle/src/build/CMakeFiles/nethack.dir/sys/unix 2>/dev/null

idx=0
for sha in "${COMMITS[@]}"; do
    short=$(git rev-parse --short "$sha")
    subject=$(git log -1 --format='%s' "$sha" | tr ',' ';')
    echo "[$idx/$((${#COMMITS[@]}-1))] $short  $subject"

    git checkout -q "$sha" -- vendor/nle/src 2>&1 || { echo "  checkout failed"; ((idx++)); continue; }

    rm -f vendor/nle/src/build/libnethack.so 2>/dev/null

    BUILD_LOG="$OUT_DIR/.build.log"
    # --clean-first removes all .o under the nethack target before
    # rebuilding, so the .so we measure reflects the actual sources at
    # this commit, not whatever was cached from a previous iteration.
    (cd vendor/nle/src/build && cmake --build . --target nethack --clean-first -j8 2>&1) > "$BUILD_LOG"
    if [ ! -f vendor/nle/src/build/libnethack.so ]; then
        echo "  build failed (see $BUILD_LOG)"
        git checkout -q "$HEAD_BEFORE" -- vendor/nle/src 2>&1
        ((idx++))
        continue
    fi

    # Extract writable-section sizes. Match the section name as a
    # whole word (`size --format=sysv` lists `.data.rel.ro` separately
    # and we want only the plain `.data`).
    sizes=$(size --format=sysv vendor/nle/src/build/libnethack.so 2>/dev/null)
    data=$(echo "$sizes"   | awk '$1=="\.data"   {print $2}' | head -1)
    bss=$(echo "$sizes"    | awk '$1=="\.bss"    {print $2}' | head -1)
    tdata=$(echo "$sizes"  | awk '$1=="\.tdata"  {print $2}' | head -1)
    tbss=$(echo "$sizes"   | awk '$1=="\.tbss"   {print $2}' | head -1)
    data=${data:-0}; bss=${bss:-0}; tdata=${tdata:-0}; tbss=${tbss:-0}
    total=$((data + bss + tdata + tbss))

    echo "$idx,$sha,$short,$data,$bss,$tdata,$tbss,$total,\"$subject\"" >> "$OUT_CSV"
    echo "  data=$data bss=$bss tdata=$tdata tbss=$tbss total=$total"
    ((idx++))
done

# Restore HEAD
git checkout -q "$HEAD_BEFORE" -- vendor/nle/src
(cd vendor/nle/src/build && cmake --build . --target nethack -j8 2>&1) > /dev/null

echo "wrote $OUT_CSV"
