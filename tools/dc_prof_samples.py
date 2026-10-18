#!/usr/bin/env python3
"""Resolve a DC_PROF_SAMPLE=1 run's PC samples against its ELF.

    tools/dc_prof_samples.py build/prof/<variant>/us_dc/sm64.us.f3dex2e build/hw/<run>/sm64_samples.bin

Each sample is the interrupted PC (low 24 bits, offset from 0x8c000000) with the frame stage
in the top byte. Inlined code is credited to the innermost function, so hot inlined helpers
show up under their own names. Prints the share of each frame stage and the top functions
overall and within display-list translation.
"""
import collections
import struct
import subprocess
import sys

ADDR2LINE = "/opt/toolchains/dc/sh-elf/bin/sh-elf-addr2line"
STAGES = ["logic", "geo", "dl", "pvr-wait", "dl", "finish", "vsync"]


def resolve(elf, pcs):
    out = subprocess.run([ADDR2LINE, "-a", "-f", "-i", "-e", elf], input="\n".join(f"{pc:#x}" for pc in pcs),
                         capture_output=True, text=True, check=True).stdout.splitlines()
    names, cur = {}, None
    i = 0
    while i < len(out):
        line = out[i]
        if line.startswith("0x"):
            cur = int(line, 16)
            names[cur] = out[i + 1] if i + 1 < len(out) else "??"   # innermost frame
            i += 3
            # skip the outer frames of an inlined chain (function/location pairs)
            while i < len(out) and not out[i].startswith("0x"):
                i += 2
            continue
        i += 1
    return names


def top(counter, total, n, title):
    print(f"\n{title}")
    for name, count in counter.most_common(n):
        print(f"  {100.0 * count / total:5.1f}%  {count:7d}  {name}")


def main(elf, path):
    data = open(path, "rb").read()
    words = struct.unpack(f"<{len(data) // 4}I", data[: len(data) // 4 * 4])
    samples = [(0x8C000000 + (w & 0xFFFFFF), w >> 24) for w in words]
    names = resolve(elf, sorted({pc for pc, _ in samples}))
    print(f"{len(samples)} samples")
    by_stage = collections.Counter(STAGES[s] if s < len(STAGES) else f"stage{s}" for _, s in samples)
    for stage, count in by_stage.most_common():
        print(f"  {stage:<9}{100.0 * count / len(samples):5.1f}%")
    overall = collections.Counter(names.get(pc, "??") for pc, _ in samples)
    top(overall, len(samples), 30, "Top functions, whole frame")
    dl = collections.Counter(names.get(pc, "??") for pc, s in samples if STAGES[s] == "dl" if s < len(STAGES))
    top(dl, max(1, sum(dl.values())), 30, "Top functions, display-list translation (share of dl samples)")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
