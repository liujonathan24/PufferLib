#!/bin/bash
# Run 3 hyperparam configs sequentially. ~25 min each.
DIR=ocean/nethack/experiments/exp_032_hyperparam
LOG=$DIR/sweep.log
echo "=== sweep start $(date +%H:%M:%S) ===" > $LOG

# Config A: small model + aggressive exploration
echo "--- A: small + aggressive --- $(date +%H:%M:%S)" >> $LOG
bash $DIR/run_hp.sh "A_small_aggr" 128 2 0.2 0.10 1500 >> $LOG 2>&1
echo "--- A done $(date +%H:%M:%S), exit=$(cat $DIR/A_small_aggr.out.exit 2>/dev/null) ---" >> $LOG

# Config B: small model + moderate
echo "--- B: small + moderate --- $(date +%H:%M:%S)" >> $LOG
bash $DIR/run_hp.sh "B_small_mod"  128 2 0.1 0.05 1500 >> $LOG 2>&1
echo "--- B done $(date +%H:%M:%S), exit=$(cat $DIR/B_small_mod.out.exit 2>/dev/null) ---" >> $LOG

# Config C: large model + aggressive (control for model size effect)
echo "--- C: large + aggressive --- $(date +%H:%M:%S)" >> $LOG
bash $DIR/run_hp.sh "C_large_aggr" 512 4 0.2 0.10 1500 >> $LOG 2>&1
echo "--- C done $(date +%H:%M:%S), exit=$(cat $DIR/C_large_aggr.out.exit 2>/dev/null) ---" >> $LOG

echo "=== sweep complete $(date +%H:%M:%S) ===" >> $LOG
