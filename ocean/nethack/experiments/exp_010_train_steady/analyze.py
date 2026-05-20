"""Parse a `puffer train --slowly` log and extract real-training throughput.

Looks for lines that pufferl emits periodically:
  - 'elapsed_steps' / 'sps' (steps per second)
  - 'episode_return' / 'episode_length'
  - 'valid_moves' / 'illegal_actions' / 'new_tiles' (from our binding.c::my_log)

Usage: python analyze.py train.log
"""
import sys, re, json, statistics

if len(sys.argv) < 2:
    sys.exit("usage: analyze.py TRAIN.log")

text = open(sys.argv[1]).read()

# Match key=value pairs in pufferl's tabular log.
pat = re.compile(r'(\w+)\s*[:=]\s*([\d.eE+-]+)')
keys_of_interest = {
    'elapsed_steps', 'sps', 'epoch', 'updates', 'episode_return',
    'episode_length', 'valid_moves', 'illegal_actions', 'new_tiles',
    'depth', 'score'
}

records = []
cur = {}
for line in text.splitlines():
    pairs = dict(pat.findall(line))
    keep = {k: float(v) for k, v in pairs.items() if k in keys_of_interest}
    if keep:
        cur.update(keep)
        if 'elapsed_steps' in keep or 'sps' in keep:
            records.append(dict(cur))
            cur = {}

if not records:
    print("No structured log entries found. Raw tail of log:")
    print('\n'.join(text.splitlines()[-30:]))
    sys.exit(0)

print(f"Parsed {len(records)} log entries.")
print()
print(f"{'#':>3} {'elapsed':>10} {'sps':>10} {'ep_len':>8} {'valid':>8} {'illegal':>8} {'new_tiles':>10} {'ep_return':>10}")
for i, r in enumerate(records[-20:]):
    print(f"{i:>3} {r.get('elapsed_steps','?'):>10} {r.get('sps','?'):>10} "
          f"{r.get('episode_length','?'):>8.0f} {r.get('valid_moves','?'):>8.1f} "
          f"{r.get('illegal_actions','?'):>8.2f} {r.get('new_tiles','?'):>10.1f} "
          f"{r.get('episode_return','?'):>10.2f}")

last = records[-5:]
if last and 'sps' in last[0]:
    sps_vals = [r['sps'] for r in last if 'sps' in r]
    if sps_vals:
        print()
        print(f"steady-state sps (last 5 entries): mean={statistics.mean(sps_vals):.0f}  median={statistics.median(sps_vals):.0f}")

# Compute valid_moves/sec if both rates available
if records and 'valid_moves' in records[-1] and 'episode_length' in records[-1] and 'sps' in records[-1]:
    valid_ratio = records[-1]['valid_moves'] / records[-1]['episode_length']
    print(f"valid_moves/c_step (last episode): {valid_ratio:.3f}")
    if 'sps' in records[-1]:
        print(f"valid_moves/sec (estimated):       {records[-1]['sps'] * valid_ratio:.0f}")
