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

Each level pack is fitted to the AICA RAM left after the core pack and the reverb ring (--arena-kb,
--reverb-kb): its music, then its sounds (smallest flow first), then other songs (jingles, event
music). A song left out is loaded from its own files when it plays, so the
songs packed are chosen to leave room for the largest left out where that lets more of them play
(see fit). The bank holds only the samples of what is in the pack.

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


def fit(ranked, core_samples, budget, music):
    """The assets of ranked (in load order) that go in a level pack, and those left out.

    An asset whose samples are all in the core bank costs its flow; any other its flow and the
    samples it adds to the level bank. The level's music and sounds go first while they fit, and
    leave free what the largest other song needs beyond the flows of the level's music, which
    the runtime lets go for it: a sound left out loads from its own files when it plays, but a
    song left out without room is silent. Of the other songs, the set packed is the one that lets the most of them play: a song left out
    plays when the room it needs on its own (its files, which the runtime loads when it plays)
    is free beside the pack; ties go to the larger set packed, which loads nothing mid-scene."""
    core = set(core_samples)

    def cost(a, have):
        s = a.samples()
        if set(s) <= core:
            return len(a.afx)
        return len(a.afx) + sum(len(v) for k, v in s.items() if k not in have)

    def alone(a):
        return len(a.afx) + sum(-len(v) % 32 + len(v) for v in a.samples().values())

    def place(assets, have, used, limit):
        taken, out = [], []
        for a in assets:
            c = cost(a, have)
            if used + c <= limit:
                taken.append(a)
                used += c
                if not set(a.samples()) <= core:
                    have = have | set(a.samples())
            else:
                out.append(a)
        return taken, out, have, used

    later = [a for a in ranked if a.stem.startswith("seq") and a.stem not in music]
    level_music = sum(len(a.afx) for a in ranked if a.stem in music)
    reserve = max(0, max((alone(a) for a in later), default=0) - level_music)
    first, first_out, have, used = place([a for a in ranked if a not in later], set(), 0, budget - reserve)
    best = None
    for bits in range(1 << len(later)):
        packed, out, _, total = place([a for i, a in enumerate(later) if bits >> i & 1], have, used, budget)
        if out:
            continue
        rest = [a for i, a in enumerate(later) if not bits >> i & 1]
        idle = level_music + sum(len(a.afx) for a in packed)
        room = budget - total + idle >= max((alone(a) for a in rest), default=0)
        score = (len(packed) + (len(rest) if room else 0), len(packed))
        if best is None or score > best[0]:
            best = (score, packed, rest, room)
    _, packed, rest, room = best
    for a in first_out:
        print(f"  warning: {a.stem} does not fit", file=sys.stderr)
    if rest and not room:
        print(f"  note: no room beside the whole pack for {' '.join(a.stem for a in rest)} (the runtime lets idle songs go for them)", file=sys.stderr)
    return first + packed, rest + first_out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("assets", type=Path, help="sm64_scene_assets.py --json output")
    ap.add_argument("--stage", type=Path, default=ROOT / "build/afx/stage")
    # afx_mem_stats on hardware: used + free with nothing loaded but the firmware's own state.
    ap.add_argument("--arena-kb", type=int, default=2023, help="AICA RAM arena (default %(default)s)")
    ap.add_argument("--reverb-kb", type=int, default=16, help="reverb ring, 0 without (default %(default)s)")
    ap.add_argument("--margin-kb", type=int, default=32, help="kept free (default %(default)s)")
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
    core_cost = len(core_bank) + sum(len(a.afx) for a in core)
    budget = (args.arena_kb - args.reverb_kb - args.margin_kb) * 1024 - core_cost
    print(f"scene_core.smp: {len(core)} assets, bank {len(core_bank) // 1024} KB, pack {size // 1024} KB; "
          f"{budget // 1024} KB left for a level")
    numbers = level_numbers()
    report = [f"core {len(core)} assets bank {len(core_bank)}"]
    by_number = {}
    for lvl, stems in sorted(lists["levels"].items()):
        if lvl in numbers and stems:
            entry = by_number.setdefault(numbers[lvl], (lvl, set(), set()))
            entry[1].update(stems)
            entry[2].update(lists.get("music", {}).get(lvl, []))
    for number, (lvl, stems, music) in sorted(by_number.items()):
        def rank(a):
            return (0 if a.stem in music else 1 if a.stem.startswith("sfx_") else 2, len(a.afx), a.stem)
        own, left_out = fit(sorted(load(stems), key=rank), core_samples, budget, music)
        on_core = [a for a in own if set(a.samples()) <= set(core_samples)]
        samples = {}
        for a in own:
            if a not in on_core:
                samples.update(a.samples())
        bank, offsets, bank_id = make_bank(samples)
        entries = [(a.stem, 1, a.bound(core_offsets, core_id)) if a in on_core else
                   (a.stem, 0, a.bound(offsets, bank_id)) for a in own]
        # Songs load last: the room one leaves when it lets go joins the free end of AICA RAM.
        entries.sort(key=lambda e: e[0].startswith("seq"))
        size = write_pack(stage / f"scene_{number:02d}.smp", bank, entries)
        flows = sum(struct.unpack_from("<I", e[2], 20)[0] for e in entries)
        out = f" left out {' '.join(a.stem for a in left_out)}" if left_out else ""
        print(f"scene_{number:02d}.smp {lvl:17s} {len(own):3d} assets ({len(on_core)} on core), "
              f"bank {len(bank) // 1024:4d} KB, flows {flows // 1024:4d} KB{out}")
        report.append(f"{number:02d} {lvl} {len(own)} assets bank {len(bank)} flows {flows}{out}")
    (stage / "scenes.txt").write_text("\n".join(report) + "\n")


if __name__ == "__main__":
    main()
