"""Smoke test: drive PufferLib's VecEnv for the NetHack env from Python.

Goal: verify the binding.c we wrote actually plugs into pufferlib's
vectorized step pipeline. Just call create_vec, reset, step a few times
with random actions, sample throughput.
"""
import os, sys, time, json
import numpy as np

sys.path.insert(0, '/tmp/pyenv')

# Force LD_LIBRARY_PATH to find libiomp5.so at runtime.
os.environ.setdefault('LD_LIBRARY_PATH',
    '/opt/intel/oneapi/compiler/2024.2/lib:' + os.environ.get('LD_LIBRARY_PATH',''))

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, REPO)
os.chdir(REPO)  # so ./vendor/nle/* resolves

from pufferlib import _C

print(f"_C.env_name = {getattr(_C, 'env_name', '?')}")
print(f"_C.gpu      = {_C.gpu}")
print(f"_C.precision_bytes = {_C.precision_bytes}")

TOTAL_AGENTS = int(os.environ.get('AGENTS', 32))
NUM_BUFFERS  = int(os.environ.get('BUFFERS', 1))
N_STEPS      = int(os.environ.get('STEPS', 2000))

args = {
    'vec': {
        'total_agents': float(TOTAL_AGENTS),
        'num_buffers':  float(NUM_BUFFERS),
        'num_threads':  float(0),
        'seed':         float(73),
    },
    'env': {},
}

print(f"creating vec with total_agents={TOTAL_AGENTS} num_buffers={NUM_BUFFERS}")
t0 = time.perf_counter()
ve = _C.create_vec(args, 0)
t1 = time.perf_counter()
print(f"  created in {t1-t0:.3f}s; obs_size={ve.obs_size} num_atns={ve.num_atns} dtype={ve.obs_dtype}")

print("resetting...")
t0 = time.perf_counter()
ve.reset()
t1 = time.perf_counter()
print(f"  reset in {t1-t0:.3f}s")

actions = np.zeros((TOTAL_AGENTS, ve.num_atns), dtype=np.float32)
print(f"stepping {N_STEPS} times with random actions...")
import numpy.random as nr
rng = nr.default_rng(0)

t0 = time.perf_counter()
total_steps = 0
for t in range(N_STEPS):
    actions[:] = rng.integers(0, 23, size=actions.shape).astype(np.float32)
    ve.cpu_step(actions.ctypes.data)
    total_steps += TOTAL_AGENTS
t1 = time.perf_counter()
wall = t1 - t0

print(f"\n=== throughput ===")
print(f"  wall            = {wall:.3f} s")
print(f"  vec steps       = {N_STEPS} (each = {TOTAL_AGENTS} agent c_steps)")
print(f"  total c_steps   = {total_steps:,}")
print(f"  c_steps/sec     = {total_steps/wall:,.0f}")

log = ve.log()
print(f"\n=== aggregated log ===")
for k, v in log.items():
    print(f"  {k:<22s} = {v}")

ve.close()
