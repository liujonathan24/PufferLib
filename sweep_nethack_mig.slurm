#!/bin/bash
#SBATCH --job-name=nh-sweepmig
#SBATCH --output=sweepmig_%j.out
#SBATCH --error=sweepmig_%j.err
#SBATCH --partition=mig
#SBATCH --time=0:30:00
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=1
#SBATCH --gres=gpu:1g.10gb:1
#SBATCH --mem=32G
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

echo "=== Protein sweep (10GB MIG A100): fit test, tiny N ==="
nvidia-smi --query-gpu=name,memory.total --format=csv,noheader
# WARNING: the conv encoder's cuDNN workspace needed ~38GB at N=256 on a full
# A100 — this 10GB MIG slice will very likely OOM. This run is the fit test;
# check sweepmig_<jobid>.err for 'CUDA OOM' to confirm.

puffer sweep nethack \
    --vec.total-agents 128 \
    --vec.num-buffers 1 \
    --vec.num-threads 1 \
    --train.minibatch-size 4096 \
    --train.horizon 64 \
    --train.total-timesteps 20000000 \
    --policy.hidden-size 128 \
    --policy.num-layers 1 \
    --cudagraphs 1
