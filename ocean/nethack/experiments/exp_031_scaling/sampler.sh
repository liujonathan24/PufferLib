#!/bin/bash
# Usage: sampler.sh <N> <LOG> <CSV> <PUFFER_PID>
# Samples every 30s while PUFFER_PID is alive.
set -u
N=$1
LOG=$2
CSV=$3
PID=$4
T0=$(date +%s)

if [ ! -f "$CSV" ]; then
  echo "wall_sec,uptime_sec,steps_raw,sps_raw,epoch,episode_return,episode_length,depth,valid_moves,illegal_actions,new_tiles,gpu_pct,vram_mb,arena_rss_mb" > "$CSV"
fi

while kill -0 "$PID" 2>/dev/null; do
  WALL=$(( $(date +%s) - T0 ))
  if [ -f "$LOG" ]; then
    CLEAN=$(tail -c 12000 "$LOG" 2>/dev/null | tr -d '\033\r' | tr -s ' ')
  else
    CLEAN=""
  fi

  STEPS=$(echo "$CLEAN" | grep -oE "Steps[[:space:]]+[-0-9.eE+kKmMgG]+" | tail -1 | awk '{print $2}')
  SPS=$(echo "$CLEAN" | grep -oE "SPS[[:space:]]+[-0-9.eE+kKmMgG]+" | tail -1 | awk '{print $2}')
  EPOCH=$(echo "$CLEAN" | grep -oE "Epoch[[:space:]]+[-0-9]+" | tail -1 | awk '{print $2}')
  UPTIME=$(echo "$CLEAN" | grep -oE "Uptime[[:space:]]+[^│]+" | tail -1 | sed 's/Uptime[[:space:]]*//' | tr -d ' ')
  EPRET=$(echo "$CLEAN" | grep -oE "episode_return[[:space:]]+[-0-9.eE+]+" | tail -1 | awk '{print $2}')
  EPLEN=$(echo "$CLEAN" | grep -oE "episode_length[[:space:]]+[-0-9.eE+]+" | tail -1 | awk '{print $2}')
  DEPTH=$(echo "$CLEAN" | grep -oE "depth[[:space:]]+[-0-9.eE+]+" | tail -1 | awk '{print $2}')
  VMOVES=$(echo "$CLEAN" | grep -oE "valid_moves[[:space:]]+[-0-9.eE+]+" | tail -1 | awk '{print $2}')
  IMOVES=$(echo "$CLEAN" | grep -oE "illegal_actions[[:space:]]+[-0-9.eE+]+" | tail -1 | awk '{print $2}')
  NTILES=$(echo "$CLEAN" | grep -oE "new_tiles[[:space:]]+[-0-9.eE+]+" | tail -1 | awk '{print $2}')
  GPU=$(echo "$CLEAN" | grep -oE "GPU:[[:space:]]+[0-9]+%" | tail -1 | grep -oE "[0-9]+")
  VRAM=$(echo "$CLEAN" | grep -oE "VRAM:[[:space:]]+[-0-9.]+/[0-9]+G" | tail -1 | sed -E 's|.*VRAM:[[:space:]]+([0-9.]+)/.*|\1|')

  if [ -d "/proc/$PID" ]; then
    RSS_KB=$(awk '/VmRSS/{print $2}' /proc/$PID/status 2>/dev/null)
    RSS_MB=$(( ${RSS_KB:-0} / 1024 ))
  else
    RSS_MB=""
  fi

  echo "$WALL,${UPTIME:-},${STEPS:-},${SPS:-},${EPOCH:-},${EPRET:-},${EPLEN:-},${DEPTH:-},${VMOVES:-},${IMOVES:-},${NTILES:-},${GPU:-},${VRAM:-},${RSS_MB}" >> "$CSV"

  sleep 30
done
