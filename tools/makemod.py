#!/usr/bin/env python3
"""Generate an original 4-channel ProTracker module for WAVE86.

    makemod.py music/WAVE86.MOD

Chip-style: tiny looped waveforms for bass/lead/pad plus synthesized
drums, so the file stays small and the DOS mixer has an easy time.
"""
import sys, math, random, struct

# ProTracker period table, octaves 1-3
NOTES = {}
_base = [856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453]
_names = ["C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"]
for o in range(3):
    for i, n in enumerate(_names):
        NOTES[f"{n}{o + 1}"] = round(_base[i] / (2 ** o))


def s8(v):
    return max(-128, min(127, int(round(v))))


# ---------------------------------------------------------------- samples
def kick():
    out = []
    ph = 0.0
    for i in range(2200):
        t = i / 8287.0
        f = 40 + 140 * math.exp(-t * 28)
        ph += 2 * math.pi * f / 8287.0
        out.append(s8(120 * math.sin(ph) * math.exp(-t * 9)))
    return out


def snare(rng):
    out = []
    for i in range(2000):
        t = i / 8287.0
        n = rng.uniform(-1, 1) * math.exp(-t * 14)
        b = 0.5 * math.sin(2 * math.pi * 180 * t) * math.exp(-t * 30)
        out.append(s8(115 * (n + b)))
    return out


def hat(rng):
    out = []
    for i in range(500):
        t = i / 8287.0
        out.append(s8(110 * rng.uniform(-1, 1) * math.exp(-t * 55)))
    return out


def saw(n=32):
    return [s8(-120 + 240 * i / n) for i in range(n)]


def square(n=32):
    return [s8(110 if i < n // 2 else -110) for i in range(n)]


def soft(n=64):
    return [s8(90 * math.sin(2 * math.pi * i / n)
               + 30 * math.sin(6 * math.pi * i / n)) for i in range(n)]


# ---------------------------------------------------------------- song
def compose(seed=86):
    rng = random.Random(seed)
    samples = [
        ("kick",   kick(),     64, False),
        ("snare",  snare(rng), 50, False),
        ("hat",    hat(rng),   26, False),
        ("bass",   saw(),      44, True),
        ("lead",   square(),   30, True),
        ("pad",    soft(),     22, True),
    ]
    KICK, SNARE, HAT, BASS, LEAD, PAD = range(1, 7)

    # chord progression: (root note name for bass octave, chord tones)
    prog = [
        ("A-1", ["A-2", "C-3", "E-3"]),
        ("F-1", ["F-2", "A-2", "C-3"]),
        ("C-2", ["C-3", "E-3", "G-3"]),
        ("G-1", ["G-2", "B-2", "D-3"]),
    ]

    def empty_pattern():
        return [[(0, 0, 0, 0) for _ in range(4)] for _ in range(64)]

    def put(p, row, ch, note=None, smp=0, eff=0, prm=0):
        period = NOTES[note] if note else 0
        p[row][ch] = (smp, period, eff, prm)

    def drums(p, fill=False):
        for bar in range(4):
            b = bar * 16
            for r in range(0, 16, 2):
                put(p, b + r, 0, "C-2", HAT)
            put(p, b + 0, 0, "C-2", KICK)
            put(p, b + 8, 0, "C-2", KICK)
            put(p, b + 4, 0, "C-2", SNARE)
            put(p, b + 12, 0, "C-2", SNARE)
            if bar == 3 and fill:
                for r in (10, 13, 14, 15):
                    put(p, b + r, 0, "C-2", SNARE)

    def bass(p, octave_pump=True):
        for bar in range(4):
            root, _ = prog[bar]
            b = bar * 16
            for r in range(0, 16, 2):
                n = root
                if octave_pump and (r // 2) % 2:
                    n = root[:-1] + str(int(root[-1]) + 1)
                put(p, b + r, 1, n, BASS)

    def arp(p, vib=False):
        for bar in range(4):
            _, tones = prog[bar]
            b = bar * 16
            seq = [tones[0], tones[1], tones[2], tones[1]]
            for r in range(16):
                eff, prm = (4, 0x36) if vib and r % 4 == 0 else (0, 0)
                put(p, b + r, 2, seq[r % 4], LEAD, eff, prm)

    def melody(p):
        scale = ["A-2", "B-2", "C-3", "D-3", "E-3", "F-3", "G-3", "A-3"]
        step = 4
        r = 0
        while r < 64:
            ln = rng.choice([2, 2, 4, 4, 6, 8])
            put(p, r, 2, scale[step], LEAD, 4, 0x25)
            step = max(0, min(7, step + rng.choice([-2, -1, 1, 1, 2])))
            r += ln

    def pad(p):
        for bar in range(4):
            _, tones = prog[bar]
            put(p, bar * 16, 3, tones[2], PAD, 0xC, 22)

    pats = []
    A = empty_pattern(); pad(A); bass(A, False); put(A, 0, 0, None, 0, 0xF, 0x7D)
    pats.append(A)
    B = empty_pattern(); drums(B); bass(B); arp(B); pad(B)
    pats.append(B)
    C = empty_pattern(); drums(C, fill=True); bass(C); melody(C); pad(C)
    pats.append(C)
    D = empty_pattern(); drums(D); bass(D); arp(D, vib=True); pad(D)
    put(D, 63, 0, None, 0, 0xD, 0x00)          # pattern break: keep length
    pats.append(D)
    order = [0, 1, 1, 2, 2, 3, 1, 0]
    return samples, pats, order


def write_mod(path, samples, pats, order):
    out = bytearray()
    out += b"WAVE86 SYNTH".ljust(20, b"\0")
    for i in range(31):
        if i < len(samples):
            name, data, vol, loop = samples[i]
            words = len(data) // 2
            out += name.encode().ljust(22, b"\0")
            out += struct.pack(">H", words)
            out += bytes((0, vol))
            if loop:
                out += struct.pack(">HH", 0, words)
            else:
                out += struct.pack(">HH", 0, 1)
        else:
            out += b"\0" * 22 + struct.pack(">HBBHH", 0, 0, 0, 0, 1)
    out += bytes((len(order), 127))
    out += bytes(order).ljust(128, b"\0")
    out += b"M.K."
    for p in pats:
        for row in p:
            for smp, period, eff, prm in row:
                out += bytes(((smp & 0xF0) | (period >> 8), period & 0xFF,
                              ((smp & 0x0F) << 4) | eff, prm))
    for name, data, vol, loop in samples:
        if len(data) % 2:
            data = data + [0]
        out += bytes(v & 0xFF for v in data)
    with open(path, "wb") as f:
        f.write(out)
    print(f"{path}: {len(out)} bytes, {len(pats)} patterns, "
          f"{len(order)} orders")


if __name__ == "__main__":
    dst = sys.argv[1] if len(sys.argv) > 1 else "music/WAVE86.MOD"
    write_mod(dst, *compose())
