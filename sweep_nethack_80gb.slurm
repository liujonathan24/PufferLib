#!/bin/bash
#SBATCH --job-name=nh-sweep80
#SBATCH --output=sweep80_%j.out
#SBATCH --error=sweep80_%j.err
#SBATCH --constraint=gpu80
#SBATCH --time=2:30:00
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=12
#SBATCH --gres=gpu:1
#SBATCH --mem=64G
#SBATCH --account=zhuangl

set -euo pipefail
cd /scratch/gpfs/ZHUANGL/jl0796/PufferLib

. /etc/profile.d/modules.sh 2>/dev/null || true
module load cudatoolkit/12.8 intel-oneapi/2024.2 2>/dev/null || true
export LD_LIBRARY_PATH=/opt/intel/oneapi/compiler/2024.2/lib:${LD_LIBRARY_PATH:-}
export PYTHONUNBUFFERED=1
export NETHACKDIR="$(pwd)/vendor/nle/src/build/dat"
export OPENBLAS_NUM_THREADS=1
export MKL_NUM_THREADS=1
export WANDB_MODE=offline
source .venv/bin/activate
ulimit -c 0

echo "=== Protein sweep (80GB A100): 8 runs x <=15min, metric=depth ==="
nvidia-smi --query-gpu=name,memory.total --format=csv,noheader

# Conv encoder is VRAM-heavy (cuDNN workspace ~35GB); N kept moderate on 80GB.
# total_timesteps is the Protein cost dim (config bounds it to ~15min/trial).
# --train.total-timesteps bounds the first 2 (pre-suggest) sweep runs to
# ~15min; runs 3+ get total_timesteps from the Protein cost dimension.
puffer sweep nethack \
    --vec.total-agents 1024 \
    --vec.num-buffers 2 \
    --vec.num-threads 12 \
    --train.minibatch-size 16384 \
    --train.horizon 128 \
    --train.total-timesteps 60000000 \
    --policy.hidden-size 256 \
    --policy.num-layers 2 \
    --cudagraphs 1
