#!/usr/bin/env python3
"""makethumb - turn a screenshot into a WAVE86 text-mode picture.

    makethumb.py screenshot.png THUMBS/KEEN4.THM

VGA text mode has no pixels, but its character shapes live in RAM, and
in 512-character mode bit 3 of the attribute picks one of two fonts.
The picture fills the details pane under the game's title: 38x12 cells,
each two colours from the launcher's palette plus a 1-bit pattern that
becomes a custom glyph. Bank A (foreground colours 0-7) and bank B
(8-15) each lend the CP437 codes the UI never uses; flat cells and cells
that look like standard block characters need no custom glyph. When a
busy picture needs more glyphs than that, near-identical patterns are
merged with a rising tolerance until it fits. Needs ffmpeg to decode.

.THM: "W86T", cols, rows, nA, nB, then nA x (code, 16 bytes) sorted by
code for bank A, nB x the same for bank B, then cols*rows x (char, attr).
"""
import sys, subprocess

COLS, ROWS = 38, 12
W, H = COLS * 8, ROWS * 16

PAL6 = [(2,0,8),(9,4,24),(0,34,24),(0,46,52),(52,6,22),(44,0,42),(58,24,2),(36,32,46),
        (15,9,26),(27,24,60),(18,60,40),(28,63,63),(63,26,34),(63,24,56),(63,54,16),(62,58,63)]
PAL = [(r*255//63, g*255//63, b*255//63) for r, g, b in PAL6]

# codes the UI never displays, available in both banks
FREE = (list(range(0x01, 0x0D)) + [0x0F] + list(range(0x11, 0x18)) + list(range(0x1A, 0x20))
        + [0x7F] + list(range(0x80, 0xB0)) + list(range(0xB4, 0xBF)) + list(range(0xC1, 0xC4))
        + list(range(0xC5, 0xD9)) + list(range(0xE0, 0xFA)) + list(range(0xFB, 0xFE)) + [0xFF])

def rows(f): return [f(y) for y in range(16)]
STD = {
    0x20: rows(lambda y: 0x00), 0xDB: rows(lambda y: 0xFF),
    0xDC: rows(lambda y: 0xFF if y >= 8 else 0x00), 0xDF: rows(lambda y: 0xFF if y < 8 else 0x00),
    0xDD: rows(lambda y: 0xF0), 0xDE: rows(lambda y: 0x0F),
    0xB0: rows(lambda y: 0x88 if y % 2 == 0 else 0x22), 0xB1: rows(lambda y: 0xAA if y % 2 == 0 else 0x55),
    0xB2: rows(lambda y: 0xDD if y % 2 == 0 else 0x77),
}


def nearest(c):
    r, g, b = c
    return min(range(16), key=lambda i: (PAL[i][0]-r)**2 + (PAL[i][1]-g)**2 + (PAL[i][2]-b)**2)

def dist(a, b): return sum((x - y) ** 2 for x, y in zip(a, b))
def hamming(a, b): return sum(bin(x ^ y).count("1") for x, y in zip(a, b))
def invert(bits): return [b ^ 0xFF for b in bits]


def analyse(px):
    """per cell: (bits with colour 1 set, colour 1, colour 2)"""
    cells = []
    for cy in range(ROWS):
        for cx in range(COLS):
            pts = [px[(cy*16+y)*W + cx*8 + x] for y in range(16) for x in range(8)]
            idx = [nearest(p) for p in pts]
            count = {}
            for i in idx: count[i] = count.get(i, 0) + 1
            top = sorted(count, key=lambda i: -count[i])
            c1 = top[0]; c2 = top[1] if len(top) > 1 else top[0]
            if c1 == c2:
                cells.append((STD[0xDB], c1, c1)); continue
            bits = []
            for y in range(16):
                b = 0
                for x in range(8):
                    if dist(pts[y*8+x], PAL[c1]) <= dist(pts[y*8+x], PAL[c2]):
                        b |= 0x80 >> x
                bits.append(b)
            cells.append((bits, c1, c2))
    return cells


def assign(cells, tol):
    """glyph codes for every cell with merge tolerance tol"""
    bank = {0: [], 1: []}
    out = []
    stats = {"flat": 0, "std": 0, "reuse": 0, "new": 0, "fallback": 0}

    def try_bank(bits, fg, bg):
        b = 1 if fg >= 8 else 0
        for k, g in enumerate(bank[b]):
            if hamming(g, bits) <= tol:
                stats["reuse"] += 1
                return (FREE[k], fg, bg)
        if len(bank[b]) < len(FREE):
            bank[b].append(bits)
            stats["new"] += 1
            return (FREE[len(bank[b]) - 1], fg, bg)
        return None

    for bits, c1, c2 in cells:
        if c1 == c2:
            out.append((0xDB, c1, c1)); stats["flat"] += 1; continue
        best = min(STD, key=lambda c: hamming(STD[c], bits))
        if hamming(STD[best], bits) <= 6:
            out.append((best, c1, c2)); stats["std"] += 1; continue
        inv = invert(bits)
        besti = min(STD, key=lambda c: hamming(STD[c], inv))
        if hamming(STD[besti], inv) <= 6:
            out.append((besti, c2, c1)); stats["std"] += 1; continue
        opts = [(bits, c1, c2), (inv, c2, c1)]
        opts.sort(key=lambda o: len(bank[1 if o[1] >= 8 else 0]))
        r = try_bank(*opts[0]) or try_bank(*opts[1])
        if r is None:
            out.append((best, c1, c2)); stats["fallback"] += 1
        else:
            out.append(r)
    return bank, out, stats


def main():
    src, dst = sys.argv[1], sys.argv[2]
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", src, "-vf", f"scale={W}:{H}",
                          "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], capture_output=True, check=True).stdout
    px = [(raw[i], raw[i+1], raw[i+2]) for i in range(0, len(raw), 3)]
    cells = analyse(px)

    for tol in (6, 10, 14, 18, 24, 32):
        bank, out, stats = assign(cells, tol)
        if stats["fallback"] <= 4:
            break

    with open(dst, "wb") as f:
        f.write(b"W86T" + bytes((COLS, ROWS, len(bank[0]), len(bank[1]))))
        for b in (0, 1):
            for k, g in enumerate(bank[b]):
                f.write(bytes([FREE[k]] + g))
        for ch, fg, bg in out:
            f.write(bytes((ch, (bg << 4) | fg)))
    print(f"{dst}: {COLS}x{ROWS} tol={tol}, glyphs A={len(bank[0])} B={len(bank[1])} of {len(FREE)}; "
          f"cells flat={stats['flat']} std={stats['std']} reuse={stats['reuse']} new={stats['new']} fallback={stats['fallback']}")


if __name__ == "__main__":
    main()
