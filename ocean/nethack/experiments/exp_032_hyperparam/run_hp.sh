#!/bin/bash
# run_hp.sh <label> <hidden> <layers> <clip> <ent> <timeout_sec>
LABEL=$1; H=$2; L=$3; CLIP=$4; ENT=$5; TIMEOUT=$6
cd /scratch/gpfs/ZHUANGL/jl0796/PufferLib
. /etc/profile.d/modules.sh 2>/dev/null
module load intel-oneapi/2024.2 2>/dev/null
export CUDA_HOME=/usr/local/cuda-12.8
export PATH=$CUDA_HOME/bin:$PATH
export LD_LIBRARY_PATH=/opt/intel/oneapi/compiler/2024.2/lib:$CUDA_HOME/lib64:${LD_LIBRARY_PATH:-}
export PYTHONUNBUFFERED=1
source .venv/bin/activate
ulimit -c 0
DIR=ocean/nethack/experiments/exp_032_hyperparam
LOG=$DIR/${LABEL}.out
ERR=$DIR/${LABEL}.err
CSV=$DIR/${LABEL}_curve.csv
PIDFILE=$DIR/${LABEL}.pid
echo "wall_sec,uptime,steps,sps,epoch,ret,len,depth,vmoves,imoves,ntiles,gpu,vram,entropy" > $CSV
timeout $TIMEOUT puffer train nethack \
    --vec.total-agents 512 --vec.num-buffers 1 --vec.num-threads 1 \
    --policy.hidden-size $H --policy.num-layers $L \
    --train.clip-coef $CLIP --train.ent-coef $ENT \
    --train.gpus 1 --train.total-timesteps 1000000000 \
    --train.minibatch-size 32768 --train.horizon 64 \
  > $LOG 2> $ERR &
PUFFER_PID=$!
echo $PUFFER_PID > $PIDFILE
T0=$(date +%s)
while kill -0 $PUFFER_PID 2>/dev/null; do
  sleep 30
  WALL=$(( $(date +%s) - T0 ))
  CLEAN=$(tail -c 8000 $LOG 2>/dev/null | tr -d '\033\r' | tr -s ' ')
  STEPS=$(echo "$CLEAN" | grep -oE "Steps[[:space:]]+[-0-9.eEkKmM+]+" | tail -1 | awk '{print $2}')
  SPS=$(echo "$CLEAN" | grep -oE "SPS[[:space:]]+[-0-9.eEkKmM+]+" | tail -1 | awk '{print $2}')
  EP=$(echo "$CLEAN" | grep -oE "Epoch[[:space:]]+[-0-9]+" | tail -1 | awk '{print $2}')
  UPT=$(echo "$CLEAN" | grep -oE "Uptime[[:space:]]+[^|]+" | tail -1 | sed 's/Uptime[[:space:]]*//' | tr -d ' ' | head -c 40)
  RET=$(echo "$CLEAN" | grep -oE "episode_return[[:space:]]+[-0-9.eE+]+" | tail -1 | awk '{print $2}')
  LEN=$(echo "$CLEAN" | grep -oE "episode_length[[:space:]]+[-0-9.eE+]+" | tail -1 | awk '{print $2}')
  DEP=$(echo "$CLEAN" | grep -oE "[^a-z]depth[[:space:]]+[-0-9.eE+]+" | tail -1 | awk '{print $2}')
  VM=$(echo "$CLEAN" | grep -oE "valid_moves[[:space:]]+[-0-9.eE+]+" | tail -1 | awk '{print $2}')
  IM=$(echo "$CLEAN" | grep -oE "illegal_actions[[:space:]]+[-0-9.eE+]+" | tail -1 | awk '{print $2}')
  NT=$(echo "$CLEAN" | grep -oE "new_tiles[[:space:]]+[-0-9.eE+]+" | tail -1 | awk '{print $2}')
  GPU=$(echo "$CLEAN" | grep -oE "GPU:[[:space:]]+[0-9]+%" | tail -1 | grep -oE "[0-9]+")
  VR=$(echo "$CLEAN" | grep -oE "VRAM:[[:space:]]+[-0-9.]+/" | tail -1 | sed -E 's|.*VRAM:[[:space:]]+([0-9.]+)/|\1|')
  ENTV=$(echo "$CLEAN" | grep -oE "entropy[[:space:]]+[-0-9.eE+]+" | tail -1 | awk '{print $2}')
  echo "$WALL,$UPT,$STEPS,$SPS,$EP,$RET,$LEN,$DEP,$VM,$IM,$NT,$GPU,$VR,$ENTV" >> $CSV
done
EXIT=$?
echo "EXIT=$EXIT" > $LOG.exit
