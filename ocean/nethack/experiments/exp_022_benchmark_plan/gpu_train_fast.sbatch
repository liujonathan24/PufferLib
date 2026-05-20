#!/bin/bash
#SBATCH --job-name=nh_fast
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=16
#SBATCH --gres=gpu:1
#SBATCH --constraint=gpu80
#SBATCH --mem-per-cpu=4G
#SBATCH --time=00:25:00
#SBATCH --output=ocean/nethack/experiments/exp_022_benchmark_plan/train_fast.out

# Mirror of exp_012_gpu_training/run.sbatch but with the fast-reset
# libnethack.so patched in. The only differences vs exp_012:
#   1. NETHACK_LIBPATH points at vendor/nle/src/build/libnethack.so
#      (the from-source build that exports nle_fr_* and uses the arena alloc)
#   2. EXTRA_CFLAGS adds -DNETHACK_FAST_RESET=1 so the env wrapper uses the
#      snapshot/restore path instead of dlopen-per-reset
#
# Target: aggregate SPS > 2x exp_012's 49K (≥100K SPS = "win threshold"
# from the benchmark plan). Stretch: 200K+.

set -u
cd "$SLURM_SUBMIT_DIR"

OUT=ocean/nethack/experiments/exp_022_benchmark_plan
mkdir -p "$OUT"

source "$PWD/.venv/bin/activate"
source /usr/share/Modules/init/bash 2>&1 || true
module load intel/2024.2 2>&1 | tail -3
module load cudatoolkit/12.8 2>&1 | tail -3
export PATH=/usr/local/cuda-12.8/bin:$PATH
export CUDA_HOME=/usr/local/cuda-12.8
LIBOMP_DIR="${CMPLR_ROOT:-/opt/intel/oneapi/compiler/2024.2}/lib"
export LD_LIBRARY_PATH="${LIBOMP_DIR}:${CUDA_HOME}/lib64:${LD_LIBRARY_PATH:-}"

# Patched libnethack.so (RNG refactor + arena alloc + fast-reset)
export NETHACK_LIBPATH="$PWD/vendor/nle/src/build/libnethack.so"
export NETHACKDIR="$PWD/vendor/nle/nethackdir"
export OMP_NUM_THREADS=$SLURM_CPUS_PER_TASK

echo "host: $(hostname)   cpus: $SLURM_CPUS_PER_TASK"
nvidia-smi -L 2>&1 | head -2
date

# Rebuild the patched libnethack from source, then the puffer _C.so with
# NETHACK_FAST_RESET=1 so c_reset takes the snapshot/restore path.
cd "$PWD/vendor/nle/src/build" && cmake --build . --target nethack -j8 2>&1 | tail -3
cd "$SLURM_SUBMIT_DIR"
EXTRA_CFLAGS="-DNETHACK_FAST_RESET=1" bash build.sh nethack 2>&1 | tail -3
ls -la pufferlib/_C.cpython-311-*.so

# Same config as exp_012 GPU training run.
python -u -m pufferlib.pufferl train nethack \
    --vec.total-agents 512 \
    --vec.num-buffers 2 \
    --train.total-timesteps 2000000 \
    --train.horizon 64 \
    --train.minibatch-size 4096 \
    --train.gpus 1 \
    2>&1 | tee "$OUT/train_fast.log"

date
echo "DONE"
