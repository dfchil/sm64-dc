#!/usr/bin/env python3
"""Turn a DC_AUDIO_RECORD log into AICAflow assets.

    tools/aica/sm64_afx.py build/hw/<run>/sm64_audio_rec.bin \\
        --table build/rec/<variant>/us_dc/sound/aica_sample_table.c \\
        --pool build/rec/<variant>/us_dc/sound/aica_pool.bin \\
        --out build/afx/bob [--loop] [--player 0] [--sub-rate HZ] [--afx-sm64 build/afx_sm64]

The log (src/audio/aica_record.h) holds every AICA voice command the game's
driver sent, stamped with the sequencer sub-update. Each START becomes a NOTE
on the same local channel with the sample's setup, UPDATEs become PATCHes of
the register words that changed, STOPs KEYOFFs. Register words are what KOS's
AICA driver writes for the same freq/vol/pan, so the result sounds as the port
does today.

Time: the game runs gAudioUpdatesPerFrame sub-updates per vblank; the log's
END record gives the rate measured on the console (--sub-rate overrides it).
AICAflow runs one authored tick per tick of the AICA's Timer A, 44100/44 Hz
(about 1002.27 per second) whatever the header says, so events are placed on
that clock and the header says 44100/44.

--loop: SM64's songs jump back at the end of each pass (seq_jump), so the
sequence player's first jump ends the intro plus one pass and the second ends
the next pass: the pass is their distance, the loop tick lies one pass before
the first jump, and the flow ends at the first jump with LOOP. Voices still
sounding there are keyed off, as the game's restart of its channels cuts
them. Record at least two passes.

Writes <out>.job (afx_sm64's input), <out>.json (what tools/aica/sm64_afx_verify.py
needs) and, through afx_sm64 (tools/aica/afx_sm64.c), <out>.afb/.afx/.afc.
"""
import argparse
import json
import math
import struct
import subprocess
import sys
from pathlib import Path

REC = struct.Struct("<IBBBBII")
START, UPDATE, STOP, SEQ_JUMP, END = 1, 2, 3, 4, 5
KEY_SYNTH, KEY_STREAM = 0x80000000, 0x40000000
FIELD_PITCH, FIELD_DIRECT, FIELD_MIX = 6, 9, 10
OP_NOTE, OP_PATCH, OP_KEYOFF = 0x10, 0x11, 0x12
BUDGET_COMMANDS, BUDGET_WRITES = 38, 171
AFX_RATE_NUM, AFX_RATE_DEN = 44100, 44          # AICA Timer A: 44 counts of 44.1 kHz
AFX_RATE = AFX_RATE_NUM / AFX_RATE_DEN
# KOS's linear-to-logarithmic volume table (kernel/arch/dreamcast/sound/arm/aica.c).
LOGS = [255] + [round(16.0 * math.log2(255.0 / i)) for i in range(1, 256)]


def aica_pitch(freq):
    """KOS aica_play: octave and 10-bit mantissa of freq against 44100 Hz."""
    base, hi = 5644800, 7
    while freq < base and hi > -8:
        base >>= 1
        hi -= 1
    lo = (freq << 10) // base if base else 0
    return ((hi & 15) << 11) | (lo & 1023)


def aica_pan(pan):
    if pan == 0x80:
        return 0
    return (0x10 | ((0x7F - pan) >> 3)) if pan < 0x80 else ((pan - 0x80) >> 3)


def words(freq, vol, pan):
    return {FIELD_PITCH: aica_pitch(freq), FIELD_DIRECT: 0x0F00 | aica_pan(pan),
            FIELD_MIX: (LOGS[min(vol, 255)] << 8) | 0x24}


def zone_key(key):
    if key & KEY_SYNTH:
        return f"synth:{key & 0xFF}:{(key >> 24) & 0xF}"
    return f"sample:{key & 0x3FFFFFFF:#x}"


def read_table(path):
    import re
    rows = {}
    for m in re.finditer(r"\{\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}",
                         Path(path).read_text()):
        key, pool, byte_len, nsamples, loop_start, loop_end, loop_flag, rate_scale, stream, fmt = \
            (int(m.group(i), 0) for i in range(1, 11))
        rows[key] = dict(pool=pool, bytes=byte_len, frames=nsamples, loop_start=loop_start,
                         loop_end=loop_end, loop=loop_flag, rate_scale=rate_scale, stream=stream, fmt=fmt)
    return rows


