#!/usr/bin/env python3
"""Summarise the DCPROF lines of one or more DC_PROF console logs.

    tools/dc_prof_report.py build/hw/os_vbl2-*/console.log [more logs ...]

One table per log: per scene, frames, late frames and the average (max) of each
frame component in milliseconds. With several logs, a final table compares the
average frame total per scene across them.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
COMPONENTS = ["total", "logic", "geo", "dl", "wait", "finish", "vsync", "audio", "render"]


def level_names():
    names = ["NONE"]
    for line in (ROOT / "levels" / "level_defines.h").read_text().splitlines():
        m = re.match(r"\s*(?:STUB|DEFINE)_LEVEL\([^,]*,\s*LEVEL_(\w+)", line)
        if m:
            names.append(m.group(1))
    return names


def parse(path):
    run, scenes, counts = None, [], {}
    for line in Path(path).read_text(errors="replace").splitlines():
        if not line.startswith("DCPROF "):
            continue
        fields = dict(kv.split("=", 1) for kv in line.split()[2:] if "=" in kv)
        kind = line.split()[1]
        if kind == "RUN":
            run = fields
        elif kind == "SCENE":
            scenes.append(fields)
        elif kind == "COUNTS":
            counts[(fields["level"], fields["area"], fields["demo"])] = fields
    return run, scenes, counts


def scene_name(fields, names):
    level = int(fields["level"])
    name = names[level] if level < len(names) else str(level)
    return f"{name}:{fields['area']}{' demo' if fields['demo'] == '1' else ''}"


def ms(us):
    return f"{int(us) / 1000:.1f}"


def main(paths):
    names = level_names()
    totals = {}
    for path in paths:
        run, scenes, counts = parse(path)
        if run is None:
            print(f"{path}: no DCPROF RUN line (did the build exit by itself?)")
            continue
        print(f"\n{path}\n  frames={run['frames']} late={run['late']} "
              f"vbl_wait={run['vbl_wait']} seconds={run['seconds']}")
        header = f"  {'scene':<24}{'frames':>7}{'late':>6}" + "".join(f"{c:>13}" for c in COMPONENTS)
        print(header + "\n  " + "-" * (len(header) - 2))
        for s in scenes:
            cells = []
            for c in COMPONENTS:
                avg, mx = s[c].split("/")
                cells.append(f"{ms(avg)} ({ms(mx)})".rjust(13))
            name = scene_name(s, names)
            print(f"  {name:<24}{s['frames']:>7}{s['late']:>6}" + "".join(cells))
            totals.setdefault(name, {})[path] = s["total"].split("/")[0]
        if counts:
            names_c = ["tris", "fast", "bakes", "epochs", "setups", "verts"]
            lists_c = ["op_tris", "op_px", "pt_tris", "pt_px", "pt_opq_px", "tr_tris", "tr_px", "tr_opq_tris",
                       "tr_opq_px"]
            print(f"\n  per-frame counts{'':<9}" + "".join(f"{c:>9}" for c in names_c) +
                  "  dl-kcyc  dstall%  istall%  insn/cyc")
            for s in scenes:
                c = counts.get((s["level"], s["area"], s["demo"]))
                if c:
                    cyc = int(c.get("cycles", 0))
                    pmu = ""
                    if cyc:
                        pmu = (f"{cyc / 1000:8.0f} {100 * int(c['dstall']) / cyc:8.1f} "
                               f"{100 * int(c['istall']) / cyc:8.1f} {int(c['insns']) / cyc:9.2f}")
                    print(f"  {scene_name(s, names):<24}" + "".join(f"{c[n]:>9}" for n in names_c) + "  " + pmu)
            # DC_PROF_LISTS builds: per PVR list, triangles and on-screen pixels per frame, and the
            # share of PT/TR pixels whose alpha is 1 everywhere (could be drawn in the OP list).
            if any(int(c.get("op_px", 0)) for c in counts.values()):
                print(f"\n  per-frame list cost{'':<5}{'OP tris':>9}{'OP kpx':>8}{'PT tris':>9}{'PT kpx':>8}"
                      f"{'PT opq%':>8}{'TR tris':>9}{'TR kpx':>8}{'TR opq%':>8}{'TR opq tris':>12}")
                for s in scenes:
                    c = counts.get((s["level"], s["area"], s["demo"]))
                    if not c:
                        continue
                    v = {n: int(c.get(n, 0)) for n in lists_c}
                    pt_pct = 100 * v["pt_opq_px"] / v["pt_px"] if v["pt_px"] else 0
                    tr_pct = 100 * v["tr_opq_px"] / v["tr_px"] if v["tr_px"] else 0
                    print(f"  {scene_name(s, names):<24}{v['op_tris']:>9}{v['op_px'] / 1000:>8.0f}{v['pt_tris']:>9}"
                          f"{v['pt_px'] / 1000:>8.0f}{pt_pct:>7.0f}%{v['tr_tris']:>9}{v['tr_px'] / 1000:>8.0f}"
                          f"{tr_pct:>7.0f}%{v['tr_opq_tris']:>12}")
    if len(paths) > 1:
        print("\nAverage frame total (ms) per scene")
        print(f"  {'scene':<24}" + "".join(f"{Path(p).parent.name[:22]:>24}" for p in paths))
        for name, by_path in totals.items():
            print(f"  {name:<24}" + "".join(f"{ms(by_path[p]) if p in by_path else '-':>24}" for p in paths))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])
