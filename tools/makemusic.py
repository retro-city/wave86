#!/usr/bin/env python3
"""WAVE86 soundtrack composer.

Generates original synthwave tunes as id Software IMF files (type-1,
560 Hz - the id standard rate) for the launcher's AdLib player.

    makemusic.py music/          # writes the whole soundtrack
"""
import os, sys, random

TICK_HZ = 560

# ---------------------------------------------------------------- OPL


def fnum_block(midi):
    f = 440.0 * 2 ** ((midi - 69) / 12.0)
    for block in range(1, 8):
        fn = int(round(f * 2 ** (20 - block) / 49716.0))
        if fn <= 1023:
            return fn, block
    return 1023, 7


class Patch:
    """(am,vib,eg,ksr,mult,tl,ar,dr,sl,rr,ws) per op + fb, conn."""
    def __init__(self, op1, op2, fb, conn):
        self.op1, self.op2, self.fb, self.conn = op1, op2, fb, conn


def op_regs(o):
    am, vib, eg, ksr, mult, tl, ar, dr, sl, rr, ws = o
    return {
        0x20: (am << 7) | (vib << 6) | (eg << 5) | (ksr << 4) | mult,
        0x40: tl & 0x3F,
        0x60: (ar << 4) | dr,
        0x80: (sl << 4) | rr,
        0xE0: ws,
    }


#             am vib eg ksr mult tl  ar dr sl rr ws
BASS = Patch((0, 0, 0, 0, 1, 14, 15, 5, 3, 7, 1),
             (0, 0, 0, 0, 1, 10, 15, 4, 4, 7, 0), fb=4, conn=0)
PAD = Patch((0, 0, 1, 0, 1, 40, 6, 3, 2, 5, 1),
            (0, 0, 1, 0, 1, 18, 5, 2, 1, 5, 0), fb=1, conn=0)
ARP = Patch((0, 0, 0, 0, 3, 24, 15, 8, 6, 8, 1),
            (0, 0, 0, 0, 1, 14, 15, 7, 5, 8, 3), fb=5, conn=0)
LEAD = Patch((0, 0, 1, 0, 2, 20, 15, 6, 3, 6, 1),
             (0, 1, 1, 0, 1, 8, 13, 5, 2, 6, 2), fb=6, conn=0)

OP1_OFF = [0, 1, 2, 8, 9, 10, 16, 17, 18]

# rhythm mode bits
R_ON, BD, SD, TT, CY, HH = 0x20, 0x10, 0x08, 0x04, 0x02, 0x01


class Song:
    def __init__(self):
        self.ev = []
        self.seq = 0
        self.last_b0 = [0] * 9

    def emit(self, t, reg, val):
        self.ev.append((t, self.seq, reg, val))
        self.seq += 1

    def set_patch(self, t, ch, p):
        o1, o2 = OP1_OFF[ch], OP1_OFF[ch] + 3
        for base, val in op_regs(p.op1).items():
            self.emit(t, base + o1, val)
        for base, val in op_regs(p.op2).items():
            self.emit(t, base + o2, val)
        self.emit(t, 0xC0 + ch, (p.fb << 1) | p.conn)

    def note_on(self, t, ch, midi):
        fn, block = fnum_block(midi)
        self.emit(t, 0xA0 + ch, fn & 0xFF)
        b0 = 0x20 | (block << 2) | (fn >> 8)
        self.emit(t, 0xB0 + ch, b0)
        self.last_b0[ch] = b0

    def note_off(self, t, ch):
        self.emit(t, 0xB0 + ch, self.last_b0[ch] & ~0x20)

    def set_freq(self, t, ch, midi):
        fn, block = fnum_block(midi)
        self.emit(t, 0xA0 + ch, fn & 0xFF)
        self.emit(t, 0xB0 + ch, (block << 2) | (fn >> 8))
        self.last_b0[ch] = (block << 2) | (fn >> 8)

    def to_imf(self):
        evs = sorted(self.ev, key=lambda e: (e[0], e[1]))
        out = bytearray()
        for i, (t, _, reg, val) in enumerate(evs):
            delay = evs[i + 1][0] - t if i + 1 < len(evs) else TICK_HZ
            out += bytes((reg, val, delay & 0xFF, (delay >> 8) & 0xFF))
        if len(out) > 0xFFFE:
            raise SystemExit("track too long for a type-1 IMF")
        return bytes((len(out) & 0xFF, len(out) >> 8)) + bytes(out)


