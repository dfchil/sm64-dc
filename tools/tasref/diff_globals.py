#!/usr/bin/env python3
"""Which globals and statics of the game logic differ between two pc-endjinn host runs at the same
movie sample (SM64_DUMP_GLOBALS_AT, a dump of the executable's __DATA segment), e.g. a TAS slice
replay against the full movie: what a slice still has to carry over.

    tools/tasref/diff_globals.py build/pc-endjinn-gcc/sm64 a/sm64_globals_N.bin b/sm64_globals_N.bin
"""
import bisect
import re
import subprocess
import sys
from pathlib import Path


def segment_vmaddr(exe):
    out = subprocess.run(["otool", "-l", exe], capture_output=True, text=True).stdout
    m = re.search(r"segname __DATA\n\s+vmaddr (0x[0-9a-f]+)", out)
    return int(m.group(1), 16)


def logic_symbols(exe):
    """Data symbols defined by the objects of src/game and src/engine next to the executable."""
    names = set()
    for o in list(Path(exe).parent.glob("src/game/**/*.o")) + list(Path(exe).parent.glob("src/engine/*.o")):
        for line in subprocess.run(["nm", str(o)], capture_output=True, text=True).stdout.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[1] in "dDbBsS":
                names.add(parts[2])
    return names


def main():
    exe, a_path, b_path = sys.argv[1:4]
    base = segment_vmaddr(exe)
    syms = []
    for line in subprocess.run(["nm", "-n", exe], capture_output=True, text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "dDbBsS":
            syms.append((int(parts[0], 16) - base, parts[2]))
    syms.sort()
    starts = [s[0] for s in syms]
    logic = logic_symbols(exe)
    a = open(a_path, "rb").read()
    b = open(b_path, "rb").read()
    # Pointers differ between any two runs (ASLR, malloc): skip aligned 8-byte words where both
    # sides look like one.
    import struct as st
    skip = set()
    for off in range(0, min(len(a), len(b)) - 7, 8):
        x, y = st.unpack_from("<Q", a, off)[0], st.unpack_from("<Q", b, off)[0]
        if x != y and 0x100000000 <= x < 0x800000000000 and 0x100000000 <= y < 0x800000000000:
            skip.update(range(off, off + 8))
    found = {}
    for off in range(min(len(a), len(b))):
        if a[off] != b[off] and off not in skip:
            i = bisect.bisect_right(starts, off) - 1
            if i >= 0:
                name = syms[i][1]
                found.setdefault(name, [syms[i][0], 0])[1] += 1
    for name, (start, count) in sorted(found.items(), key=lambda kv: kv[1][0]):
        size = (starts[starts.index(start) + 1] - start) if starts.index(start) + 1 < len(starts) else 0
        tag = "logic" if name in logic else "     "
        print(f"{tag} {name:48s} {count:6d} of {size:7d} bytes differ")


if __name__ == "__main__":
    main()
