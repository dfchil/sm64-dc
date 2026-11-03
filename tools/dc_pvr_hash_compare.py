#!/usr/bin/env python3
"""Compare the PVR hashes of two runs (DC_PVR_HASH=1 builds, console logs or host files).

    tools/dc_pvr_hash_compare.py A/console.log B/console.log
    tools/dc_pvr_hash_compare.py --dumps A B [ELF_A ELF_B]   (DC_PVR_DUMP_EVERY runs' pvr_<frame>.bin)

With the two builds' ELFs, a dump's textures compare by the symbol their N64 source address falls in
(two builds place the game's data at different addresses); without them, textures are not compared.

Each "PVRH frame hash vertices" line covers the DC_PVR_HASH_EVERY frames up to that frame. TAS
lines in between ("TAS: slice N ...") say where in the benchmark a block is. Prints how many blocks
match, and the first blocks that differ with the slice they fall in.
"""
import os
import re
import struct
import sys

PVRH = re.compile(r"^PVRH (\d+) ([0-9a-f]{8}) (\d+)")
PVRF = re.compile(r"^PVRF (\d+) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8}) (\d+)")   # DC_PVR_HASH_EVERY=1
SLICE = re.compile(r"TAS: slice (\d+)")


def read(path):
    blocks, where, frames, slice_ = {}, {}, {}, None
    with open(path, errors="replace") as f:
        for line in f:
            m = SLICE.search(line)
            if m:
                slice_ = int(m.group(1))
            m = PVRH.match(line)
            if m:
                frame = int(m.group(1))
                blocks[frame] = (m.group(2), int(m.group(3)))
                where[frame] = slice_
            m = PVRF.match(line)
            if m:
                frames[int(m.group(1))] = (m.group(2), m.group(3), m.group(4), int(m.group(5)))
    return blocks, where, frames


def compare_frames(fa, fb):
    """Per frame and per list (OP, PT, TR), from DC_PVR_HASH_EVERY=1 runs."""
    common = sorted(set(fa) & set(fb))
    differ = [f for f in common if fa[f] != fb[f]]
    per_list = [sum(1 for f in differ if fa[f][k] != fb[f][k]) for k in range(3)]
    print(f"frames: {len(fa)} and {len(fb)}, compared {len(common)}, differ {len(differ)} "
          f"(OP {per_list[0]}, PT {per_list[1]}, TR {per_list[2]})")
    return differ


def header_key(words):
    """What of a header reaches the picture: its first four words, without the texture's address
    (uploads land elsewhere when they come in another order) and the fog bit (vertex_key takes the
    fog each vertex gets). The last four hold no colour for these polygons, and the interpreter
    leaves whatever was on the stack in them."""
    # words[7] is the texture's N64 source (DC_PVR_HASH's dump puts it there), compared instead.
    src = words[7] if words[0] & (1 << 3) else 0
    return (words[0], words[1], words[2] & ~(3 << 22), words[3] & ~0x1FFFFF, src, (words[2] >> 22) & 3 == 1)


def vertex_key(v, h):
    """A vertex as it reaches the picture: no u, v without a texture, no fog amount without fog."""
    v = list(v)
    if h is None or not h[0] & (1 << 3):
        v[4] = v[5] = 0
    if h is None or not h[5]:
        v[7] &= 0x00FFFFFF
    return tuple(v)


def symbols(elf):
    """The ELF's data symbols, sorted, to name a texture's source address."""
    import subprocess
    out = subprocess.run(["/opt/toolchains/dc/sh-elf/bin/sh-elf-nm", "-n", "-S", elf], capture_output=True, text=True).stdout
    rows = []
    for line in out.splitlines():
        f = line.split()
        if len(f) == 4 and f[2] in "dDrRbB":
            rows.append((int(f[0], 16), int(f[1], 16), f[3]))
    rows.sort()
    return rows


def source_name(rows, addr):
    import bisect
    if not addr:
        return None
    i = bisect.bisect_right(rows, (addr, 1 << 40, "")) - 1
    if i >= 0 and rows[i][0] <= addr < rows[i][0] + max(rows[i][1], 1):
        return f"{rows[i][2]}+{addr - rows[i][0]:x}"
    return f"?{addr:08x}"


