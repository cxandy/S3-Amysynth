#!/usr/bin/env python3
"""midi2amysong.py - convert a standard MIDI file (SMF) into an AMYSONG text
file for the S3-Amysynth WiFi importer.

The target is a step sequencer: layer 0 = drums (kick/snare/hat1/hat2),
up to 3 melodic layers, one 16th-note pattern of 16 (1 bar) or 32 steps
(2 bars). A conversion is therefore a LOOP: the first N bars are quantized
to the 16th grid and everything after is dropped. Chords in one channel
collapse to their top note per step.

Usage:
  python midi2amysong.py in.mid --bars 2 --patch 256 --name "My Song"
  (writes in.amysong next to the input)

Options:
  --out PATH    output file (default: input with .amysong)
  --bars 1|2    loop length in bars (16 or 32 steps)  [2]
  --patch N     AMY patch for every melodic layer     [256 built-in piano]
  --bpm N       tempo override (default: from the file's set-tempo meta) [120]
  --drumch N    channel number treated as drums       [10]
  --name TEXT   song name shown on the device         [default "SONG"]
"""

import argparse
import struct
import sys


class SmfError(Exception):
    pass


def read_vlq(data, i):
    v = 0
    for _ in range(4):
        b = data[i]
        i += 1
        v = (v << 7) | (b & 0x7F)
        if not (b & 0x80):
            return v, i
    raise SmfError("bad VLQ")


def parse_smf(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"MThd" or len(data) < 14:
        raise SmfError("not a MIDI file")
    (hlen,) = struct.unpack(">I", data[4:8])
    fmt, ntrk, div = struct.unpack(">HHH", data[8:14])
    if fmt not in (0, 1):
        raise SmfError("format %d not supported" % fmt)
    if div & 0x8000:
        raise SmfError("SMPTE time division not supported")
    tracks = []
    off = 8 + hlen
    while off < len(data):
        if data[off:off + 4] != b"MTrk":
            off += 1
            continue
        (tlen,) = struct.unpack(">I", data[off + 4:off + 8])
        tracks.append(data[off + 8:off + 8 + tlen])
        off += 8 + tlen
    return fmt, ntrk, div, tracks


def decode_track(data):
    """(tick, channel, note, on) events with absolute ticks; on=False for
    note-offs (kept only to sanity-check velocity 0). Returns (events, tempo)."""
    events = []
    i, tick, running = 0, 0, 0
    tempo = None
    while i < len(data):
        dt, i = read_vlq(data, i)
        tick += dt
        b = data[i]
        if b == 0xFF:                       # meta
            i += 1
            mt = data[i]; i += 1
            ln, i = read_vlq(data, i)
            if mt == 0x51 and ln == 3:
                tempo = struct.unpack(">I", b"\x00" + data[i:i + 3])[0]
            i += ln
            continue
        if b in (0xF0, 0xF7):               # sysex
            ln, i = read_vlq(data, i)
            i += ln
            continue
        if b == 0xF8 or b >= 0xF1:          # realtime interleave
            continue
        if b & 0x80:
            st = b
            running = b
            i += 1                          # consume the status byte
        else:
            if running == 0:
                raise SmfError("running status with no prior status")
            st = running                    # i already on first data byte
        kind = st >> 4
        ch = st & 0x0F
        nd = 1 if kind in (0xC, 0xD) else 2
        if i + nd > len(data):
            break
        body = data[i:i + nd]
        i += nd
        if kind == 0x9 and body[1] > 0:
            events.append((tick, ch, body[0], True))
        elif kind in (0x8, 0x9):
            events.append((tick, ch, body[0], False))
    return events, tempo


DRUM_TRACK = {
    # GM drum note -> drum-layer track (0 kick, 1 snare, 2 hat1, 3 perc)
    35: 0, 36: 0,        # kick
    37: 1, 38: 1, 40: 1, # snare rim
    42: 2, 44: 2, 46: 2, # closed hats / pedal
    # everything else lands on the perc track
}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("midi")
    ap.add_argument("--out", default=None)
    ap.add_argument("--bars", type=int, default=2, choices=(1, 2))
    ap.add_argument("--patch", type=int, default=256)
    ap.add_argument("--bpm", type=int, default=None)
    ap.add_argument("--drumch", type=int, default=10)
    ap.add_argument("--name", default=None)
    args = ap.parse_args()

    try:
        fmt, ntrk, div, tracks = parse_smf(args.midi)
    except SmfError as e:
        print("error: %s" % e, file=sys.stderr)
        return 1

    tempo = 500000          # default 120 bpm
    notes = []              # (tick, channel 0..15, midi note)
    for tr in tracks:
        evs, t = decode_track(tr)
        if t:
            tempo = t
        for tick, ch, note, on in evs:
            if on:
                notes.append((tick, ch, note))

    bpm = args.bpm if args.bpm else max(1, int(round(60000000.0 / tempo)))
    steps = 16 * args.bars
    span = 4 * div * args.bars           # loop span in ticks
    step_ticks = div / 4.0               # one 16th in ticks

    grid = {}                            # (step, ch) -> top note (highest)
    drum = {}                            # (step, drum_track) -> True
    for tick, ch, note in notes:
        if tick >= span:
            continue
        s = int(round(tick / step_ticks))
        if s >= steps:
            continue
        if ch % 16 == args.drumch % 16:
            drum[(s, DRUM_TRACK.get(note, 3))] = True
        else:
            key = (s, ch)
            if key not in grid or note > grid[key]:
                grid[key] = note

    mlayers = sorted({ch for (_, ch) in grid})
    if len(mlayers) > 3:
        print("warning: %d melodic channels, keeping the first 3" % len(mlayers),
              file=sys.stderr)
        mlayers = mlayers[:3]
    if not mlayers and not drum:
        print("error: no notes within the first %d bar(s)" % args.bars,
              file=sys.stderr)
        return 1

    def row_tokens(f):
        return " ".join(f(s) for s in range(steps))

    base = 60
    lines = ["amysong 1"]
    if args.name:
        lines.append('name "%s"' % args.name[:15])
    lines.append("bpm %d" % bpm)
    lines.append("pattern %d" % steps)

    if drum:
        lines.append("layer drum")
        for dr in range(4):
            toks = ["x" if (s, dr) in drum else "." for s in range(steps)]
            lines.append("hit %d %s" % (dr, " ".join(toks)))

    for ch in mlayers:
        lines.append("layer melodic %d" % args.patch)
        lines.append("base %d" % base)
        toks = []
        for s in range(steps):
            note = grid.get((s, ch))
            if note is None:
                toks.append(".")
            else:
                ofs = note - base
                toks.append("0" if ofs == 0 else ("+%d" % ofs if ofs > 0 else "%d" % ofs))
        lines.append("notes %s" % " ".join(toks))

    text = "\n".join(lines) + "\n"
    out = args.out or (args.midi[:-4] + ".amysong" if args.midi.lower().endswith(".mid")
                       else args.midi + ".amysong")
    with open(out, "w", encoding="utf-8") as f:
        f.write(text)
    print("wrote %s (%d bpm, %d steps, %d melodic layer(s)%s)" %
          (out, bpm, steps, len(mlayers), ", drums" if drum else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())