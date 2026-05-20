#!/bin/bash
#SBATCH --job-name=nh_gpu
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=16
#SBATCH --gres=gpu:1
#SBATCH --mem-per-cpu=4G
#SBATCH --time=00:30:00
#SBATCH --output=ocean/nethack/experiments/exp_012_gpu_training/train.out

set -u
cd "$SLURM_SUBMIT_DIR"

OUT=ocean/nethack/experiments/exp_012_gpu_training
mkdir -p "$OUT"

source /usr/share/Modules/init/bash 2>&1 || true
module load intel/2024.2 cudatoolkit/12.8 2>&1 | tail -3

LIBOMP_DIR="${CMPLR_ROOT:-/opt/intel/oneapi/compiler/2024.2}/lib"
export LD_LIBRARY_PATH="${LIBOMP_DIR}:${LD_LIBRARY_PATH:-}"
export NETHACK_LIBPATH="$PWD/vendor/nle/lib/libnethack.so"
export NETHACKDIR="$PWD/vendor/nle/nethackdir"
export OMP_NUM_THREADS=$SLURM_CPUS_PER_TASK

echo "host: $(hostname)"
echo "cpus: $SLURM_CPUS_PER_TASK"
nvidia-smi -L 2>&1 | head -2
which nvcc
nvcc --version 2>&1 | tail -1
date

VENV=$PWD/.venv
if [ ! -x "$VENV/bin/python" ]; then
    echo "ERROR: $VENV/bin/python missing — run uv venv first" >&2
    exit 2
fi
echo "venv python: $($VENV/bin/python --version)"
$VENV/bin/python -c "import torch; print('torch=', torch.__version__, 'cuda=', torch.cuda.is_available(), 'devs=', torch.cuda.device_count())"

# Rebuild _C.so with CUDA bindings (default mode, no --cpu flag).
# Use system NCCL + cuDNN if present, else fall back to wheel-provided.
PYTHONPATH="$VENV/lib/python3.11/site-packages" bash build.sh nethack 2>&1 | tail -8
ls -la pufferlib/_C.cpython-311-*.so

# Run training on GPU.
"$VENV/bin/python" -u -m pufferlib.pufferl train nethack \
    --vec.total-agents 256 \
    --vec.num-buffers 2 \
    --train.total-timesteps 1000000 \
    --train.horizon 64 \
    --train.minibatch-size 2048 \
    --train.gpus 1 \
    2>&1 | tee "$OUT/train.log"

date
echo "DONE"