def read_dump(path, syms=None):
    """A DC_PVR_DUMP_EVERY frame: per list (OP, PT, TR), each vertex with the header it draws with.
    The lists are apart because the native renderer sends PT and TR in another order (the PVR draws
    each list in its own pass, so their order does not change the picture)."""
    data = open(path, "rb").read()
    lists, cur = ([], [], []), 0
    hdr = {}
    for i in range(0, len(data) - 35, 36):
        tag = struct.unpack_from("<I", data, i)[0]
        words = struct.unpack_from("<8I", data, i + 4)
        if tag & ~3 == 0x48445230:
            cur = tag & 3
            key = header_key(words)
            # The texture's source: by symbol with the ELF's symbols, else left out.
            hdr[cur] = key[:4] + ((source_name(syms, key[4]) if syms else None),) + key[5:]
        elif tag == 0x56545830:
            lists[cur].append((vertex_key(words, hdr.get(cur)), hdr.get(cur)[:5] if hdr.get(cur) else None))
    return [v for l in lists for v in l]


def f32(w):
    return struct.unpack("<f", struct.pack("<I", w))[0]


def compare_dumps(dir_a, dir_b, elf_a=None, elf_b=None):
    """Vertex by vertex: headers exact, positions and texture coordinates within tolerances."""
    names = sorted(set(os.listdir(dir_a)) & set(os.listdir(dir_b)))
    names = [n for n in names if n.startswith("pvr_") and n.endswith(".bin")]
    syms_a = symbols(elf_a) if elf_a else None
    syms_b = symbols(elf_b) if elf_b else None
    worst = {"xy": 0.0, "z": 0.0, "uv": 0.0, "col": 0}
    total = exact = bad = frames_bad = 0
    for n in sorted(names, key=lambda n: int(n[4:-4])):
        a = read_dump(os.path.join(dir_a, n), syms_a)
        b = read_dump(os.path.join(dir_b, n), syms_b)
        fb = 0
        if len(a) != len(b):
            print(f"  {n}: {len(a)} vs {len(b)} vertices")
            frames_bad += 1
            continue
        for (va, ha), (vb, hb) in zip(a, b):
            total += 1
            if ha == hb and va == vb:
                exact += 1
                continue
            dxy = max(abs(f32(va[1]) - f32(vb[1])), abs(f32(va[2]) - f32(vb[2])))
            za, zb = f32(va[3]), f32(vb[3])
            dz = abs(za - zb) / max(abs(za), abs(zb), 1e-9)
            textured = ha is not None and ha[0] & (1 << 3)   # else u, v are whatever the bake left
            duv = max(abs(f32(va[4]) - f32(vb[4])), abs(f32(va[5]) - f32(vb[5]))) if textured else 0.0
            dcol = max(abs(((va[k] >> s) & 255) - ((vb[k] >> s) & 255)) for k in (6, 7) for s in (0, 8, 16, 24))
            worst["xy"] = max(worst["xy"], dxy)
            worst["z"] = max(worst["z"], dz)
            worst["uv"] = max(worst["uv"], duv)
            worst["col"] = max(worst["col"], dcol)
            # Beyond rounding: an eighth of a pixel, a relative depth of 1e-4, a 1/4096 texel step at
            # the PVR's [0, 1] coordinates, one colour step, or another header.
            if ha != hb or va[0] != vb[0] or dxy > 0.125 or dz > 1e-4 or duv > 1.0 / 4096 or dcol > 1:
                bad += 1
                fb += 1
                if bad <= 10:
                    print(f"  {n}: flags {va[0]:08x}/{vb[0]:08x} xy {dxy:.4f} z {dz:.2e} uv {duv:.2e} col {dcol}"
                          f"{' header differs' if ha != hb else ''}")
        frames_bad += fb > 0
    print(f"dumps: {len(names)} frames, {total} vertices, {exact} identical, {bad} beyond rounding "
          f"(in {frames_bad} frames); largest differences: xy {worst['xy']:.4f} px, z {worst['z']:.2e} "
          f"relative, uv {worst['uv']:.2e}, colour {worst['col']}")
    return 1 if bad or frames_bad else 0


def main():
    if len(sys.argv) in (4, 6) and sys.argv[1] == "--dumps":
        return compare_dumps(*sys.argv[2:])
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    a, wa, fa = read(sys.argv[1])
    b, _, fb = read(sys.argv[2])
    if fa and fb:
        return 1 if compare_frames(fa, fb) else 0
    common = sorted(set(a) & set(b))
    same = [f for f in common if a[f] == b[f]]
    differ = [f for f in common if a[f] != b[f]]
    print(f"blocks: {len(a)} and {len(b)}, compared {len(common)}, same {len(same)}, differ {len(differ)}")
    for f in differ[:20]:
        print(f"  frame {f} (slice {wa[f]}): {a[f][0]} {a[f][1]} verts vs {b[f][0]} {b[f][1]} verts")
    if differ:
        slices = sorted({wa[f] for f in differ}, key=lambda s: -1 if s is None else s)
        print("slices with differences:", ", ".join(str(s) for s in slices))
    return 1 if differ or not common else 0


if __name__ == "__main__":
    sys.exit(main())
