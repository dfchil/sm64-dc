#!/usr/bin/env python3
"""Checks a slice replay's trace (host SM64_TRACE with SM64_SLICES) against the N64 trace, matching
lines by sample number: for each stretch of consecutive samples, where it first differs.

    tools/tasref/compare_slice.py n64.trace replay.trace
"""
import sys

ref = {}
for line in open(sys.argv[1]):
    ref[int(line.split(" ", 1)[0])] = line.rstrip("\n")
runs, cur = [], None
for line in open(sys.argv[2]):
    n = int(line.split(" ", 1)[0])
    if cur is None or n != cur[1] + 1:
        cur = [n, n, None]
        runs.append(cur)
    cur[1] = n
    if cur[2] is None and ref.get(n) != line.rstrip("\n"):
        cur[2] = n
for start, end, bad in runs:
    if end - start < 30:
        continue
    status = "matches" if bad is None else f"differs from sample {bad} ({bad - start} in)"
    print(f"samples {start}-{end}: {status}")
    if bad is not None:
        a = ref.get(bad, "").split()
        b = [l for l in open(sys.argv[2]) if l.startswith(f"{bad} ")][0].split()
        for x, y in zip(a, b):
            if x != y:
                print(f"   n64 {x}\n   got {y}")
