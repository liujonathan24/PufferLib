#!/bin/bash
set -u
cd /scratch/gpfs/ZHUANGL/jl0796/PufferLib
DIR=ocean/nethack/experiments/exp_039_goal_8h
for N in 64 128 256 512 1024; do
  bash $DIR/run_sps.sh $N 60 baseline 2>/dev/null
  sleep 5
done
echo "DONE" > $DIR/baseline_done.flag
