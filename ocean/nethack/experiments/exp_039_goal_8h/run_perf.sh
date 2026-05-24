#!/bin/bash
# Run puffer train under perf record (software cpu-clock event, no root needed).
# Usage: run_perf.sh [N] [TIMEOUT] [TAG]
set -u
N=${1:-128}
TIMEOUT=${2:-30}
TAG=${3:-perf}
MB=$(( N * 64 ))
cd /scratch/gpfs/ZHUANGL/jl0796/PufferLib
. /etc/profile.d/modules.sh 2>/dev/null
module load intel-oneapi/2024.2 2>/dev/null
export CUDA_HOME=/usr/local/cuda-12.8
export PATH=$CUDA_HOME/bin:$PATH
export LD_LIBRARY_PATH=/opt/intel/oneapi/compiler/2024.2/lib:$CUDA_HOME/lib64:${LD_LIBRARY_PATH:-}
export PYTHONUNBUFFERED=1
export NETHACKDIR="$(pwd)/vendor/nle/nethackdir"
source .venv/bin/activate
ulimit -c 0
DIR=ocean/nethack/experiments/exp_038_goal_8h
mkdir -p $DIR/cores
LOG=$DIR/sps_${TAG}_n${N}.out
ERR=$DIR/sps_${TAG}_n${N}.err
PERF_DATA=/scratch/gpfs/ZHUANGL/jl0796/PufferLib/$DIR/perf.data
cd $DIR/cores
# -F 99 sampling, dwarf call-graph, software cpu-clock (no root needed)
perf record -F 99 -e cpu-clock -g --call-graph dwarf -o $PERF_DATA -- \
  timeout $TIMEOUT puffer train nethack \
    --vec.total-agents $N --vec.num-buffers 1 --vec.num-threads 1 \
    --train.gpus 1 --train.total-timesteps 1000000000 \
    --train.minibatch-size $MB --train.horizon 64 \
  > /scratch/gpfs/ZHUANGL/jl0796/PufferLib/$LOG \
  2> /scratch/gpfs/ZHUANGL/jl0796/PufferLib/$ERR
EXIT=$?
echo "EXIT=$EXIT" > /scratch/gpfs/ZHUANGL/jl0796/PufferLib/$LOG.exit
exit $EXIT
