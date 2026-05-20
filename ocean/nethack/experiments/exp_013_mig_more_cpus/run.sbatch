#!/bin/bash
#SBATCH --job-name=nh_mig
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=24
#SBATCH --gres=gpu:1
#SBATCH --partition=mig
#SBATCH --mem-per-cpu=4G
#SBATCH --time=00:25:00
#SBATCH --output=ocean/nethack/experiments/exp_013_mig_more_cpus/train.out

set -u
cd "$SLURM_SUBMIT_DIR"

OUT=ocean/nethack/experiments/exp_013_mig_more_cpus
mkdir -p "$OUT"

source "$PWD/.venv/bin/activate"
source /usr/share/Modules/init/bash 2>&1 || true
module load intel/2024.2 2>&1 | tail -3
module load cudatoolkit/12.8 2>&1 | tail -3
export PATH=/usr/local/cuda-12.8/bin:$PATH
export CUDA_HOME=/usr/local/cuda-12.8
LIBOMP_DIR="${CMPLR_ROOT:-/opt/intel/oneapi/compiler/2024.2}/lib"
export LD_LIBRARY_PATH="${LIBOMP_DIR}:${CUDA_HOME}/lib64:${LD_LIBRARY_PATH:-}"

export NETHACK_LIBPATH="$PWD/vendor/nle/lib/libnethack.so"
export NETHACKDIR="$PWD/vendor/nle/nethackdir"
export OMP_NUM_THREADS=$SLURM_CPUS_PER_TASK

echo "host: $(hostname)   cpus: $SLURM_CPUS_PER_TASK"
nvidia-smi -L 2>&1 | head -2
nvidia-smi -q -i 0 2>&1 | grep -E "Product Name|Memory:|MIG Mode|Persistence" | head -8
nvcc --version 2>&1 | tail -1
python -c "import torch; print('torch=', torch.__version__, 'cuda=', torch.cuda.is_available(), 'devs=', torch.cuda.device_count())"
date

bash build.sh nethack 2>&1 | tail -5
ls -la pufferlib/_C.cpython-311-*.so

# Match exp_012 config except total_agents doubled (more CPU cores feeding GPU).
python -u -m pufferlib.pufferl train nethack \
    --vec.total-agents 1024 \
    --vec.num-buffers 2 \
    --train.total-timesteps 1500000 \
    --train.horizon 64 \
    --train.minibatch-size 4096 \
    --train.gpus 1 \
    2>&1 | tee "$OUT/train.log"

date
echo "DONE"
