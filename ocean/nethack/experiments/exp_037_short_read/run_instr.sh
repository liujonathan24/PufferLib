#!/bin/bash
# Instrumented N=1024 repro — captures DEF_MREAD_SHORT and DEF_BWRITE_SHORT on the abort.
set -u
N=${1:-1024}
TIMEOUT=${2:-300}
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
ulimit -c unlimited
DIR=ocean/nethack/experiments/exp_037_short_read
mkdir -p $DIR/cores
LOG=$DIR/n${N}.out
ERR=$DIR/n${N}.err
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
