#!/bin/bash
#SBATCH --job-name=nh_16c
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=16
#SBATCH --gres=gpu:1
#SBATCH --constraint=gpu80
#SBATCH --mem-per-cpu=4G
#SBATCH --time=00:35:00
#SBATCH --output=ocean/nethack/experiments/exp_024_cpu_scaling/train_16cpu.out

set -u
cd "$SLURM_SUBMIT_DIR"

OUT=ocean/nethack/experiments/exp_024_cpu_scaling
mkdir -p "$OUT"

source "$PWD/.venv/bin/activate"
source /usr/share/Modules/init/bash 2>&1 || true
module load intel/2024.2 2>&1 | tail -3
module load cudatoolkit/12.8 2>&1 | tail -3
export PATH=/usr/local/cuda-12.8/bin:$PATH
export CUDA_HOME=/usr/local/cuda-12.8
LIBOMP_DIR="${CMPLR_ROOT:-/opt/intel/oneapi/compiler/2024.2}/lib"
export LD_LIBRARY_PATH="${LIBOMP_DIR}:${CUDA_HOME}/lib64:${LD_LIBRARY_PATH:-}"

export NETHACK_LIBPATH="$PWD/vendor/nle/src/build/libnethack.so"
export NETHACKDIR="$PWD/vendor/nle/nethackdir"
export OMP_NUM_THREADS=$SLURM_CPUS_PER_TASK

echo "host: $(hostname)   cpus: $SLURM_CPUS_PER_TASK"
nvidia-smi -L 2>&1 | head -2
date

# Rebuild libnethack + puffer _C.so with FAST_RESET (default ON now)
cd "$PWD/vendor/nle/src/build" && cmake --build . --target nethack -j8 2>&1 | tail -3
cd "$SLURM_SUBMIT_DIR"
bash build.sh nethack 2>&1 | tail -3
ls -la pufferlib/_C.cpython-311-*.so

# Scale total_agents with CPU count. Roughly 32 agents/CPU.
TOTAL_AGENTS=$((SLURM_CPUS_PER_TASK * 32))

python -u -m pufferlib.pufferl train nethack \
    --vec.total-agents $TOTAL_AGENTS \
    --vec.num-buffers 2 \
    --train.total-timesteps 3000000 \
    --train.horizon 64 \
    --train.minibatch-size 4096 \
    --train.gpus 1 \
    2>&1 | tee "$OUT/train_16cpu.log"

date
echo "DONE-16"