# ---------------------------------------------------------------- music

MINOR = [0, 2, 3, 5, 7, 8, 10]

PROGS = [
    [(0, 0), (8, 1), (3, 1), (10, 1)],   # i VI III VII
    [(0, 0), (5, 0), (8, 1), (10, 1)],   # i iv VI VII
    [(0, 0), (10, 1), (8, 1), (10, 1)],  # i VII VI VII
    [(0, 0), (3, 1), (8, 1), (5, 0)],    # i III VI iv
]

DRUMS = {
    "four": {0: BD | HH, 2: HH, 4: BD | SD | HH, 6: HH,
             8: BD | HH, 10: HH, 12: BD | SD | HH, 14: HH | CY},
    "half": {0: BD | HH, 2: HH, 4: HH, 6: HH, 8: SD | HH,
             10: HH, 11: BD, 12: HH, 14: HH},
}


def compose(cfg):
    rng = random.Random(cfg["seed"])
    s = Song()
    st = cfg["st"]                       # ticks per 16th
    bar = st * 16
    key = cfg["key"]                     # midi root, octave 3-ish
    prog = PROGS[cfg["prog"]]
    drums = DRUMS[cfg["drums"]]

    CH_BASS, CH_P1, CH_P2, CH_P3, CH_ARP, CH_LEAD = 0, 1, 2, 3, 4, 5

    # patches + rhythm mode drum setup
    s.set_patch(0, CH_BASS, BASS)
    for ch in (CH_P1, CH_P2, CH_P3):
        s.set_patch(0, ch, PAD)
    s.set_patch(0, CH_ARP, ARP)
    s.set_patch(0, CH_LEAD, LEAD)
    drum_ops = {                         # off: (mult, tl, ar, dr, sl, rr)
        0x10: (0, 8, 15, 8, 7, 8), 0x13: (0, 0, 15, 9, 7, 9),   # kick
        0x14: (0, 3, 15, 10, 7, 11),                            # snare
        0x11: (1, 10, 15, 12, 9, 13),                           # hihat
        0x12: (0, 15, 15, 10, 7, 10),                           # tom
        0x15: (1, 12, 14, 9, 8, 9),                             # cymbal
    }
    for off, (mult, tl, ar, dr, sl, rr) in drum_ops.items():
        s.emit(0, 0x20 + off, mult)
        s.emit(0, 0x40 + off, tl)
        s.emit(0, 0x60 + off, (ar << 4) | dr)
        s.emit(0, 0x80 + off, (sl << 4) | rr)
        s.emit(0, 0xE0 + off, 0)
    s.emit(0, 0xC0 + 6, 0x06)
    s.emit(0, 0xC0 + 7, 0x00)
    s.emit(0, 0xC0 + 8, 0x00)
    s.set_freq(0, 6, key - 12)           # kick pitch
    s.set_freq(0, 7, key + 29)           # snare/hat pitch
    s.set_freq(0, 8, key + 36)           # tom/cymbal pitch
    s.emit(0, 0xBD, R_ON)

    # lead motif: 2 bars of (start16, len16, scale step) covering 32 16ths
    motif = []
    pos, step = 0, rng.choice([0, 2, 4])
    while pos < 30:
        ln = rng.choice([2, 2, 4, 4, 6])
        ln = min(ln, 32 - pos)
        motif.append((pos, ln, step))
        step = max(0, min(9, step + rng.choice([-2, -1, -1, 1, 1, 2, 3])))
        pos += ln + (2 if rng.random() < 0.25 else 0)

    def scale_note(step, base):
        return base + 12 * (step // 7) + MINOR[step % 7]

    def chord_notes(b):
        root_off, major = prog[b % 4]
        root = key + root_off
        third = root + (4 if major else 3)
        return root, third, root + 7

    def bar_pad(b, t0):
        r, t3, f5 = chord_notes(b)
        for ch, n in ((CH_P1, r + 12), (CH_P2, t3 + 12), (CH_P3, f5 + 12)):
            s.note_on(t0, ch, n)
            s.note_off(t0 + bar - 2, ch)

    def bar_bass(b, t0):
        r, _, _ = chord_notes(b)
        for i in range(8):
            n = r - 12 + (12 if cfg["octave_bass"] and i % 2 else 0)
            s.note_on(t0 + i * st * 2, CH_BASS, n)
            s.note_off(t0 + i * st * 2 + st * 2 - 2, CH_BASS)

    def bar_arp(b, t0):
        r, t3, f5 = chord_notes(b)
        tones = [r + 12, t3 + 12, f5 + 12, r + 24, f5 + 12, t3 + 12]
        patt = cfg["arp"]
        for i in range(16):
            n = tones[patt[i % len(patt)] % len(tones)]
            s.note_on(t0 + i * st, CH_ARP, n)
            s.note_off(t0 + i * st + st - 2, CH_ARP)

    def bar_drums(b, t0, fill=False):
        patt = dict(drums)
        if fill:
            for i in (10, 12, 14):
                patt[i] = patt.get(i, 0) | SD
        for i in range(16):
            bits = patt.get(i, 0)
            if bits:
                s.emit(t0 + i * st, 0xBD, R_ON)          # edge: retrigger
                s.emit(t0 + i * st, 0xBD, R_ON | bits)

    def bars_lead(b0_, t0, nbars):
        for rep in range(nbars // 2):
            tr = t0 + rep * 2 * bar
            var = rng.choice([0, 0, 0, 2, -3]) if rep % 2 else 0
            for pos, ln, step in motif:
                n = scale_note(step + (var > 0) - (var < 0), key + 24)
                s.note_on(tr + pos * st, CH_LEAD, n)
                s.note_off(tr + pos * st + ln * st - 2, CH_LEAD)

    # ---------------- arrangement ----------------
    t = st * 2
    b = 0

    for i in range(4):                    # intro: pads + arp
        bar_pad(b, t); bar_arp(b, t); t += bar; b += 1
    for i in range(8):                    # groove
        bar_pad(b, t); bar_arp(b, t); bar_bass(b, t)
        bar_drums(b, t, fill=(i == 7)); t += bar; b += 1
    lead_start = t
    for i in range(16):                   # lead section
        bar_pad(b, t); bar_bass(b, t)
        if i % 4 < 3:
            bar_arp(b, t)
        bar_drums(b, t, fill=(i % 8 == 7)); t += bar; b += 1
    bars_lead(b, lead_start, 16)
    for i in range(4):                    # breakdown
        bar_pad(b, t); bar_bass(b, t); t += bar; b += 1
    for i in range(8):                    # reprise
        bar_pad(b, t); bar_arp(b, t); bar_bass(b, t)
        bar_drums(b, t, fill=(i == 7)); t += bar; b += 1

    # outro: silence everything
    for ch in range(6):
        s.note_off(t, ch)
    s.emit(t + 2, 0xBD, 0)

    return s.to_imf()


TRACKS = [
    ("NIGHTRUN.IMF", dict(seed=86001, key=45, st=68, prog=0, drums="four",
                          octave_bass=True, arp=[0, 1, 2, 3, 2, 1])),
    ("SUNSETDR.IMF", dict(seed=86002, key=48, st=80, prog=1, drums="half",
                          octave_bass=False, arp=[0, 2, 1, 2])),
    ("GRIDLOCK.IMF", dict(seed=86003, key=43, st=60, prog=2, drums="four",
                          octave_bass=True, arp=[0, 3, 1, 4, 2, 5])),
    ("CHROME86.IMF", dict(seed=86004, key=50, st=72, prog=3, drums="half",
                          octave_bass=False, arp=[0, 1, 3, 1, 2, 4])),
]


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "music"
    os.makedirs(outdir, exist_ok=True)
    for name, cfg in TRACKS:
        data = compose(cfg)
        if len(data) > 60000:
            raise SystemExit(f"{name}: {len(data)} bytes exceeds player cap")
        path = os.path.join(outdir, name)
        with open(path, "wb") as f:
            f.write(data)
        dur = sum(data[i + 2] | (data[i + 3] << 8)
                  for i in range(2, len(data), 4)) / TICK_HZ
        print(f"{path}: {len(data)} bytes, {dur:.0f}s, "
              f"{(len(data) - 2) // 4} events")


if __name__ == "__main__":
    main()
