#!/bin/bash
#SBATCH --job-name=nh_train
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=16
#SBATCH --mem-per-cpu=2G
#SBATCH --time=00:25:00
#SBATCH --partition=cpu
#SBATCH --output=ocean/nethack/experiments/exp_009_real_training/train.out

set -u
cd "$SLURM_SUBMIT_DIR"

OUT=ocean/nethack/experiments/exp_009_real_training
mkdir -p "$OUT"

source /usr/share/Modules/init/bash 2>&1 || true
module load intel/2024.2 2>&1 || echo "(module load failed, falling back)"

# CMPLR_ROOT is set by the intel module. Fall back to a known path.
LIBOMP_DIR="${CMPLR_ROOT:-/opt/intel/oneapi/compiler/2024.2}/lib"
if [ ! -f "$LIBOMP_DIR/libiomp5.so" ]; then
    LIBOMP_DIR=/opt/intel/oneapi/compiler/2024.2/lib
fi
export LD_LIBRARY_PATH="${LIBOMP_DIR}:${LD_LIBRARY_PATH:-}"
echo "host: $(hostname)   cpus: $SLURM_CPUS_PER_TASK   libomp: $(ls $LIBOMP_DIR/libiomp5.so 2>&1)"
date

# Reuse the existing repo-local .venv (on shared /scratch). Skip install.
VENV=$PWD/.venv
if [ ! -x "$VENV/bin/python" ]; then
    echo "ERROR: $VENV/bin/python missing — run uv venv first" >&2
    exit 2
fi
echo "venv python: $($VENV/bin/python --version)"

export NETHACK_LIBPATH="$PWD/vendor/nle/lib/libnethack.so"
export NETHACKDIR="$PWD/vendor/nle/nethackdir"
export OMP_NUM_THREADS=$SLURM_CPUS_PER_TASK

# _C.so must already be built for python3.11 (we did this on the login node).
ls -la pufferlib/_C.cpython-311-*.so 2>&1 | tail -1

# Tiny training session, --slowly to use torch backend (CPU).
# Set --train.gpus 1 to avoid the ZeroDivisionError; the torch backend
# will pick device='cpu' when _C.gpu == 0 (which it is for our --cpu build).
"$VENV/bin/python" -u -m pufferlib.pufferl train nethack \
    --slowly \
    --vec.total-agents 256 \
    --vec.num-buffers 1 \
    --train.total-timesteps 200000 \
    --train.horizon 64 \
    --train.minibatch-size 2048 \
    --train.gpus 1 \
    2>&1 | tee "$OUT/train.log"

date
echo "DONE"
