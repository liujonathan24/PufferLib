#!/bin/bash
# run_n.sh <N> <TIMEOUT>
set -u
N=$1
TIMEOUT=$2
MB=$(( N * 64 ))

cd /scratch/gpfs/ZHUANGL/jl0796/PufferLib
. /etc/profile.d/modules.sh 2>/dev/null
module load intel-oneapi/2024.2 2>/dev/null
export CUDA_HOME=/usr/local/cuda-12.8
export PATH=$CUDA_HOME/bin:$PATH
export LD_LIBRARY_PATH=/opt/intel/oneapi/compiler/2024.2/lib:$CUDA_HOME/lib64:$LD_LIBRARY_PATH
export PYTHONUNBUFFERED=1
source .venv/bin/activate
ulimit -c 0

LOG="ocean/nethack/experiments/exp_031_scaling/n${N}.out"
ERR="ocean/nethack/experiments/exp_031_scaling/n${N}.err"
CSV="ocean/nethack/experiments/exp_031_scaling/n${N}_curve.csv"
PIDFILE="ocean/nethack/experiments/exp_031_scaling/n${N}.pid"

# Start puffer in background so we can grab its PID for the sampler
timeout $TIMEOUT puffer train nethack \
    --vec.total-agents $N --vec.num-buffers 1 --vec.num-threads 1 \
    --train.gpus 1 --train.total-timesteps 1000000000 \
    --train.minibatch-size $MB --train.horizon 64 \
  > $LOG 2> $ERR &
PUFFER_PID=$!
echo $PUFFER_PID > $PIDFILE

# Start sampler in same script; when puffer exits, sampler exits.
ocean/nethack/experiments/exp_031_scaling/sampler.sh $N $LOG $CSV $PUFFER_PID &
SAMPLER_PID=$!

wait $PUFFER_PID
EXIT=$?
# Let sampler catch the final exit and stop on its own
sleep 2
kill $SAMPLER_PID 2>/dev/null || true
echo "EXIT=$EXIT" >> $LOG.exit
exit $EXIT
