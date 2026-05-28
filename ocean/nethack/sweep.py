#!/usr/bin/env python3
"""Small random hyperparameter sweep for NetHack. Samples from the ranges in
config/nethack.ini [sweep.*] sections, runs short trials, and ranks by the
final `depth` (dungeon level) reported in the training dashboard.

Usage: python ocean/nethack/sweep.py [n_runs] [seconds_per_run]
"""
import os, sys, re, math, random, subprocess, time

N_RUNS = int(sys.argv[1]) if len(sys.argv) > 1 else 5
RUN_SECS = int(sys.argv[2]) if len(sys.argv) > 2 else 300

HACKDIR = os.path.join(os.getcwd(), "vendor/nle/src/build/dat")

def log_uniform(lo, hi):
    return math.exp(random.uniform(math.log(lo), math.log(hi)))

def sample():
    return {
        "env.score-coef":       log_uniform(0.001, 0.1),
        "env.descent-coef":     random.uniform(0.0, 50.0),
        "env.scout-coef":       random.uniform(0.0, 2.0),
        "env.reveal-coef":      random.uniform(0.0, 0.1),
        "env.illegal-penalty":  random.uniform(-2.0, 0.0),
        "train.learning-rate":  log_uniform(0.0001, 0.01),
        "train.gamma":          random.uniform(0.99, 0.9999),
        "train.gae-lambda":     random.uniform(0.5, 0.99),
        "train.ent-coef":       log_uniform(0.001, 0.5),
        "train.max-grad-norm":  random.uniform(0.1, 3.0),
    }

# Fixed config: small N for fast iteration on the login node, conv encoder.
FIXED = [
    "--vec.total-agents", "256",
    "--vec.num-threads", "4",
    "--train.minibatch-size", "256",
    "--train.horizon", "64",
    "--policy.hidden-size", "256",
    "--policy.num-layers", "2",
    "--cudagraphs", "1",
]

DEPTH_RE = re.compile(r"depth\s+([0-9.]+)")

def run_trial(idx, hp):
    args = ["puffer", "train", "nethack"] + FIXED
    for k, v in hp.items():
        args += [f"--{k}", f"{v:.6g}"]
    env = dict(os.environ, NETHACKDIR=HACKDIR)
    logpath = f"/tmp/sweep_run_{idx}.log"
    with open(logpath, "w") as f:
        p = subprocess.Popen(args, stdout=f, stderr=subprocess.STDOUT, env=env)
        time.sleep(RUN_SECS)
        p.terminate()
        try: p.wait(timeout=15)
        except subprocess.TimeoutExpired: p.kill()
    # Parse the last few depth values, report their max (best avg-depth seen).
    depths = []
    with open(logpath) as f:
        for line in f:
            m = DEPTH_RE.search(line)
            if m:
                try: depths.append(float(m.group(1)))
                except ValueError: pass
    best = max(depths[-30:]) if depths else 0.0
    final = depths[-1] if depths else 0.0
    return best, final, len(depths)

def main():
    print(f"NetHack sweep: {N_RUNS} runs x {RUN_SECS}s, metric=depth (dungeon level)\n")
    results = []
    for i in range(N_RUNS):
        hp = sample()
        print(f"=== Run {i+1}/{N_RUNS} ===")
        for k, v in hp.items():
            print(f"  {k}: {v:.4g}")
        best, final, n = run_trial(i, hp)
        print(f"  -> best_depth={best:.3f}  final_depth={final:.3f}  (epochs={n})\n")
        results.append((best, final, hp))

    results.sort(key=lambda r: r[0], reverse=True)
    print("\n===== RANKED RESULTS (by best depth) =====")
    for rank, (best, final, hp) in enumerate(results, 1):
        print(f"#{rank}: best={best:.3f} final={final:.3f}")
        print("    " + "  ".join(f"{k}={v:.4g}" for k, v in hp.items()))

if __name__ == "__main__":
    main()
