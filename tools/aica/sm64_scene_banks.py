#!/usr/bin/env python3
"""Scene packs with one shared sample bank each (pack version 2), from sm64_scene_assets.py's lists.

    tools/aica/sm64_scene_assets.py --json build/afx/scene_assets.json
    tools/aica/sm64_scene_banks.py build/afx/scene_assets.json --stage build/afx/stage

Every staged asset carries its own AFB with copies of the samples it uses, so samples that songs
and sounds share were loaded many times over. Here each pack holds one AFB with every sample its
flows use, once, and the flows are rewritten to bind to it: each relocation's sample offset, the
address in the setup it names (CONTROL's low 7 bits and SAMPLE_LOW, which the loader checks
against the relocation), the bank identity and the control id. AICAflow's runtime compares bank
identities from the headers only, so a bank built here is as good as one from AICAforge.

scene_core.smp holds the global set, read once; scene_LL.smp (LL = gCurrLevelNum, decimal) the
level's own assets. A level flow whose samples are all in the core bank binds to that instead,
so its samples are not in the level bank at all.

Pack format (little-endian): "SMPK", version 2, entry count, bank offset, bank bytes (a whole
AFB file); per entry a 24-byte NUL-padded stem, the bank it binds (0 = this pack's, 1 = the core
pack's), the offset and size of its AFX in the file, and a reserved word; files 32-byte aligned.
"""
import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
MAGIC = b"SMPK"
ENTRY = struct.Struct("<24s4I")
SETUP_BYTES = 36


def level_numbers():
    """level folder -> gCurrLevelNum, in levels/level_defines.h order (LEVEL_NONE is 0)."""
    out, n = {}, 0
    for line in (ROOT / "levels/level_defines.h").read_text().splitlines():
        m = re.match(r"\s*(DEFINE_LEVEL|STUB_LEVEL)\((.*)\)", line)
        if not m:
            continue
        n += 1
        args = [a.strip() for a in m[2].split(",")]
        if m[1] == "DEFINE_LEVEL":
            out[args[3]] = n
    # The title screen, file select and star select run as LEVEL_UNKNOWN_1 (levels/intro, levels/menu).
    out["intro"] = out["menu"] = 1
    return out


class Asset:
    def __init__(self, stage, stem):
        self.stem = stem
        self.afx = bytearray((stage / f"{stem}.afx").read_bytes())
        afb = (stage / f"{stem}.afb").read_bytes()
        off, size = struct.unpack_from("<II", afb, 16)
        payload = afb[off:off + size]
        ro, rc = struct.unpack_from("<II", self.afx, 48)
        self.relocs = []
        for i in range(rc):
            pair, so, sb = struct.unpack_from("<III", self.afx, ro + 12 * i)
            self.relocs.append((pair, payload[so:so + sb]))

    def samples(self):
        return {hashlib.sha1(data).digest(): data for _, data in self.relocs}

    def bound(self, offsets, bank_id):
        """The AFX rewritten for a bank: offsets maps sample digest -> offset in it."""
        x = bytearray(self.afx)
        image_off, image_size = struct.unpack_from("<II", x, 16)
        ro, _ = struct.unpack_from("<II", x, 48)
        for i, (pair, data) in enumerate(self.relocs):
            new = offsets[hashlib.sha1(data).digest()]
            struct.pack_into("<I", x, ro + 12 * i + 4, new)
            at = image_off + pair
            control = struct.unpack_from("<H", x, at)[0]
            struct.pack_into("<HH", x, at, (control & ~0x7F) | (new >> 16), new & 0xFFFF)
        struct.pack_into("<II", x, 40, *bank_id)
        image = bytes(x[image_off:image_off + image_size])
        h = 2166136261
        for b in image:
            h = ((h ^ b) * 16777619) & 0xFFFFFFFF
        struct.pack_into("<I", x, 32, h or 1)
        return bytes(x)


def make_bank(samples):
    """An AFB holding samples (digest -> bytes) 32-byte aligned; returns (file, offsets, id)."""
    payload, offsets = bytearray(), {}
    for digest, data in samples.items():
        payload += bytes(-len(payload) % 32)
        offsets[digest] = len(payload)
        payload += data
    payload += bytes(-len(payload) % 32)
    d = hashlib.sha1(payload).digest()
    bank_id = (struct.unpack_from("<I", d, 0)[0] | 1, struct.unpack_from("<I", d, 4)[0])
    header = b"AFB\0" + struct.pack("<IIIIIII", 1, bank_id[0], bank_id[1], 32, len(payload), 32 + len(payload), 0)
    return header + bytes(payload), offsets, bank_id


def write_pack(path, bank_file, entries):
    """entries: (stem, bank index, afx bytes)."""
    out = bytearray(MAGIC + struct.pack("<4I", 2, len(entries), 0, 0) + bytes(ENTRY.size * len(entries)))
    out += bytes(-len(out) % 32)
    struct.pack_into("<II", out, 12, len(out), len(bank_file))
    out += bank_file
    for i, (stem, which, afx) in enumerate(entries):
        out += bytes(-len(out) % 32)
        ENTRY.pack_into(out, 20 + i * ENTRY.size, stem.encode(), which, len(out), len(afx), 0)
        out += afx
    path.write_bytes(bytes(out))
    return len(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("assets", type=Path, help="sm64_scene_assets.py --json output")
    ap.add_argument("--stage", type=Path, default=ROOT / "build/afx/stage")
    args = ap.parse_args()
    stage = args.stage / "afx"
    lists = json.loads(args.assets.read_text())

    def load(stems):
        out = []
        for s in stems:
            if (stage / f"{s}.afx").exists() and (stage / f"{s}.afb").exists():
                out.append(Asset(stage, s))
        return out

    core = load(lists["global"])
    core_samples = {}
    for a in core:
        core_samples.update(a.samples())
    core_bank, core_offsets, core_id = make_bank(core_samples)
    size = write_pack(stage / "scene_core.smp", core_bank,
                      [(a.stem, 0, a.bound(core_offsets, core_id)) for a in core])
    print(f"scene_core.smp: {len(core)} assets, bank {len(core_bank) // 1024} KB, pack {size // 1024} KB")
    numbers = level_numbers()
    report = [f"core {len(core)} assets bank {len(core_bank)}"]
    by_number = {}
    for lvl, stems in sorted(lists["levels"].items()):
        if lvl in numbers and stems:
            by_number.setdefault(numbers[lvl], (lvl, set()))[1].update(stems)
    for number, (lvl, stems) in sorted(by_number.items()):
        own = load(sorted(stems))
        on_core = [a for a in own if set(a.samples()) <= set(core_samples)]
        rest = [a for a in own if a not in on_core]
        samples = {}
        for a in rest:
            samples.update(a.samples())
        bank, offsets, bank_id = make_bank(samples)
        entries = [(a.stem, 1, a.bound(core_offsets, core_id)) for a in on_core]
        entries += [(a.stem, 0, a.bound(offsets, bank_id)) for a in rest]
        size = write_pack(stage / f"scene_{number:02d}.smp", bank, entries)
        flows = sum(struct.unpack_from("<I", e[2], 20)[0] for e in entries)
        print(f"scene_{number:02d}.smp {lvl:17s} {len(own):3d} assets ({len(on_core)} on core), "
              f"bank {len(bank) // 1024:4d} KB, flows {flows // 1024:4d} KB")
        report.append(f"{number:02d} {lvl} {len(own)} assets bank {len(bank)} flows {flows}")
    (stage / "scenes.txt").write_text("\n".join(report) + "\n")


if __name__ == "__main__":
    main()
