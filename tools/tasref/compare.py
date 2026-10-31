#!/usr/bin/env python3
"""The first movie sample where two traces differ (tasref's N64 one, the pc-endjinn host's
SM64_TRACE one), with the fields that differ decoded and the samples around it.

    tools/tasref/compare.py n64.trace host.trace [--context 5]
    tools/tasref/compare.py n64.trace build/hw/<run>/console.log

A Dreamcast console log (make DC_TAS=1) holds a TASREF line every DC_TAS_TRACE_EVERY samples:
those are compared with the N64's lines for the same samples.
"""
import argparse
import struct


def parse(line):
    head, *fields = line.split()
    out = {"sample": int(head)}
    for f in fields:
        k, v = f.split("=", 1)
        out[k] = v
    return out


def show(key, value):
    """Floats are logged as their bits: give the number too."""
    if key in ("pos", "vel", "fv", "cf", "cp"):
        nums = [struct.unpack("<f", struct.pack("<I", int(x, 16)))[0] for x in value.split(",")]
        return value + " (" + ", ".join(f"{n:.6g}" for n in nums) + ")"
    return value


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("n64")
    ap.add_argument("host")
    ap.add_argument("--context", type=int, default=5)
    args = ap.parse_args()
    a = open(args.n64).read().splitlines()
    b = open(args.host, errors="replace").read().splitlines()
    tasref = [l.split("TASREF ", 1)[1] for l in b if "TASREF " in l]
    if tasref:
        last = None
        for line in tasref:
            n = int(line.split()[0])
            if n >= len(a):
                break
            if a[n] != line:
                pa, pb = parse(a[n]), parse(line)
                print(f"console differs at sample {n} (n64 level {pa.get('L')} area {pa.get('A')}); "
                      f"last equal: {last}")
                for k in pa:
                    if pa[k] != pb.get(k):
                        print(f"  {k:4s} n64  {show(k, pa[k])}\n       dc   {show(k, pb.get(k, '-'))}")
                return
            last = n
        print(f"console matches the N64 at all {len(tasref)} traced samples (up to {last})")
        return
    if b and not b[0].split(" ", 1)[0].isdigit():
        print(f"{args.host}: neither a trace nor a console log with TASREF lines")
        return
    n = min(len(a), len(b))
    first = next((i for i in range(n) if a[i] != b[i]), None)
    if first is None:
        print(f"identical for {n} samples (n64 {len(a)}, host {len(b)})")
        return
    pa, pb = parse(a[first]), parse(b[first])
    print(f"first difference at sample {first} (n64 level {pa.get('L')} area {pa.get('A')}, timer {pa.get('t')})")
    for k in pa:
        if pa[k] != pb.get(k):
            print(f"  {k:4s} n64  {show(k, pa[k])}\n       host {show(k, pb.get(k, '-'))}")
    lo = max(0, first - args.context)
    print("\nbefore and after (n64 / host):")
    for i in range(lo, min(n, first + args.context + 1)):
        mark = "*" if a[i] != b[i] else " "
        print(f"{mark} {a[i]}\n{mark} {b[i]}")


if __name__ == "__main__":
    main()
