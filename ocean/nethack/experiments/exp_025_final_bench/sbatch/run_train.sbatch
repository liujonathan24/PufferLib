#!/bin/bash
#SBATCH --job-name=nh_train
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --gres=gpu:1
#SBATCH --constraint=gpu80
#SBATCH --mem-per-cpu=4G
#SBATCH --time=00:25:00
# CPU count and per-cpu agents are passed via --export=CPUS=,AGENTS_PER_CPU=
# Sbatch with: sbatch --cpus-per-task=$CPUS --output=$OUT \
#                --export=ALL,CPUS=$CPUS,AGENTS_PER_CPU=$APC,TAG=$TAG \
#                run_train.sbatch

set -u
cd "$SLURM_SUBMIT_DIR"

: "${CPUS:?CPUS must be set in --export}"
: "${AGENTS_PER_CPU:?AGENTS_PER_CPU must be set in --export}"
: "${TAG:?TAG must be set in --export}"

OUT=ocean/nethack/experiments/exp_025_final_bench
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

echo "host: $(hostname)   cpus: $SLURM_CPUS_PER_TASK   agents_per_cpu: $AGENTS_PER_CPU   tag: $TAG"
echo "commit: $(git rev-parse --short HEAD)"
nvidia-smi -L 2>&1 | head -2
date

cd "$PWD/vendor/nle/src/build" && cmake --build . --target nethack -j8 2>&1 | tail -3
cd "$SLURM_SUBMIT_DIR"

TOTAL_AGENTS=$((SLURM_CPUS_PER_TASK * AGENTS_PER_CPU))

python -u -m pufferlib.pufferl train nethack \
    --vec.total-agents $TOTAL_AGENTS \
    --vec.num-buffers 2 \
    --train.total-timesteps 3000000 \
    --train.horizon 64 \
    --train.minibatch-size 4096 \
    --train.gpus 1 \
    2>&1 | tee "$OUT/train_${TAG}.log"

date
echo "DONE-$TAG"
