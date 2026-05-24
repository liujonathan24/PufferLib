#!/bin/bash
# Quick SPS sample at given N for 60s
set -u
N=${1:-64}
TIMEOUT=${2:-60}
MB=$(( N * 64 ))
cd /scratch/gpfs/ZHUANGL/jl0796/PufferLib
. /etc/profile.d/modules.sh 2>/dev/null
module load intel-oneapi/2024.2 2>/dev/null
export CUDA_HOME=/usr/local/cuda-12.8
export PATH=$CUDA_HOME/bin:$PATH
export LD_LIBRARY_PATH=/opt/intel/oneapi/compiler/2024.2/lib:$CUDA_HOME/lib64:${LD_LIBRARY_PATH:-}
export PYTHONUNBUFFERED=1
export NETHACKDIR="$(pwd)/vendor/nle/nethackdir"
# exp_039: BLAS workers spinning idle competed for cores (~9.7% user CPU)
export OPENBLAS_NUM_THREADS=1
export MKL_NUM_THREADS=1
source .venv/bin/activate
ulimit -c 0
DIR=ocean/nethack/experiments/exp_039_goal_8h
mkdir -p $DIR/cores
TAG=${3:-baseline}
LOG=$DIR/sps_${TAG}_n${N}.out
ERR=$DIR/sps_${TAG}_n${N}.err
cd $DIR/cores
timeout $TIMEOUT puffer train nethack \
    --vec.total-agents $N --vec.num-buffers 1 --vec.num-threads 1 \
    --train.gpus 1 --train.total-timesteps 1000000000 \
    --train.minibatch-size $MB --train.horizon 64 \
  > /scratch/gpfs/ZHUANGL/jl0796/PufferLib/$LOG \
  2> /scratch/gpfs/ZHUANGL/jl0796/PufferLib/$ERR &
PID=$!
wait $PID
EXIT=$?
echo "EXIT=$EXIT" >> /scratch/gpfs/ZHUANGL/jl0796/PufferLib/$LOG.exit
exit $EXIT
