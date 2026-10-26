#!/usr/bin/env python3
"""Scene packs for DC_AICAFLOW builds: each level's songs and sounds in one file, read when the level loads.

    tools/aica/sm64_scene_packs.py --stage build/afx/stage build/hw/<run>/console.log [more logs ...]

The logs are DC_PROF runs of a DC_AICAFLOW build: their DCPROF ASSET lines name every song and sound the
game loaded, in which level. An asset loaded in --core-min levels or more goes into scene_core.smp, which the
game reads once (in practice the title song, which every round of the title loads again); the rest of each
level's assets go into scene_LL.smp (LL = level number, decimal, as gCurrLevelNum). The game
(src/pc/audio/sm64_aicaflow.c, sm64_afx_scene_prepare) reads the level's pack in init_level, before the level
starts. Anything a pack misses still loads from /pc when it first plays, and shows up as an ASSET line of
its own in the next run: add that run's log and run this again.

A sound shows up only in the first level that loaded it, because it then stays loaded. A level reached in
another order than the recorded one therefore loads such sounds from /pc as before: record the order that
matters, or add more logs.

Pack format (little-endian): "SMPK", version 1, entry count; per entry a 24-byte NUL-padded stem and the
offset and size of its .afb and of its .afx in the file; then the files, each 32-byte aligned. Also writes
scenes.txt, the manifest in readable form.
"""
import argparse
import collections
import re
import struct
import sys
from pathlib import Path

MAGIC = b'SMPK'
ENTRY = struct.Struct('<24s4I')
ASSET = re.compile(r'DCPROF ASSET level=(\d+) area=\d+ demo=\d frame=\d+ us=\d+ bytes=\d+ ok=1 name=(\S+)')


def scene_assets(logs):
    """level -> set of stems, from the logs' successful single-asset loads (packs themselves excluded)."""
    levels = collections.defaultdict(set)
    for log in logs:
        for level, stem in ASSET.findall(Path(log).read_text(errors='replace')):
            if stem.startswith(('seq', 'sfx_')):
                levels[int(level)].add(stem)
    return levels


def split_core(levels, core_min):
    # A sound stays loaded once played, so a log shows it in the first level that used it only; a song
    # is freed when another plays and shows up again in each level that plays it (the title's).
    counts = collections.Counter(s for stems in levels.values() for s in stems)
    core = {s for s, n in counts.items() if n >= core_min}
    return core, {level: stems - core for level, stems in levels.items()}


def write_pack(path, stems, stage):
    """Returns (assets packed, pack bytes, stems without staged files)."""
    present = [s for s in sorted(stems) if (stage / f'{s}.afb').exists() and (stage / f'{s}.afx').exists()]
    missing = sorted(set(stems) - set(present))
    out = bytearray(MAGIC + struct.pack('<2I', 1, len(present)) + bytes(ENTRY.size * len(present)))
    for i, stem in enumerate(present):
        placed = []
        for ext in ('afb', 'afx'):
            data = (stage / f'{stem}.{ext}').read_bytes()
            out += bytes(-len(out) % 32)   # each file 32-byte aligned in the pack
            placed += [len(out), len(data)]
            out += data
        ENTRY.pack_into(out, 12 + i * ENTRY.size, stem.encode(), *placed)
    path.write_bytes(bytes(out))
    return len(present), len(out), missing


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('logs', nargs='+', type=Path, help='console logs of DC_PROF DC_AICAFLOW runs')
    parser.add_argument('--stage', type=Path, required=True, help='stage dir holding afx/ (tools/aica/sm64_afx_batch.py)')
    parser.add_argument('--core-min', type=int, default=2, help='an asset loaded in this many levels goes in the core pack')
    args = parser.parse_args(argv)
    stage = args.stage / 'afx'
    if not stage.is_dir():
        sys.exit(f'{stage}: no such directory')
    levels = scene_assets(args.logs)
    if not levels:
        sys.exit('no DCPROF ASSET lines in the logs (a DC_PROF DC_AICAFLOW build logs them)')
    core, per_level = split_core(levels, args.core_min)
    lines = []
    n, size, missing = write_pack(stage / 'scene_core.smp', core, stage)
    lines.append(f'core {n} assets {size} bytes: ' + ' '.join(sorted(core)))
    print(f'scene_core.smp: {n} assets, {size // 1024} KB' + (f'; no files for {missing}' if missing else ''))
    for level in sorted(per_level):
        stems = per_level[level]
        if not stems:
            continue
        n, size, missing = write_pack(stage / f'scene_{level:02d}.smp', stems, stage)
        lines.append(f'{level:02d} {n} assets {size} bytes: ' + ' '.join(sorted(stems)))
        print(f'scene_{level:02d}.smp: {n} assets, {size // 1024} KB' + (f'; no files for {missing}' if missing else ''))
    (stage / 'scenes.txt').write_text('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
