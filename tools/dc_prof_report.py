#!/usr/bin/env python3
"""Summarise the DCPROF lines of one or more DC_PROF console logs.

    tools/dc_prof_report.py [--late] build/hw/os_vbl2-*/console.log [more logs ...]

One table per log: per scene, frames, late frames and the average (max) of each
frame component in milliseconds, then a level summary (late frames by cause,
the load hitch left out, on-time share), the title screens between the demos
on one line. DC_PROF_SKIP=999 runs add the PVR render time per level by what
was left out. --late lists every late frame. With several logs, a final table
compares the average frame total per scene across them.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
COMPONENTS = ["total", "logic", "geo", "dl", "wait", "finish", "vsync", "audio", "render"]
CAUSES = ["cpu", "pvr", "both", "load", "other"]
SKIP_NAMES = {0: "nothing", 1: "PT list", 2: "TR list", 3: "all 2D", 4: "2D TR", 5: "3D TR", 6: "sky (OP far)",
              7: "3D OP", 8: "full-screen 2D TR", 9: "2D OP", 10: "shadows"}


def level_names():
    names = ["NONE"]
    for line in (ROOT / "levels" / "level_defines.h").read_text().splitlines():
        m = re.match(r"\s*(?:STUB|DEFINE)_LEVEL\([^,]*,\s*LEVEL_(\w+)", line)
        if m:
            names.append(m.group(1))
    return names


def parse(path):
    run, scenes, counts, late, skip = None, [], {}, [], {}
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
        elif kind == "LATE":
            late.append(fields)
        elif kind == "SKIP":
            skip.setdefault((fields["level"], fields["area"], fields["demo"]), {})[int(fields["code"])] = fields
    return run, scenes, counts, late, skip


def scene_name(fields, names):
    level = int(fields["level"])
    name = names[level] if level < len(names) else str(level)
    return f"{name}:{fields['area']}{' demo' if fields['demo'] == '1' else ''}"


def is_title(fields):
    # Between demos the game sits on the title screen (the Mario head) with the previous level's
    # number still set: a non-demo scene of a real level.
    return fields["demo"] == "0" and int(fields["level"]) > 1


def ms(us):
    return f"{int(us) / 1000:.1f}"


def pct(n, d):
    return f"{100.0 * n / d:.2f}%" if d else "-"


def level_summary(scenes, names):
    print(f"\n  level summary (late frames by cause; 'excl load' leaves the level-load hitch out)")
    header = (f"  {'scene':<24}{'frames':>7}{'late':>6}{'excl load':>10}{'on time':>9}"
              + "".join(f"{c:>7}" for c in CAUSES) + f"{'dl ms':>7}{'pvr ms':>8}{'pvr max':>8}{'vram MB':>8}")
    print(header + "\n  " + "-" * (len(header) - 2))
    title = {"frames": 0, "late": 0, **{c: 0 for c in CAUSES}, "render": 0, "rmax": 0, "dl": 0}
    rows = []
    for s in scenes:
        by = {c: int(s.get(f"late_{c}", 0)) for c in CAUSES}
        if is_title(s):
            title["frames"] += int(s["frames"])
            title["late"] += int(s["late"])
            for c in CAUSES:
                title[c] += by[c]
            title["render"] += int(s["render"].split("/")[0]) * int(s["frames"])
            title["dl"] += int(s["dl"].split("/")[0]) * int(s["frames"])
            title["rmax"] = max(title["rmax"], int(s["render"].split("/")[1]))
            continue
        rows.append((scene_name(s, names), int(s["frames"]), int(s["late"]), by,
                     int(s["dl"].split("/")[0]), int(s["render"].split("/")[0]), int(s["render"].split("/")[1]),
                     int(s.get("vram_free", 0))))
    if title["frames"]:
        f = title["frames"]
        rows.append(("TITLE (between demos)", f, title["late"], {c: title[c] for c in CAUSES},
                     title["dl"] // f, title["render"] // f, title["rmax"], 0))
    total_f = total_l = total_x = 0
    for name, frames, late, by, dl, render, rmax, vram in rows:
        excl = late - by["load"]
        if not name.startswith("TITLE") and "demo" in name:
            total_f += frames
            total_l += late
            total_x += excl
        print(f"  {name:<24}{frames:>7}{late:>6}{excl:>10}{pct(frames - excl, frames):>9}"
              + "".join(f"{by[c]:>7}" for c in CAUSES)
              + f"{dl / 1000:>7.1f}{render / 1000:>8.1f}{rmax / 1000:>8.1f}{(vram / 1048576) if vram else 0:>8.2f}")
    if total_f:
        print(f"  {'all demos':<24}{total_f:>7}{total_l:>6}{total_x:>10}{pct(total_f - total_x, total_f):>9}")


def skip_table(scenes, skip, names):
    if not skip:
        return
    codes = sorted({c for per in skip.values() for c in per})
    print(f"\n  PVR render ms per level by what was left out (DC_PROF_SKIP=999); delta against nothing left out")
    print(f"  {'scene':<24}" + "".join(f"{SKIP_NAMES.get(c, c)[:13]:>14}" for c in codes))
    for s in scenes:
        per = skip.get((s["level"], s["area"], s["demo"]))
        if not per or is_title(s):
            continue
        base = int(per[0]["render"].split("/")[0]) if 0 in per else None
        cells = []
        for c in codes:
            if c not in per:
                cells.append(f"{'-':>14}")
                continue
            avg = int(per[c]["render"].split("/")[0])
            if c == 0 or base is None:
                cells.append(f"{avg / 1000:>14.2f}")
            else:
                cells.append(f"{avg / 1000:>7.2f}{(avg - base) / 1000:>+7.2f}")
        print(f"  {scene_name(s, names):<24}" + "".join(cells))


def main(paths, show_late):
    names = level_names()
    totals = {}
    for path in paths:
        run, scenes, counts, late, skip = parse(path)
        if run is None:
            print(f"{path}: no DCPROF RUN line (did the build exit by itself?)")
            continue
        print(f"\n{path}\n  frames={run['frames']} late={run['late']} "
              f"vbl_wait={run['vbl_wait']} seconds={run['seconds']} fsaa={run.get('fsaa', '?')}")
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
        if any("late_cpu" in s for s in scenes):
            level_summary(scenes, names)
        skip_table(scenes, skip, names)
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
        if show_late and late:
            print(f"\n  late frames ({len(late)})")
            print(f"  {'scene':<24}{'frame':>7}{'since':>6}{'fields':>7}{'cause':>7}{'logic':>7}{'geo':>6}"
                  f"{'dl':>7}{'wait':>7}{'render':>7}{'audio':>7}")
            for l in late:
                print(f"  {scene_name(l, names):<24}{l['frame']:>7}{l['since']:>6}{l['fields']:>7}{l['cause']:>7}"
                      f"{ms(l['logic']):>7}{ms(l['geo']):>6}{ms(l['dl']):>7}{ms(l['wait']):>7}{ms(l['render']):>7}"
                      f"{ms(l['audio']):>7}")
    if len(paths) > 1:
        print("\nAverage frame total (ms) per scene")
        print(f"  {'scene':<24}" + "".join(f"{Path(p).parent.name[:22]:>24}" for p in paths))
        for name, by_path in totals.items():
            print(f"  {name:<24}" + "".join(f"{ms(by_path[p]) if p in by_path else '-':>24}" for p in paths))


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--late"]
    if not args:
        sys.exit(__doc__)
    main(args, "--late" in sys.argv)
