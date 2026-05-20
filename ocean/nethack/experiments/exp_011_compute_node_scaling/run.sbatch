#!/bin/bash
#SBATCH --job-name=nh_scal2
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=64
#SBATCH --mem-per-cpu=2G
#SBATCH --time=00:30:00
#SBATCH --partition=cpu
#SBATCH --output=ocean/nethack/experiments/exp_011_compute_node_scaling/scaling.out

set -eu
cd "$SLURM_SUBMIT_DIR"
OUT=ocean/nethack/experiments/exp_011_compute_node_scaling
mkdir -p "$OUT"

echo "host: $(hostname)   cpus: $SLURM_CPUS_PER_TASK"
date
nproc

# Use the existing nethack binary (built with -DNETHACK_PROFILE=1).
ls -la nethack

# Use a hot CPU set under our control.
for POL in random wait north; do
    bash ocean/nethack/experiments/exp_003_parallel/run.sh \
        "$OUT/${POL}_100k" 100000 "$POL" 1 2 4 8 16 32 64
done

date
echo "DONE"