def read_log(path):
    data = Path(path).read_bytes()
    magic, upf, wave_count, band_count, wave_samples = struct.unpack_from("<5I", data)
    assert magic == 0x43455241, "not a DC_AUDIO_RECORD log"
    at = 32
    waves = []
    for _ in range(wave_count):
        waves.append(list(struct.unpack_from(f"<{wave_samples}h", data, at)))
        at += wave_samples * 2
    records = [REC.unpack_from(data, i) for i in range(at, len(data) - REC.size + 1, REC.size)]
    return upf, waves, records


def sub_rate_of(upf, records, override=None):
    """Sub-updates per second: --sub-rate, else the END record's measurement, else nominal."""
    if override:
        return override, "given"
    end = next((r for r in records if r[1] == END and r[5]), None)
    if end:
        return end[6] * 1e6 / end[5], "measured"
    return upf * 60.0, "nominal (no measurement in the log)"


def to_ticks(records, sub_rate, origin=None):
    """Sub-update stamps onto the AICAflow clock, relative to the first voice command (or to an
    earlier origin: a loop point before the first note)."""
    first = next(r[0] for r in records if r[1] in (START, UPDATE))
    if origin is not None:
        first = min(first, origin)
    scale = AFX_RATE / sub_rate
    return [(round((r[0] - first) * scale),) + r[1:] for r in records], first, scale


def loop_bounds(records, player, check=True):
    """(loop_tick, end_tick) on the mapped clock, from the player's first two jumps to the same
    target: the loop's jump back. Other jumps (an intro's branch, a variation's) go elsewhere."""
    by_target = {}
    for r in records:
        if r[1] == SEQ_JUMP and r[2] == player:
            by_target.setdefault(r[6], []).append(r[0])
    loops = [ts for ts in by_target.values() if len(ts) >= 2]
    if not loops:
        sys.exit(f"no target of player {player} was jumped to twice; record two passes to loop")
    jumps = min(loops, key=lambda ts: ts[1])
    length, end = jumps[1] - jumps[0], jumps[0]
    # A loop point at the time origin can come out a tick before it after rounding onto the clock.
    if check and length > end + 2:
        sys.exit("the pass is longer than the recording before the first jump")
    return max(end - length, 0), end, jumps


