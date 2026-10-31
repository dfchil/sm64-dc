#!/usr/bin/env python3
"""Lists a TAS slices file (src/game/tas_slice.c; the host writes it with SM64_SLICE_CAPTURE),
and with --first-per-level prints a playlist of each level's first slice.

    tools/tasref/slices.py tas_slices.bin [--first-per-level]
"""
import argparse
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent


def level_names():
    out, n = {}, 0
    for line in (ROOT / "levels/level_defines.h").read_text().splitlines():
        m = re.match(r"\s*(DEFINE_LEVEL|STUB_LEVEL)\((.*)\)", line)
        if m:
            n += 1
            out[n] = [a.strip() for a in m[2].split(",")][1].replace("LEVEL_", "") if m[1] == "DEFINE_LEVEL" else "?"
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("slices", type=Path)
    ap.add_argument("--first-per-level", action="store_true")
    args = ap.parse_args()
    data = args.slices.read_bytes()
    magic, size = struct.unpack_from("<II", data, 0)
    assert magic == 0x31534C53, "not a slices file"
    names = level_names()
    count = (len(data) - 8) // size
    seen, playlist = set(), []
    for i in range(count):
        sample, wtype, level, area, node, arg, seed = struct.unpack_from("<IBBBBIH", data, 8 + i * size)
        if not args.first_per_level:
            print(f"{i:3d} sample {sample:6d} {names.get(level, level):10s} L{level:2d} area {area} node {node:3d} seed {seed:04x}")
        elif level not in seen:
            seen.add(level)
            playlist.append(str(i))
    if args.first_per_level:
        print(",".join(playlist))


if __name__ == "__main__":
    main()
