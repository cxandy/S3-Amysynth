#!/usr/bin/env python3
"""Generate tiny SMF test files for the whole-song arrange importer.

Writes:
  test_sections.mid  - Intro(2 bars, drums) / Verse(4 bars, drums+mel A) /
                       Chorus(4 bars, drums+mel B), with 0xFF 0x06 markers.
  test_nomarker.mid  - the same notes but NO section markers (firmware must
                       reject it with "need at least 2 section markers").

Pure stdlib, no dependencies.
"""

import os
import struct
import sys

DIV = 480          # ticks per quarter note
TPB = 4 * DIV      # ticks per 4/4 bar
STEP = DIV // 4    # one 16th


def vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    return bytes(reversed(out))


def meta(typ, data):
    return b"\xff" + bytes([typ]) + vlq(len(data)) + data


def track_body(events):
    events = sorted(events, key=lambda e: e[0])
    body = bytearray()
    prev = 0
    for t, d in events:
        body += vlq(t - prev) + d
        prev = t
    body += vlq(0) + meta(0x2F, b"")
    return b"MTrk" + struct.pack(">I", len(body)) + bytes(body)


def build(markers):
    t0 = [
        (0, meta(0x03, b"Section Test")),
        (0, meta(0x51, struct.pack(">I", 500000)[1:])),  # 120 bpm
        # leading XG-style sysex: regresses the "F0 status not skipped before
        # length VLQ" bug (real files like YAMAHA demo .mid start with this).
        (0, b"\xf0" + vlq(8) + bytes([0x43, 0x10, 0x4c, 0x00, 0x00, 0x7e, 0x00, 0xf7])),
    ]
    if markers:
        for bar, name in [(0, b"Intro"), (2, b"Verse"), (6, b"Chorus")]:
            t0.append((bar * TPB, meta(0x06, name)))

    melody = []  # (tick, delta, payload) dummies; built via helper below
    drum = []

    def note_events(base_tick, ch, notes):
        evs = []
        for i, n in enumerate(notes):
            t = base_tick + i * STEP
            evs.append((t, bytes([0x90 | ch, n, 96])))
            evs.append((t + 120, bytes([0x80 | ch, n, 0])))
        return evs

    # Verse: melody A on channel 0, first two bars of the 4-bar section.
    t1 = [(2 * TPB, bytes([0xC0, 0]))]  # ch 0 -> Acoustic Grand
    t1 += note_events(2 * TPB, 0, [60, 62, 64, 65, 67, 64, 62, 60] * 2)

    # Chorus: melody B on channel 1, higher, different patch.
    t2 = [(6 * TPB, bytes([0xC0 | 1, 80]))]
    t2 += note_events(6 * TPB, 1, [67, 71, 72, 74, 76, 74, 72, 71] * 2)

    # Drums on channel 10 (GM drum), one beat per bar for the whole song.
    t3 = []
    for bar in range(10):
        b0 = bar * TPB
        t3 += [(b0, bytes([0x99, 36, 100]))]
        t3 += [(b0 + 8 * STEP, bytes([0x99, 36, 100]))]
        t3 += [(b0 + 4 * STEP, bytes([0x99, 38, 100]))]
        t3 += [(b0 + 12 * STEP, bytes([0x99, 38, 100]))]
        for s in range(0, 16, 2):
            t3.append((b0 + s * STEP, bytes([0x99, 42, 80])))

    file_data = (b"MThd" + struct.pack(">IHHH", 6, 1, 2, DIV) +
                 track_body(t0) + track_body(t1) + track_body(t2) + track_body(t3))
    return file_data


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    for name, markers in [("test_sections.mid", True), ("test_nomarker.mid", False)]:
        with open(os.path.join(here, name), "wb") as f:
            f.write(build(markers))
        print("wrote", name)


if __name__ == "__main__":
    sys.exit(main())