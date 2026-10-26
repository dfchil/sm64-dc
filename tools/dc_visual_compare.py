#!/usr/bin/env python3
"""Side-by-side stills of two Dreamcast recordings (tools/dc_visual_run.zsh), aligned on the
title screen's white fade, so a build can be checked against a reference frame by frame.

    tools/dc_visual_compare.py build/hw/visual-ref-*/video.mp4 build/hw/visual-new-*/video.mp4 [--out DIR]

Both recordings must start the same way (a plain build: title screen, then the attract demo). The
anchor is the first frame whose average luma is over 200, the white fade before the star iris.
Writes three sheets into DIR (default: next to the second video), top row = first video:
  title.jpg       the Mario head, every 2 s from 24 s before the fade (envmap highlight, sparkles)
  transition.jpg  the white fade and the star iris, 5 frames a second for 2.4 s
  gameplay.jpg    the first attract demo, every 4 s from 3 s after the fade (shadows, cutouts, HUD)
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path


def white_anchor(video):
    out = subprocess.run(["ffmpeg", "-hide_banner", "-nostats", "-i", str(video), "-vf",
                          "signalstats,metadata=print:key=lavfi.signalstats.YAVG", "-f", "null", "-"],
                         capture_output=True, text=True).stderr
    t = None
    for line in out.splitlines():
        m = re.search(r"pts_time:([\d.]+)", line)
        if m:
            t = float(m.group(1))
        m = re.search(r"YAVG=([\d.]+)", line)
        if m and t is not None and float(m.group(1)) > 200:
            return t
    sys.exit(f"{video}: no white fade found (is it a plain build that reached the attract demo?)")


def strip(video, start, step, count, width, tmp):
    """One row of `count` frames, `step` seconds apart from `start`, each `width` pixels wide."""
    tmp.mkdir(parents=True, exist_ok=True)
    frames = []
    for i in range(count):
        f = tmp / f"{i:02d}.png"
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-ss", f"{max(start + i * step, 0):.3f}",
                        "-i", str(video), "-frames:v", "1", "-vf", f"scale={width}:-1", "-y", str(f)], check=True)
        frames.append(f)
    row = tmp / "row.png"
    inputs = sum((["-i", str(f)] for f in frames), [])
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", *inputs, "-filter_complex",
                    f"hstack=inputs={count}" if count > 1 else "null", "-y", str(row)], check=True)
    return row


def sheet(name, videos, anchors, offset, step, count, width, out):
    rows = [strip(v, a + offset, step, count, width, out / f".{name}{i}") for i, (v, a) in enumerate(zip(videos, anchors))]
    dst = out / f"{name}.jpg"
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", str(rows[0]), "-i", str(rows[1]),
                    "-filter_complex", "vstack=inputs=2", "-y", str(dst)], check=True)
    return dst


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("reference")
    ap.add_argument("candidate")
    ap.add_argument("--out")
    args = ap.parse_args()
    videos = [Path(args.reference), Path(args.candidate)]
    out = Path(args.out) if args.out else videos[1].parent / "compare"
    out.mkdir(parents=True, exist_ok=True)
    anchors = [white_anchor(v) for v in videos]
    print(f"white fade at {anchors[0]:.2f} s / {anchors[1]:.2f} s")
    for dst in (sheet("title", videos, anchors, -24.0, 2.0, 8, 240, out),
                sheet("transition", videos, anchors, 0.0, 0.2, 12, 160, out),
                sheet("gameplay", videos, anchors, 3.0, 4.0, 8, 240, out)):
        print(dst)


if __name__ == "__main__":
    main()
