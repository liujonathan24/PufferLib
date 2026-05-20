#!/bin/bash
#SBATCH --job-name=nh_scaling
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=64
#SBATCH --mem-per-cpu=2G
#SBATCH --time=00:30:00
#SBATCH --partition=cpu
#SBATCH --output=ocean/nethack/experiments/exp_004_srun_scaling/scaling.out

set -eu
cd "$SLURM_SUBMIT_DIR"
OUT=ocean/nethack/experiments/exp_004_srun_scaling
mkdir -p "$OUT"

echo "host: $(hostname)   cpus: $SLURM_CPUS_PER_TASK"
date

bash ocean/nethack/experiments/exp_003_parallel/run.sh \
    "$OUT/random_50k" 50000 random 1 2 4 8 16 32 64
bash ocean/nethack/experiments/exp_003_parallel/run.sh \
    "$OUT/wait_50k" 50000 wait 1 2 4 8 16 32 64

date
echo "DONE"
