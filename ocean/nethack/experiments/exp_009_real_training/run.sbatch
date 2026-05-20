#!/bin/bash
#SBATCH --job-name=nh_train
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=32
#SBATCH --mem-per-cpu=2G
#SBATCH --time=00:45:00
#SBATCH --partition=cpu
#SBATCH --output=ocean/nethack/experiments/exp_009_real_training/train.out

set -eu
cd "$SLURM_SUBMIT_DIR"

OUT=ocean/nethack/experiments/exp_009_real_training
mkdir -p "$OUT"

source /usr/share/Modules/init/bash
module load intel/2024.2
source /opt/intel/oneapi/compiler/2024.2/env/vars.sh > /dev/null 2>&1
export LD_LIBRARY_PATH=/opt/intel/oneapi/compiler/2024.2/lib:${LD_LIBRARY_PATH:-}

# Fresh uv venv on the compute node's local /tmp so installs are fast.
VENV=/tmp/${USER}_puffer_$$
/scratch/gpfs/ZHUANGL/jl0796/bin/uv venv "$VENV" --python 3.11 > /dev/null
"$VENV/bin/python" -m ensurepip --quiet
"$VENV/bin/pip" install --quiet torch numpy pybind11 rich rich_argparse scikit-learn gpytorch wandb 2>&1 | tail -3

# Build the nethack _C.so using our local repo.
export NETHACK_LIBPATH="$PWD/vendor/nle/lib/libnethack.so"
export NETHACKDIR="$PWD/vendor/nle/nethackdir"
export OMP_NUM_THREADS=$SLURM_CPUS_PER_TASK
PYTHONPATH="$PWD" "$VENV/bin/python" -c "import pybind11; print('pybind11 from:', pybind11.get_include())"
PYTHONPATH="$VENV/lib/python3.11/site-packages" bash build.sh nethack --cpu 2>&1 | tail -3

echo "host: $(hostname)  cpus: $OMP_NUM_THREADS"
date

# Run a small training session, --slowly to use torch backend (no CUDA).
# Tiny config so we see throughput in ~5-10 minutes.
PYTHONPATH="$PWD" "$VENV/bin/python" -u -m pufferlib.pufferl train nethack \
    --slowly \
    --vec.total_agents 256 \
    --vec.num_buffers 1 \
    --train.total_timesteps 200000 \
    --train.horizon 64 \
    --train.minibatch_size 2048 \
    --train.gpus 0 \
    2>&1 | tee "$OUT/train.log"

date
echo "DONE"

# Save a quick analysis script that reads the log for valid_moves rate etc.
rm -rf "$VENV"
