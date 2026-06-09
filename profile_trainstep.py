"""Micro-benchmark + torch profiler of the NetHack train-step model.

Replicates the real policy (DefaultEncoder -> MinGRU(2L,192) -> DefaultDecoder)
and profiles forward+backward over a representative PPO minibatch so we can see
where the GPU time in the 'Train' phase actually goes. obs is the real 92 bytes.
"""
import torch, time
import numpy as np
from torch.profiler import profile, ProfilerActivity
import pufferlib.models as M

dev = 'cuda'
OBS = 92            # 9x9 crop + 11 compact blstats
HID = 192
LAYERS = 2
HORIZON = 256
MB = 131072         # peak-throughput minibatch
SEG = MB // HORIZON # segments

enc = M.DefaultEncoder(OBS, HID).to(dev)
net = M.MinGRU(HID, num_layers=LAYERS).to(dev)
dec = M.DefaultDecoder(np.array([23]), HID).to(dev)
params = list(enc.parameters()) + list(net.parameters()) + list(dec.parameters())
n_params = sum(p.numel() for p in params)
opt = torch.optim.Adam(params, lr=1e-4)

obs = torch.randint(0, 255, (SEG, HORIZON, OBS), device=dev, dtype=torch.uint8).float()

def step():
    opt.zero_grad(set_to_none=True)
    h = enc(obs.reshape(-1, OBS)).reshape(SEG, HORIZON, HID)
    h = net.forward_train(h)
    logits, value = dec(h.reshape(-1, HID))
    loss = logits.float().pow(2).mean() + value.float().pow(2).mean()
    loss.backward()
    opt.step()

# warmup
for _ in range(5):
    step()
torch.cuda.synchronize()

# time
N = 30
t0 = time.perf_counter()
for _ in range(N):
    step()
torch.cuda.synchronize()
dt = (time.perf_counter() - t0) / N
print(f"params={n_params/1e3:.1f}K  minibatch={MB}  seg={SEG} T={HORIZON}")
print(f"train-step (fwd+bwd+opt): {dt*1000:.2f} ms  ->  {MB/dt/1e3:.0f}K samples/s/minibatch")

# profile
with profile(activities=[ProfilerActivity.CPU, ProfilerActivity.CUDA]) as prof:
    for _ in range(10):
        step()
    torch.cuda.synchronize()
print("\n=== TOP CUDA OPS (self time) ===")
print(prof.key_averages().table(sort_by="self_cuda_time_total", row_limit=15))
