#!/usr/bin/env python3
"""Generate a synthetic pldrift CSV and ptp4l log with known drift, to check
drift_analysis.py recovers it. Usage: python make_synthetic.py [ppm] [hours]"""
import sys
import numpy as np

ppm = float(sys.argv[1]) if len(sys.argv) > 1 else 12.345
hours = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0
fnom = 200e6
rng = np.random.default_rng(1)

n = int(hours * 3600)
t = np.arange(n, dtype=np.float64) + rng.normal(0, 200e-6, n)   # sample time jitter
age = rng.uniform(0, 50e-6, n)                                   # newest record age
pl = np.floor((t - age) * fnom * (1 + ppm * 1e-6)).astype(np.int64) + 123_456_789
mono = (t * 1e9).astype(np.int64) + 50_000_000_000
real = mono + 1_790_000_000_000_000_000
temp = (45000 + 3000 * np.sin(t / 1800)).astype(int)

with open("synthetic.csv", "w") as f:
    f.write(f"# synthetic ppm={ppm}\n")
    f.write("mono_raw_ns,realtime_ns,pl64,pl_lo,scan_ns,temp_mc,retries\n")
    for i in range(n):
        lo = pl[i] % (1 << 32)
        scan = 3000 if i % 997 else 90000        # occasional outlier
        f.write(f"{mono[i]},{real[i]},{lo},{lo},{scan},{temp[i]},0\n")

with open("synthetic_ptp.log", "w") as f:
    for i in range(n):
        st = 2 if i > 30 else 1
        f.write(f"ptp4l[{100+i}.123]: master offset {int(rng.normal(0,15000))} s{st} "
                f"freq {int(-4200 + rng.normal(0,50)):+d} path delay 25000\n")
print(f"wrote synthetic.csv ({n} rows, {ppm} ppm, 32-bit counter) and synthetic_ptp.log")
