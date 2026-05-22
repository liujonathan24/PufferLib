#!/bin/bash
set -e
cd /scratch/gpfs/ZHUANGL/jl0796/PufferLib
. /etc/profile.d/modules.sh 2>/dev/null
module load intel-oneapi/2024.2 2>/dev/null
export CUDA_HOME=/usr/local/cuda-12.8
export PATH=$CUDA_HOME/bin:$PATH
export LD_LIBRARY_PATH=/opt/intel/oneapi/compiler/2024.2/lib:$CUDA_HOME/lib64:$LD_LIBRARY_PATH
source .venv/bin/activate
exec puffer train nethack \
    --vec.total-agents 4 \
    --vec.num-buffers 1 \
    --vec.num-threads 1 \
    --train.gpus 1 \
    --train.total-timesteps 200000 \
    --train.minibatch-size 256 \
    --train.horizon 64 \
    "$@"