def band_buffer(wave, band):
    """The driver's per-band wave buffer: one cycle decimated by 2^band, repeated to 64."""
    count = 64 >> band
    cycle = [wave[i] for i in range(0, 64, 64 // count)]
    return (cycle * (64 // count))[:64]


def build_voices(records):
    """One voice per occupancy of a game channel: its NOTE words, then each change of words."""
    voices, open_ = [], {}
    for t, cmd, ch, vol, pan, freq, key in records:
        if cmd == START:
            if ch in open_:
                open_.pop(ch)["stop"] = t
            v = dict(id=len(voices), ch=ch, key=key, start=t, stop=None, words=words(freq, vol, pan))
            v["events"] = [(t, "note", dict(v["words"]))]
            voices.append(v)
            open_[ch] = v
        elif cmd == UPDATE and ch in open_:
            v = open_[ch]
            w = words(freq, vol, pan)
            changed = {f: x for f, x in w.items() if v["words"].get(f) != x}
            if changed:
                v["events"].append((t, "patch", changed))
                v["words"] = w
        elif cmd == STOP and ch in open_:
            open_.pop(ch)["stop"] = t
    return voices


def sounding(voices, t):
    return [v for v in voices if v["start"] < t and (v["stop"] is None or v["stop"] > t)]


def place_cut(voices, lt, b0, b1):
    """Where a looping flow ends and what crosses its LOOP.

    The game does not cut voices at its jump: the ones sounding there release
    on their own, into the next pass. So the flow runs past the jump by D
    ticks, into the game's recorded second pass (which carries those tails),
    and loops to lt + D, where the first pass is in the same place. D starts
    after the last tail (of the first pass at b0, of the intro at lt) and is
    chosen where the fewest voices cross the cut. A voice crossing the cut
    (b0 + D) is matched to the first-pass voice playing the same note across
    lt + D: they share a channel, so after the LOOP that note's remaining
    changes and release land on the voice that is sounding."""
    def last_tail(t):
        return max([v["stop"] - t for v in sounding(voices, t) if v["stop"] is not None] + [0])
    d_min = max(last_tail(b0), last_tail(lt)) + 1
    best = None
    for d in range(d_min, min(d_min + 4000, b1 - b0)):
        x, y = sounding(voices, b0 + d), sounding(voices, lt + d)
        if best is None or len(x) < best[1]:
            best = (d, len(x))
            if not x:
                break
    d = best[0]
    cut_x, cut_y = sounding(voices, b0 + d), sounding(voices, lt + d)
    links, free = {}, list(cut_y)
    for vx in sorted(cut_x, key=lambda v: v["start"]):
        match = None
        for vy in free:
            if (vy["key"] == vx["key"] and vy["events"][0][2][FIELD_PITCH] == vx["events"][0][2][FIELD_PITCH] and
                    abs((vx["start"] - b0) - (vy["start"] - lt)) <= 12):
                match = vy
                break
        if match is None:
            print(f"warning: voice {vx['id']} crossing the cut has no first-pass match; released at the loop")
            continue
        free.remove(match)
        links[vx["id"]] = match["id"]
    return lt + d, b0 + d, links


def allocate(voices, end, links):
    """Local channels: voices that overlap get different ones, and each pair crossing the
    loop (links: second-pass voice -> first-pass voice) gets one channel for both."""
    import bisect
    span = {v["id"]: (v["start"], min(v["stop"] if v["stop"] is not None else end, end)) for v in voices
            if v["start"] < end}
    busy = []                    # per channel: sorted [(start, stop)]

    def free(ch, intervals):
        for a, b in intervals:
            lst = busy[ch]
            i = bisect.bisect_left(lst, (a, -1))
            if i < len(lst) and lst[i][0] < b + 1:
                return False
            if i > 0 and lst[i - 1][1] + 1 > a:
                return False
        return True

    def place(ids):
        intervals = [span[i] for i in ids]
        ch = 0
        while True:
            if ch == len(busy):
                busy.append([])
            if free(ch, intervals):
                for iv in intervals:
                    bisect.insort(busy[ch], iv)
                return ch
            ch += 1

    channel_of = {}
    for vx, vy in links.items():
        channel_of[vx] = channel_of[vy] = place([vx, vy])
    for v in sorted(voices, key=lambda v: v["start"]):
        if v["id"] in span and v["id"] not in channel_of:
            channel_of[v["id"]] = place([v["id"]])
    if len(busy) > 64:
        sys.exit(f"{len(busy)} channels needed")
    return channel_of


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--table", required=True)
    ap.add_argument("--pool", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--loop", action="store_true", help="loop between the sequence player's first two jumps")
    ap.add_argument("--player", type=int, default=0)
    ap.add_argument("--sub-rate", type=float, help="sub-updates per second (default: the log's measurement)")
    ap.add_argument("--tail", type=int, default=0, help="ticks appended after the last event of a one-shot")
    ap.add_argument("--lanes", action="store_true", help="add a lane map, so the game can pan the flow (sounds)")
    ap.add_argument("--afx-sm64", default=str(Path(__file__).resolve().parent.parent.parent / "build" / "afx_sm64"))
    args = ap.parse_args()
    upf, waves, raw = read_log(args.log)
    table = read_table(args.table)
    pool = Path(args.pool).read_bytes()
    sub_rate, how = sub_rate_of(upf, raw, args.sub_rate)
    origin = None
    if args.loop:
        # A song whose intro is shorter than its first note's delay loops to before that note.
        lt, _, _ = loop_bounds(raw, args.player, check=False)
        origin = lt
    records, first, scale = to_ticks(raw, sub_rate, origin)
    print(f"{sub_rate:.3f} sub-updates/s ({how}); AICAflow clock {AFX_RATE:.3f} ticks/s")

    zones, zone_of = [], {}

    def zone_index(key):
        zk = zone_key(key)
        if zk not in zone_of:
            if key & KEY_SYNTH:
                pcm = band_buffer(waves[key & 0xFF], (key >> 24) & 0xF)
                zones.append(dict(key=zk, fmt=0, loop=1, loop_start=0, loop_end=63, frames=64,
                                  data=struct.pack("<64h", *pcm)))
            else:
                row = table.get(key & 0x3FFFFFFF)
                if row is None:
                    sys.exit(f"sample key {key & 0x3FFFFFFF:#x} is not in the table")
                frames, nbytes = row["frames"], row["bytes"]
                if key & KEY_STREAM or frames > 65534:
                    print(f"warning: sample {zk} has {frames} frames; truncated to 65534")
                    frames = 65534
                    nbytes = min(nbytes, {0: frames * 2, 1: frames, 2: frames // 2}[row["fmt"]])
                loop_end = row["loop_end"] if row["loop"] else frames - 1
                loop_start = row["loop_start"]
                if loop_end >= frames:   # a loop past a truncated sample's end: loop on its last frame
                    print(f"warning: sample {zk} loops to frame {loop_end}, past its {frames} frames; clamped")
                    loop_end = frames - 1
                    loop_start = min(loop_start, loop_end)
                zones.append(dict(key=zk, fmt=row["fmt"], loop=row["loop"], loop_start=loop_start,
                                  loop_end=loop_end, frames=frames,
                                  data=pool[row["pool"]:row["pool"] + nbytes]))
            zone_of[zk] = len(zones) - 1
        return zone_of[zk]

    voices = build_voices(records)
    last_tick = max((v["stop"] if v["stop"] is not None else v["events"][-1][0]) for v in voices)
    loop_tick = end = None
    links = {}
    if args.loop:
        lt, b0, jumps = loop_bounds(records, args.player)
        b1 = b0 + (jumps[1] - jumps[0])
        loop_tick, end, links = place_cut(voices, lt, b0, b1)
        print(f"intro {lt} ticks ({lt / AFX_RATE:.2f} s), pass {b0 - lt} ticks ({(b0 - lt) / AFX_RATE:.2f} s); "
              f"cut {end - b0} ticks into the second pass, after the first pass's tails: "
              f"loop {end} -> {loop_tick}, {len(links)} voices carried across")
    else:
        end = last_tick + args.tail
    channel_of = allocate(voices, end, links)
    events = []
    carried = set(links) | set(links.values())
    for v in voices:
        if v["start"] >= end:
            continue
        ch = channel_of[v["id"]]
        for t, kind, w in v["events"]:
            if t >= end:
                break
            if kind == "note":
                events.append((t, 1, OP_NOTE, ch, zone_index(v["key"]), w))
            else:
                events.append((t, 2, OP_PATCH, ch, 0, w))
        if v["stop"] is not None and v["stop"] < end:
            events.append((v["stop"], 0, OP_KEYOFF, ch, 0, {}))
        elif v["id"] not in carried:
            # Sounding at the end with nothing to carry it across the loop: release it there.
            events.append((end, 0, OP_KEYOFF, ch, 0, {}))
    # One tick's burst must fit the ARM7 budget: push the surplus NOTEs/PATCHes a tick later.
    events.sort(key=lambda e: (e[0], e[1]))
    by_tick = {}
    for e in events:
        by_tick.setdefault(e[0], []).append(e)
    spread, carry, delayed = [], [], 0
    for t in range(0, end + 1):
        todo = carry + by_tick.get(t, [])
        carry = []
        commands = writes = 0
        for e in todo:
            cost = 19 if e[2] == OP_NOTE else 1 if e[2] == OP_KEYOFF else len(e[5])
            if (commands + 1 > BUDGET_COMMANDS or writes + cost > BUDGET_WRITES) and e[2] != OP_KEYOFF:
                carry.append(e)
                continue
            commands += 1
            writes += cost
            delayed += t != e[0]
            spread.append((t,) + e[1:])
        if t >= end and not carry:
            break
    if carry:
        sys.exit("could not fit the events into the budget")
    channels = max(e[3] for e in spread) + 1
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out.with_suffix(".job"), "wb") as f:
        f.write(struct.pack("<8I", 0x424F4A53, len(zones), len(spread), end,
                            loop_tick if loop_tick is not None else 0xFFFFFFFF, AFX_RATE_NUM, channels, AFX_RATE_DEN))
        for z in zones:
            f.write(struct.pack("<6I", z["fmt"], z["loop"], z["loop_start"], z["loop_end"], z["frames"], len(z["data"])))
            f.write(z["data"])
            f.write(b"\0" * (-len(z["data"]) % 4))
        for i, (t, order, op, ch, setup, w) in enumerate(spread):
            mask = sum(1 << fld for fld in w)
            fields = [w.get(fld, 0) for fld in range(18)]
            f.write(struct.pack("<IIBBHI18H", t, i, op, ch, setup, mask, *fields))
    out.with_suffix(".json").write_text(json.dumps(dict(
        log=str(Path(args.log).resolve()), player=args.player, sub_rate=sub_rate, sub_rate_source=how,
        first_sub=first, afx_rate=[AFX_RATE_NUM, AFX_RATE_DEN], loop_tick=loop_tick, end=end,
        zones=[z["key"] for z in zones], events=len(spread), delayed_events=delayed), indent=1))
    print(f"{len(zones)} setups, {len(spread)} events ({delayed} moved later to fit a tick's budget) on "
          f"{channels} channels, {end} ticks, {sum(len(z['data']) for z in zones)} sample bytes -> {out.with_suffix('.job')}")
    tool = Path(args.afx_sm64)
    if tool.exists():
        subprocess.run([str(tool), str(out.with_suffix(".job")), str(out.with_suffix(".afb")), str(out.with_suffix(".afx"))]
                       + (["--lanes"] if args.lanes else []), check=True)
    else:
        print(f"afx_sm64 not built at {tool}; see tools/aica/afx_sm64.c")


if __name__ == "__main__":
    main()
