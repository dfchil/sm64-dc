#!/usr/bin/env python3
"""Turn a recorder log (tools/aica/recorder) into AICAflow assets.

    tools/aica/sm64_afx.py build/audio_rec/logs/rec_seq05.bin \\
        --table build/us_dc/sound/aica_sample_table.c \\
        --pool build/us_dc/sound/aica_pool.bin \\
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

--continuous: a sound the game requests every frame was recorded for as long as the requests
went on, and plays until they stop. The flow loops over a stretch of its recording that ends as
it began, so the sound holds rather than starting again.

--lane-masks: sequence channel masks, for a song whose channels the game fades apart: lane n + 1
holds the channels of mask n, lane 0 the rest. A lane's voices cannot share local channels with
another's, so a song gets no more lanes than its fades need (SONG_LANES, sm64_afx_batch.py).

Reverb: a song's voices carry the note's reverb send (logs with 20-byte records) as DSP_SEND,
IMXL by level on MIXS0, which the scene's reverb program reads (src/pc/audio/sm64_aicaflow.c).
--full-send gives every voice IMXL 15 instead, for sounds: the game scales it per lane at run
time, as their reverb depends on the area and the sound's distance.

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
REC20 = struct.Struct("<IBBBBIIBB2x")   # with the note's reverb send and sequence channel
START, UPDATE, STOP, SEQ_JUMP, END = 1, 2, 3, 4, 5
KEY_SYNTH, KEY_STREAM = 0x80000000, 0x40000000
FIELD_ENV_AD, FIELD_ENV_DR, FIELD_PITCH, FIELD_DSP_SEND, FIELD_DIRECT, FIELD_MIX = 4, 5, 6, 8, 9, 10
# The AICA amplitude EG's decay (D1R/D2R/RR) time over its whole range, 1023 steps of 3/32 dB, for
# R = 0..31 with KRS = 0xF (rate 2R), measured on a console through the EG monitor (2026-10-27):
# ~7% slower than MAME's DRTimes, halving every 2 R. R 0 holds. Reaching the end stops the voice.
EG_FULL_MS = [None, 124006, 95165, 62955, 47581, 31722, 23654, 15861, 11861, 7900, 5948, 3953, 2970, 1978,
              1484, 990.4, 743.7, 493.7, 373.6, 246.8, 185.0, 123.2, 92.57, 61.82, 46.04, 30.77, 23.02, 15.52,
              11.51, 7.76, 5.88, 5.88]
# TL steps (0.375 dB, 4 EG steps) per AICAflow tick.
EG_SLOPE = [0.0] + [1023.0 / 4.0 / ms * 1000.0 / (44100 / 44) for ms in EG_FULL_MS[1:]]
EG_ENV_DR = 0x3C1F            # KRS 0xF (no pitch scaling), DL 0, RR 31 (key-off as before)
EG_TOP = 250.0                # EG attenuation (TL steps) a sounding voice keeps below: 255.75 ends it
FULL_SEND = 0xF0
CONTINUOUS_MIN_TICKS = 3000   # a held sound's recording runs 4 s; shorter ones ended by themselves
CONTINUOUS_ATTACK_TICKS = 200
CONTINUOUS_MIN_BODY = 300
# The least N64 send (0..127, reverb / 128 of the voice) of each AICA IMXL code 1..15, which steps
# 3 dB from -42 dB to 0 dB; the same table is send_code in src/pc/audio/sm64_aicaflow.c.
SEND_LEVELS = [1, 2, 2, 3, 4, 5, 7, 10, 14, 20, 28, 39, 54, 77, 108]
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


def dsp_send(reverb):
    """The DSP_SEND word for an N64 reverb send: IMXL in the high nibble, MIXS0."""
    return sum(1 for level in SEND_LEVELS if (reverb & 0x7F) >= level) << 4


def zone_key(key):
    if key & KEY_SYNTH:
        return f"synth:{key & 0xFF}:{(key >> 24) & 0xF}"
    return f"sample:{key & 0x3FFFFFFF:#x}"


# Long samples. The AICA addresses a voice's sample with 16-bit loop points, so a sample plays at
# most SAMPLE_MAX frames; SM64 has three longer ones, which the KOS-era port streamed. A one-shot
# is cut into pieces, each its own setup, and the flow keys each piece on the tick the one before
# runs out (split_long). A looping one is resampled to fit and played at a lower step to match
# (LongSamples.fitted, freq_scale in the .json): its notes run at many pitches, and a piece handed
# over at each would need its own copy per pitch.
SAMPLE_MAX = 65533           # frames a piece plays: its LEA (the 16-bit loop end word)
SAMPLE_FRAMES_MAX = 65534    # frames a setup holds (a one-shot's LEA is its frames - 1)
SPLIT_SEARCH = 6000          # frames before a piece's limit searched for a quiet place to cut
TICK_SAMPLES = 44            # AICA output samples per AICAflow tick (Timer A)


def pitch_step(word):
    """A voice's position step per output sample, in 1/4096 frames (the AICA's phase register)."""
    octave = (word >> 11) & 15
    octave = octave - 16 if octave > 7 else octave
    step = 1024 + (word & 1023)
    return step << (octave + 2) if octave + 2 >= 0 else step >> -(octave + 2)


class LongSamples:
    """The source PCM of samples longer than SAMPLE_MAX, and their re-encoded pieces."""

    def __init__(self, table, pool, table_path):
        self.table, self.pool = table, pool
        here = Path(table_path).parent
        self.ctl, self.tbl = here / "sound_data.ctl", here / "sound_data.tbl"
        self.parsed = None
        self.cache = {}

    def frames(self, key):
        if key & KEY_SYNTH:
            return None
        row = self.table.get(key & 0x3FFFFFFF)
        return row["frames"] if row and row["frames"] > SAMPLE_FRAMES_MAX else None

    def looped(self, key):
        return bool(self.table[key & 0x3FFFFFFF]["loop"])

    def pcm(self, key):
        """The sample as the N64 has it (VADPCM decoded), else the pool's ADPCM decoded."""
        addr = key & 0x3FFFFFFF
        if addr in self.cache:
            return self.cache[addr]
        pcm = None
        if self.ctl.exists() and self.tbl.exists():
            import vadpcm
            from sm64_albank_parse import parse_all
            if self.parsed is None:
                self.parsed = parse_all(str(self.ctl), str(self.tbl), with_data=True)[1]
            s = self.parsed.get(addr)
            if s is not None:
                pcm = vadpcm.decode(s.data, s.codec, s.order, s.npredictors, s.book)
        if pcm is None:
            import yamaha_adpcm
            row = self.table[addr]
            pcm = yamaha_adpcm.decode(self.pool[row["pool"]:row["pool"] + row["bytes"]], row["frames"])
        self.cache[addr] = list(pcm)
        return self.cache[addr]

    def piece(self, key, f0, f1, last):
        """Frames f0..f1 of a one-shot as a zone that stops at f1 (LEA = f1 - f0), or plays out. PCM
        and the first ADPCM piece are the pool's bytes; a later ADPCM piece is encoded again, as
        the AICA starts its decoder from silence at a key-on."""
        row = self.table[key & 0x3FFFFFFF]
        n = f1 - f0
        frames = min(n + 1, row["frames"] - f0)          # the frame at LEA, for the interpolation
        fmt, at = row["fmt"], row["pool"]
        if fmt in (0, 1):
            size = 2 if fmt == 0 else 1
            data = self.pool[at + f0 * size:at + (f0 + frames) * size]
        elif f0 == 0:
            data = self.pool[at:at + (frames + 1) // 2]
        else:
            from transcode import beam_encode_cached
            data, _ = beam_encode_cached(self.pcm(key)[f0:f0 + frames])
        return dict(key=f"{zone_key(key)}@{f0}-{f1}", fmt=fmt, loop=0, loop_start=0,
                    loop_end=frames - 1 if last else n, frames=frames, data=data)

    def fitted(self, key):
        """A looping sample resampled so its loop ends by SAMPLE_MAX: (zone, frequency scale). The
        loop points land on whole frames (ratio = new loop length / old), so the loop stays seamless."""
        import numpy as np
        from transcode import beam_encode_cached
        row = self.table[key & 0x3FFFFFFF]
        ls, le = row["loop_start"], row["loop_end"]
        length = le - ls
        new_length = length * SAMPLE_MAX // le
        ratio = new_length / length
        new_ls = round(ls * ratio)
        while new_ls + new_length > SAMPLE_MAX:
            new_length -= 1
            ratio = new_length / length
            new_ls = round(ls * ratio)
        src = np.array(self.pcm(key)[:le], dtype=np.float64)
        n_out = new_ls + new_length + 1
        x = ls + (np.arange(n_out) - new_ls) / ratio      # each output frame's place in the source
        half = 24
        taps = np.arange(-half + 1, half + 1)
        base = np.floor(x).astype(np.int64)
        idx = base[:, None] + taps[None, :]
        frac = x[:, None] - idx
        # past the loop's end the source goes on at its start, as the AICA plays it
        wrapped = np.where(idx >= le, ls + (idx - ls) % length, idx)
        vals = np.where(wrapped >= 0, src[np.clip(wrapped, 0, le - 1)], 0.0)
        kernel = ratio * np.sinc(ratio * frac) * np.kaiser(2 * half, 8.0)[None, :]
        out = np.clip(np.round((vals * kernel).sum(axis=1)), -32768, 32767).astype(int).tolist()
        data, _ = beam_encode_cached(out)
        zone = dict(key=zone_key(key), fmt=2, loop=1, loop_start=new_ls, loop_end=new_ls + new_length,
                    frames=n_out, data=data)
        return zone, ratio      # fewer frames a cycle: the same pitch is a slower step


def split_long(voices, samples, flow_end):
    """Each voice on a one-shot longer than SAMPLE_MAX as a chain of voices, one per piece: a piece
    stops (its LEA) on the tick the next is keyed on, at the frame the AICA has reached by then
    (TICK_SAMPLES output samples a tick at the voice's pitch step), so the pieces join without a
    gap. A cut goes where the sample is quietest in the last SPLIT_SEARCH frames of a piece's
    room: a piece starts its ADPCM decoder from silence. Returns (voices, cuts made)."""
    out, cuts = [], 0
    for v in voices:
        total = samples.frames(v["key"])
        if total is None or samples.looped(v["key"]):
            out.append(v)
            continue
        pcm = samples.pcm(v["key"])
        stop = v["stop"] if v["stop"] is not None else flow_end
        pieces, f0, t0, pos, t = [], 0, v["start"], 0, v["start"]
        candidates = []
        while t < stop and (pos >> 12) < total:
            pos += TICK_SAMPLES * pitch_step(word_at(v, t, FIELD_PITCH))
            t += 1
            f = pos >> 12
            if f - f0 > SAMPLE_MAX:
                if not candidates:
                    sys.exit(f"voice {v['id']}: no tick to cut its sample at")
                cut_t, cut_f = min(candidates, key=lambda c: (max(abs(x) for x in pcm[max(c[1] - 32, 0):c[1] + 32]), -c[1]))
                pieces.append((t0, f0, cut_t, cut_f))
                t0, f0, candidates = cut_t, cut_f, []
                t, pos = cut_t, cut_f << 12
                continue
            if f - f0 >= SAMPLE_MAX - SPLIT_SEARCH and f < total:
                candidates.append((t, f))
        if not pieces:
            out.append(v)
            continue
        pieces.append((t0, f0, None, min(total, f0 + SAMPLE_MAX)))
        cuts += len(pieces) - 1
        note = dict(v["events"][0][2])
        for i, (start, a, until, b) in enumerate(pieces):
            if i:
                for when, kind, w in v["events"]:
                    if when < start:
                        note.update(w)
            piece = dict(v, start=start, stop=until if until is not None else v["stop"],
                         piece=(a, b, until is None))
            body = [e for e in v["events"][1:] if start <= e[0] and (until is None or e[0] < until)]
            piece["events"] = [(v["start"] if i == 0 else start, "note", dict(note) if i else v["events"][0][2])] + body
            out.append(piece)
    for i, v in enumerate(out):
        v["id"] = i
    return out, cuts


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
    assert magic == 0x43455241, "not a recorder log"
    at = 32
    waves = []
    for _ in range(wave_count):
        waves.append(list(struct.unpack_from(f"<{wave_samples}h", data, at)))
        at += wave_samples * 2
    # Header word 5: the record size, 0 in logs from before the reverb send (16 bytes, send 0).
    if struct.unpack_from("<I", data, 20)[0] == REC20.size:
        records = [REC20.unpack_from(data, i) for i in range(at, len(data) - REC20.size + 1, REC20.size)]
    else:
        records = [REC.unpack_from(data, i) + (0, 0xFF) for i in range(at, len(data) - REC.size + 1, REC.size)]
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


def build_voices(records, full_send=False, freq_scale=None):
    """One voice per occupancy of a game channel: its NOTE words, then each change of words.
    A NOTE leaves out a send of 0, its setup's. freq_scale: {sample address: factor} for the
    samples LongSamples.fitted resampled."""
    def voice_words(freq, vol, pan, reverb, key=0):
        if freq_scale and not key & KEY_SYNTH and (key & 0x3FFFFFFF) in freq_scale:
            freq = round(freq * freq_scale[key & 0x3FFFFFFF])
        w = words(freq, vol, pan)
        w[FIELD_DSP_SEND] = FULL_SEND if full_send else dsp_send(reverb)
        return w
    voices, open_, last_channel = [], {}, {}
    for t, cmd, ch, vol, pan, freq, key, reverb, seq_channel in records:
        if cmd == START:
            # A note starting after its layer has gone logs no sequence channel: the voice's last.
            seq_channel = last_channel.get(ch, 0) if seq_channel == 0xFF else seq_channel
            last_channel[ch] = seq_channel
            if ch in open_:
                open_.pop(ch)["stop"] = t
            v = dict(id=len(voices), ch=ch, key=key, seq_channel=seq_channel, start=t, stop=None,
                     words=voice_words(freq, vol, pan, reverb, key))
            v["events"] = [(t, "note", {f: x for f, x in v["words"].items() if f != FIELD_DSP_SEND or x})]
            voices.append(v)
            open_[ch] = v
        elif cmd == UPDATE and ch in open_:
            v = open_[ch]
            w = voice_words(freq, vol, pan, reverb, v["key"])
            changed = {f: x for f, x in w.items() if v["words"].get(f) != x}
            if changed:
                v["events"].append((t, "patch", changed))
                v["words"] = w
        elif cmd == STOP and ch in open_:
            open_.pop(ch)["stop"] = t
    return voices


def twin_signature(v):
    """What two voices share to be twins (merge_twins): the sample, the start and stop, the
    sequence channel and every word change."""
    return (v["key"], v["start"], v["stop"], v["seq_channel"], repr(v["events"]))


def twin_boost(v, twins):
    """TL steps (0.375 dB) a voice goes up for its twins: n + 1 equal voices in phase are
    20 log10(n + 1) dB, 16 steps for a pair; but no further than its loudest word allows (TL 0),
    the same steps for every word so that its fades keep their shape. Returns (steps, short)."""
    want = round(16 * math.log2(twins + 1))
    loudest = min([w[FIELD_MIX] >> 8 for _, _, w in v["events"] if FIELD_MIX in w and w[FIELD_MIX] >> 8 != MUTE_TL]
                  or [0])
    steps = min(want, loudest)
    return steps, want - steps


def merge_twins(voices):
    """Some sounds start the same sample twice on the same sub-update, with the same words and the
    same changes after (two layers of one note). The N64 adds them in phase: one voice 6 dB up. On
    the AICA the firmware keys the second about 24 samples after the first (a comb, dips near
    800 Hz), and a host backend keys both on one sample, often past full scale. Each such group is
    one voice here, louder by twin_boost; its reverb send (IMXL, 3 dB steps) by as much, up to 15.
    Returns the voices, renumbered, and the most TL steps a voice stays under its group."""
    groups, order = {}, []
    for v in voices:
        sig = twin_signature(v)
        if sig not in groups:
            groups[sig] = []
            order.append(sig)
        groups[sig].append(v)
    out, short_most = [], 0
    for sig in order:
        group = groups[sig]
        v = group[0]
        if len(group) > 1:
            steps, short = twin_boost(v, len(group) - 1)
            short_most = max(short_most, short)
            for w in [e[2] for e in v["events"]] + [v["words"]]:
                if FIELD_MIX in w and (w[FIELD_MIX] >> 8) != MUTE_TL:
                    w[FIELD_MIX] -= steps << 8
                if w.get(FIELD_DSP_SEND):
                    imxl = min(((w[FIELD_DSP_SEND] >> 4) & 15) + round(steps * 0.375 / 3), 15)
                    w[FIELD_DSP_SEND] = (w[FIELD_DSP_SEND] & ~0xF0) | (imxl << 4)
            v["twins"] = len(group) - 1
        out.append(v)
    for i, v in enumerate(out):
        v["id"] = i
    return out, short_most


def eg_ad(r):
    """ENV_AD for decay rate R (D1R = D2R, so DL does not matter) and an instant attack."""
    return (r << 11) | (r << 6) | 31


MUTE_TL = 255


def fit_eg(v, tol):
    """Let the AICA's EG carry a voice's level fall. The recording's TL steps are targets; the
    voice's total level is its TL register plus the EG's attenuation, which falls at one of the
    hardware's rates from key-on. A target holds until the next one, so the total is checked
    against it in the middle of its hold (at its change, the EG would always run ahead of the
    stairs and the voice end up quieter). Greedily, at each point where the total would leave
    `tol` TL steps of the target, choose a rate (an ENV_AD write) and/or a TL correction that keeps it
    within for the most target changes ahead. The EG never goes past the lowest target still to
    come (TL cannot go below 0) nor near its end (that stops the voice). Rewrites the voice's
    events: the NOTE gets ENV_AD/ENV_DR, PATCHes keep their other fields, their MIX only where a
    correction is due. Returns the number of TL writes saved."""
    ev = v["events"]
    idx = [k for k, e in enumerate(ev) if FIELD_MIX in e[2] or k == 0]
    t = [ev[k][0] for k in idx]
    T = [float(ev[k][2][FIELD_MIX] >> 8) for k in idx]
    n = len(t)
    if n < 3:
        return 0
    tm = [(t[j] + t[j + 1]) / 2 for j in range(n - 1)] + [t[-1]]   # the middle of each hold
    sufmin = T[:]
    for j in range(n - 2, -1, -1):
        sufmin[j] = min(T[j], sufmin[j + 1])

    def run(i, reg, e0, r):
        """Target changes after i kept within tol with TL `reg` and rate r from level e0 at t[i],
        or -1 if target i itself is not. The change it stops at must still be reachable (EG at
        or under every target from there on), so the next decision can correct: shortened until
        it is (rate 0 always is)."""
        s, L = EG_SLOPE[r], -1
        for j in range(i, n):
            if j > i and T[j] >= MUTE_TL:
                break
            e = e0 + s * (tm[j] - t[i])
            if e > min(sufmin[j], EG_TOP) or abs(reg + e - T[j]) > tol:
                break
            L += 1
        while L >= 0 and i + L + 1 < n and e0 + s * (t[i + L + 1] - t[i]) > min(sufmin[i + L + 1], EG_TOP):
            L -= 1
        return L

    reg, e, r = T[0], 0.0, 0
    plan = {0: (reg, 0)}            # target index -> (TL written or None, rate written or None)
    i = 0
    while True:
        if i and T[i] >= MUTE_TL:
            # A mute is written as it is: TL 255 plus the EG does not silence the voice on the
            # hardware (a sound ended at TL 173 + EG played on at -65 dB). The EG stops there, so
            # a target after it is still in reach.
            plan[i] = (MUTE_TL, 0 if r else None)
            reg, r = MUTE_TL, 0
            if i + 1 >= n:
                break
            i += 1
            continue
        best = None
        for correct in (False, True) if i else (True,):
            for rr in range(32):
                reg_i = round(T[i] - e - EG_SLOPE[rr] * (tm[i] - t[i])) if correct else reg
                if reg_i < 0 or reg_i > 255:
                    continue
                L = run(i, reg_i, e, rr)
                if L < 0:
                    continue
                # stream bytes: a TL-only correction is a PATCH_LEVEL (4), a rate change needs a
                # PATCH (6 + 2 per word), and an event tick of its own a WAIT (about 2)
                tl_w, rate_w = correct and i > 0, rr != r
                cost = 0 if i == 0 else (4 if not rate_w else 6 + 2 * (tl_w + rate_w)) + 2
                key = ((L + 1) / max(cost, 1), L, -cost)
                if best is None or key > best[0]:
                    best = (key, correct, reg_i, rr)
        _, correct, reg_new, r_new = best
        if i == 0:
            plan[0] = (round(reg_new), r_new)
        elif correct or r_new != r:
            plan[i] = (round(reg_new) if correct else None, r_new if r_new != r else None)
        if correct:
            reg = round(reg_new)
        L = best[0][1]
        nxt = i + L + 1
        if nxt >= n:
            break
        e = e + EG_SLOPE[r_new] * (t[nxt] - t[i])
        r = r_new
        i = nxt
    before = n - 1
    # rewrite the events
    uses_eg = any(rate for _, rate in plan.values())
    for j, k in enumerate(idx):
        tk, kind, w = ev[k]
        w = dict(w)
        tl, rate = plan.get(j, (None, None))
        if j == 0:
            w[FIELD_MIX] = (tl << 8) | (w[FIELD_MIX] & 0xFF)
            if uses_eg:
                w[FIELD_ENV_AD] = eg_ad(rate)
                w[FIELD_ENV_DR] = EG_ENV_DR      # KRS 0xF: the rates do not follow the pitch
        else:
            del w[FIELD_MIX]
            if tl is not None:
                w[FIELD_MIX] = (tl << 8) | (ev[k][2][FIELD_MIX] & 0xFF)
            if rate is not None:
                w[FIELD_ENV_AD] = eg_ad(rate)
        ev[k] = (tk, kind, w)
    v["events"] = [e for e in ev if e[1] == "note" or e[2]]
    after = sum(1 for e in v["events"][1:] if FIELD_MIX in e[2])
    v["eg_check"] = (t, T, tol)
    return before - after


def check_eg(v):
    """Replays a fitted voice: its total level (TL + EG) at every target change, against the target."""
    t, T, tol = v["eg_check"]
    t = [(t[j] + t[j + 1]) / 2 for j in range(len(t) - 1)] + [t[-1]]   # fit_eg's check points
    reg = r = 0
    e, last = 0.0, None
    worst = 0.0
    evs = v["events"]
    k = 0
    for j in range(len(t)):
        while k < len(evs) and evs[k][0] <= t[j]:
            tk, kind, w = evs[k]
            if last is not None:
                e = min(e + EG_SLOPE[r] * (tk - last), 255.75)
            last = tk
            if kind == "note":
                e = 0.0
                r = 0
            if FIELD_MIX in w:
                reg = w[FIELD_MIX] >> 8
            if FIELD_ENV_AD in w:
                r = (w[FIELD_ENV_AD] >> 6) & 31
            k += 1
        ej = min(e + EG_SLOPE[r] * (t[j] - last), 255.75)
        worst = max(worst, abs(min(reg + ej, MUTE_TL) - min(T[j], MUTE_TL)))   # past TL 255 is silence
    return worst


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


def word_at(v, t, field):
    """A voice's register word at tick t."""
    value = None
    for when, _, w in v["events"]:
        if when > t:
            break
        value = w.get(field, value)
    return value


def continuous_loop(voices, looped):
    """(loop tick, end tick, links) for a sound held by the game's requests, or None if it ends by
    itself. Past the first note's attack, the longest stretch whose end has the same notes
    sounding as its start, at the nearest pitch and level. A voice crosses the LOOP on a looped
    sample, or level with its partner in a one-shot (which would run out early otherwise); links
    as in place_cut."""
    last = max(v["stop"] if v["stop"] is not None else v["events"][-1][0] for v in voices)
    if last < CONTINUOUS_MIN_TICKS:
        return None
    best = None
    for end in range(last - 2, last // 2, -8):
        x = sounding(voices, end)
        for lt in range(voices[0]["start"] + CONTINUOUS_ATTACK_TICKS, end - CONTINUOUS_MIN_BODY, 8):
            y = sounding(voices, lt)
            if len(x) != len(y):
                continue
            links, off, free = {}, 0, list(y)
            for vx in sorted(x, key=lambda v: v["start"]):
                same = [v for v in free if v["key"] == vx["key"]
                        and (looped(vx) or abs((end - vx["start"]) - (lt - v["start"])) <= 12)]
                if not same:
                    break
                def distance(vy):
                    return (abs(word_at(vx, end, FIELD_PITCH) - word_at(vy, lt, FIELD_PITCH))
                            + abs((word_at(vx, end, FIELD_MIX) >> 8) - (word_at(vy, lt, FIELD_MIX) >> 8)))
                vy = min(same, key=distance)
                off += distance(vy)
                free.remove(vy)
                links[vx["id"]] = vy["id"]
            else:
                if best is None or (off, lt - end) < best[0]:
                    best = ((off, lt - end), lt, end, links)
    return best[1:] if best else None


def allocate(voices, end, links, masks=()):
    """Local channels, and each one's lane: voices that overlap get different ones, each pair
    crossing the loop (links: second-pass voice -> first-pass voice) gets one channel for both, and
    masks (--lane-masks) keeps each lane's voices on channels of their own."""
    import bisect
    span = {v["id"]: (v["start"], min(v["stop"] if v["stop"] is not None else end, end)) for v in voices
            if v["start"] < end}
    busy = []                    # per channel: sorted [(start, stop)]
    lanes = []                   # per channel: its lane
    def voice_lane(v):
        return next((n + 1 for n, mask in enumerate(masks) if mask >> v["seq_channel"] & 1), 0)
    lane_of = {v["id"]: voice_lane(v) for v in voices}

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
                lanes.append(lane_of[ids[0]])
            if lanes[ch] == lane_of[ids[0]] and free(ch, intervals):
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
    return channel_of, lanes


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--table", required=True)
    ap.add_argument("--pool", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--loop", action="store_true", help="loop between the sequence player's first two jumps")
    ap.add_argument("--lane-masks", help="comma-separated sequence channel masks, one lane each (songs)")
    ap.add_argument("--continuous", action="store_true", help="loop a sound the game keeps requesting")
    ap.add_argument("--player", type=int, default=0)
    ap.add_argument("--sub-rate", type=float, help="sub-updates per second (default: the log's measurement)")
    ap.add_argument("--tail", type=int, default=0, help="ticks appended after the last event of a one-shot")
    ap.add_argument("--lanes", action="store_true", help="add a lane map, so the game can pan the flow (sounds)")
    ap.add_argument("--hw-env", type=float, metavar="TOL",
                    help="let the AICA's EG carry level falls, within TOL TL steps (0.375 dB) of the recording")
    ap.add_argument("--full-send", action="store_true",
                    help="every voice at full reverb send, for the game to scale per lane (sounds)")
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
    samples = LongSamples(table, pool, args.table)
    fitted, freq_scale = {}, {}
    for key in sorted({r[6] for r in records if r[1] == START}):
        if samples.frames(key) and samples.looped(key):
            zone, factor = samples.fitted(key)
            fitted[zone["key"]] = zone
            freq_scale[key & 0x3FFFFFFF] = factor
            print(f"sample {zone['key']}: {samples.frames(key)} frames resampled to {zone['frames']}, "
                  f"loop {zone['loop_start']}-{zone['loop_end']}, frequency x {factor:.5f}")

    def zone_index(v):
        key = v["key"]
        zk = zone_key(key)
        if "piece" in v:
            a, b, last = v["piece"]
            zk = f"{zk}@{a}-{b}"
            if zk not in zone_of:
                zones.append(samples.piece(key, a, b, last))
                zone_of[zk] = len(zones) - 1
            return zone_of[zk]
        if zk in fitted and zk not in zone_of:
            zones.append(fitted[zk])
            zone_of[zk] = len(zones) - 1
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
                if key & KEY_STREAM or frames > SAMPLE_FRAMES_MAX:
                    # Only a voice that never plays past SAMPLE_MAX frames gets here (split_long).
                    print(f"sample {zk} has {frames} frames; its notes play no further than {SAMPLE_FRAMES_MAX}")
                    frames = SAMPLE_FRAMES_MAX
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

    voices = build_voices(records, args.full_send, freq_scale)
    before = len(voices)
    voices, lost = merge_twins(voices)
    if len(voices) != before:
        print(f"{before - len(voices)} twin voices merged into their pair's other voice (+6 dB)"
              + (f"; one stays {lost} TL steps ({lost * 0.375:.2f} dB) under the pair" if lost else ""))
    last_tick = max((v["stop"] if v["stop"] is not None else v["events"][-1][0]) for v in voices)
    voices, cuts = split_long(voices, samples, last_tick + args.tail)
    if cuts:
        if args.loop or args.continuous:
            sys.exit("a long sample's pieces in a looping flow: not supported")
        print(f"{cuts} cuts: long samples played as chained pieces")
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
        held = None
        if args.continuous:
            held = continuous_loop(voices, lambda v: v["key"] & KEY_SYNTH or table[v["key"] & 0x3FFFFFFF]["loop"])
        if held:
            loop_tick, end, links = held
            print(f"held sound: loop {end} -> {loop_tick}, {len(links)} voices carried across")
    masks = [int(m, 0) for m in args.lane_masks.split(",")] if args.lane_masks else []
    channel_of, lanes = allocate(voices, end, links, masks)
    events = []
    carried = set(links) | set(links.values())
    if args.hw_env is not None:
        # A voice carried across the LOOP is left alone: after the jump its events assume the EG
        # level of the other pass.
        saved, worst = 0, 0.0
        for v in voices:
            if v["id"] not in carried and v["start"] < end:
                saved += fit_eg(v, args.hw_env)
                if "eg_check" in v:
                    worst = max(worst, check_eg(v))
        if worst > args.hw_env + 0.5:
            sys.exit(f"EG fit leaves a voice {worst:.2f} TL steps off its recording")
        print(f"EG: {saved} TL writes left to the hardware envelope; worst {worst:.2f} TL steps off")
    for v in voices:
        if v["start"] >= end:
            continue
        ch = channel_of[v["id"]]
        for t, kind, w in v["events"]:
            if t >= end:
                break
            if kind == "note":
                events.append((t, 1, OP_NOTE, ch, zone_index(v), w))
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
        log=str(Path(args.log).resolve()), player=args.player, full_send=args.full_send, sub_rate=sub_rate, sub_rate_source=how,
        first_sub=first, afx_rate=[AFX_RATE_NUM, AFX_RATE_DEN], loop_tick=loop_tick, end=end,
        zones=[z["key"] for z in zones], events=len(spread), delayed_events=delayed,
        freq_scale={f"{k:#x}": f for k, f in freq_scale.items()}), indent=1))
    print(f"{len(zones)} setups, {len(spread)} events ({delayed} moved later to fit a tick's budget) on "
          f"{channels} channels, {end} ticks, {sum(len(z['data']) for z in zones)} sample bytes -> {out.with_suffix('.job')}")
    tool = Path(args.afx_sm64)
    lane_args = ["--lanes"] if args.lanes else []
    if masks:
        out.with_suffix(".lanes").write_bytes(bytes(lanes[:channels]))
        lane_args = ["--lanes", str(out.with_suffix(".lanes"))]
    if tool.exists():
        subprocess.run([str(tool), str(out.with_suffix(".job")), str(out.with_suffix(".afb")), str(out.with_suffix(".afx"))]
                       + lane_args, check=True)
    else:
        print(f"afx_sm64 not built at {tool}; see tools/aica/afx_sm64.c")


if __name__ == "__main__":
    main()
