#!/bin/bash
# Host-side test of pldrift against a simulated OCM ring (no FPGA needed).
# Run from the test/ directory: bash run_host_test.sh
set -e
gcc -O2 -Wall -Wextra -o pldrift_host ../../../br-external/package/pldrift/src/pldrift.c
python3 mkring.py 3275 $(( (1<<32) - 50000000 - 10000 ))
python3 writer.py & sleep 0.3
./pldrift_host -m ring.bin -b 0 -i 0.25 -d 3 -o host_test.csv
wait
python3 - <<'PY'
import csv
rows = [r for r in csv.reader(l for l in open("host_test.csv") if not l.startswith("#"))][1:]
pl = [int(r[2]) for r in rows]
ok = len(rows) >= 10 and all(b > a for a, b in zip(pl, pl[1:])) and pl[-1] > (1 << 32)
print(f"{len(rows)} rows, increasing across 32-bit boundary: {ok}")
print("PASS" if ok else "FAIL")
PY
rm -f ring.bin pldrift_host
