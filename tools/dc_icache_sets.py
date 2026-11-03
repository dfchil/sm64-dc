#!/usr/bin/env python3
"""Where on the SH4's I-cache a DC_PROF_SAMPLE run spends its display-list time, per cache set, to
pick DC_SM_WARM_SET / DC_SM_WARM2_SET: the warm code (gfx_retro_dc.c SM_WARM, SM_WARM2) should sit on
sets that .sub0 leaves cold.

    tools/dc_icache_sets.py build/<variant>/us_dc/sm64.us.f3dex2e build/hw/<run>/sm64_samples.bin [lines]

The I-cache is 8 KB direct-mapped with 32-byte lines: an address's set is (address / 32) % 256. Prints
each .sub0 function's sets and samples, where the warm code lies now, and the coldest windows of
.sub0 for a block of `lines` sets (default: the size of .sub1's warm code).
"""
import struct
import subprocess
import sys

NM = "/opt/toolchains/dc/sh-elf/bin/sh-elf-nm"
OBJDUMP = "/opt/toolchains/dc/sh-elf/bin/sh-elf-objdump"


def sections(elf):
    out = {}
    for line in subprocess.run([OBJDUMP, "-h", elf], capture_output=True, text=True).stdout.splitlines():
        f = line.split()
        if len(f) > 3 and f[1] in (".sub0", ".sub1", ".sub2"):
            out[f[1]] = (int(f[3], 16), int(f[2], 16))
    return out


def symbols(elf, lo, size):
    rows = []
    for line in subprocess.run([NM, "-S", "-n", elf], capture_output=True, text=True).stdout.splitlines():
        f = line.split()
        if len(f) == 4:
            a, s = int(f[0], 16), int(f[1], 16)
            if lo <= a < lo + size and s:
                rows.append((a, s, f[3].lstrip("_")))
    return rows


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    elf, samples = sys.argv[1], sys.argv[2]
    secs = sections(elf)
    data = open(samples, "rb").read()
    pcs = [0x8C000000 | (w & 0xFFFFFF) for (w,) in struct.iter_unpack("<I", data[: len(data) // 4 * 4])]
    heat = [0] * 256       # samples of .sub0 code per set
    lo0, size0 = secs[".sub0"]
    for pc in pcs:
        if lo0 <= pc < lo0 + size0:
            heat[(pc // 32) % 256] += 1
    total = sum(heat)
    print(f"{len(pcs)} samples, {total} in .sub0 ({size0} bytes)")
    for name in (".sub0", ".sub1", ".sub2"):
        lo, size = secs[name]
        for a, s, n in symbols(elf, lo, size):
            first, last = (a // 32) % 256, ((a + s - 1) // 32) % 256
            hits = sum(1 for pc in pcs if a <= pc < a + s)
            print(f"  {name} {n:40s} sets {first:3d}-{last:3d}  {hits:7d} samples")
    lo1, size1 = secs[".sub1"]
    warm = [r for r in symbols(elf, lo1, size1)]
    lines = int(sys.argv[3]) if len(sys.argv) > 3 else max(1, (sum(s for _, s, _ in warm) + 31) // 32)
    # The coldest runs of `lines` sets inside .sub0's span.
    span = (size0 + 31) // 32
    wins = sorted((sum(heat[(s0 + k) % 256] for k in range(lines)), s0) for s0 in range(0, span - lines + 1))
    print(f"coldest {lines}-set windows of .sub0 (samples, first set):")
    for h, s0 in wins[:8]:
        print(f"  sets {s0:3d}-{s0 + lines - 1:3d}: {h} samples ({100.0 * h / max(total, 1):.2f}% of .sub0's)")


if __name__ == "__main__":
    main()
