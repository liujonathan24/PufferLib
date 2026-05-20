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

# Activate the repo-local venv first so `python` resolves to 3.11+numpy.
source "$PWD/.venv/bin/activate"

source /usr/share/Modules/init/bash 2>&1 || true
module load intel/2024.2 2>&1 | tail -3
module load cudatoolkit/12.8 2>&1 | tail -3

# Fallbacks if module load is silently no-op on this node.
export PATH=/usr/local/cuda-12.8/bin:$PATH
export CUDA_HOME=/usr/local/cuda-12.8
LIBOMP_DIR="${CMPLR_ROOT:-/opt/intel/oneapi/compiler/2024.2}/lib"
export LD_LIBRARY_PATH="${LIBOMP_DIR}:${CUDA_HOME}/lib64:${LD_LIBRARY_PATH:-}"

export NETHACK_LIBPATH="$PWD/vendor/nle/lib/libnethack.so"
export NETHACK_DIR="$PWD/vendor/nle/nethackdir"
export NETHACKDIR="$PWD/vendor/nle/nethackdir"
export OMP_NUM_THREADS=$SLURM_CPUS_PER_TASK

echo "host: $(hostname)   cpus: $SLURM_CPUS_PER_TASK"
nvidia-smi -L 2>&1 | head -2
which nvcc; nvcc --version 2>&1 | tail -1
which python; python --version
python -c "import torch; print('torch=', torch.__version__, 'cuda=', torch.cuda.is_available(), 'devs=', torch.cuda.device_count())"
date

# Rebuild _C.so with CUDA bindings (the default, no --cpu flag).
bash build.sh nethack 2>&1 | tail -6
ls -la pufferlib/_C.cpython-311-*.so

# GPU training — the CUDA backend uses _C.create_pufferl with GPU tensors.
python -u -m pufferlib.pufferl train nethack \
    --vec.total-agents 512 \
    --vec.num-buffers 2 \
    --train.total-timesteps 1000000 \
    --train.horizon 64 \
    --train.minibatch-size 4096 \
    --train.gpus 1 \
    2>&1 | tee "$OUT/train.log"

date
echo "DONE"
