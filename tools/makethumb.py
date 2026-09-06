#!/usr/bin/env python3
"""makethumb - turn a screenshot into a WAVE86 text-mode thumbnail.

    makethumb.py screenshot.png THUMBS/KEEN4.THM

The trick: VGA text mode has no pixels, but its 8x16 character shapes
live in RAM. The thumbnail is a 26x6 grid of cells; each cell gets two
colours from the launcher's 16-colour palette and a 1-bit pattern that
becomes a custom glyph. Up to 100 glyphs are redefined (CP437 codes the
UI never uses); flat cells and cells that look like standard block
characters need none. Needs ffmpeg to decode the image.

.THM layout: "W86T", cols, rows, nglyphs, then nglyphs x (code, 16 bytes
of bitmap), then cols*rows x (char, attribute).
"""
import sys, subprocess, struct

COLS, ROWS = 26, 6
W, H = COLS * 8, ROWS * 16
BUDGET = 100

# the launcher's VGA palette (vga.c synth_pal, 6-bit) as 8-bit RGB
PAL6 = [(2,0,8),(9,4,24),(0,34,24),(0,46,52),(52,6,22),(44,0,42),(58,24,2),(36,32,46),
        (15,9,26),(27,24,60),(18,60,40),(28,63,63),(63,26,34),(63,24,56),(63,54,16),(62,58,63)]
PAL = [(r*255//63, g*255//63, b*255//63) for r, g, b in PAL6]

# characters the UI does not use, safe to redefine
FREE = list(range(0x80, 0xAE)) + list(range(0xE0, 0xFE)) + list(range(0xB5, 0xBF)) \
     + list(range(0xC1, 0xC4)) + list(range(0xC5, 0xD9))

# standard block characters as 8x16 bitmaps (row bytes)
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


def dist(a, b):
    return sum((x - y) ** 2 for x, y in zip(a, b))


def hamming(a, b):
    return sum(bin(x ^ y).count("1") for x, y in zip(a, b))


def main():
    src, dst = sys.argv[1], sys.argv[2]
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", src, "-vf", f"scale={W}:{H}",
                          "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], capture_output=True, check=True).stdout
    px = [(raw[i], raw[i+1], raw[i+2]) for i in range(0, len(raw), 3)]

    cells = []                       # (bitmap rows, fg, bg)
    for cy in range(ROWS):
        for cx in range(COLS):
            pts = [px[(cy*16+y)*W + cx*8 + x] for y in range(16) for x in range(8)]
            idx = [nearest(p) for p in pts]
            count = {}
            for i in idx: count[i] = count.get(i, 0) + 1
            top = sorted(count, key=lambda i: -count[i])
            fg = top[0]; bg = top[1] if len(top) > 1 else top[0]
            if fg == bg:
                cells.append((STD[0xDB], fg, bg)); continue
            bits = []
            for y in range(16):
                b = 0
                for x in range(8):
                    p = pts[y*8+x]
                    if dist(p, PAL[fg]) <= dist(p, PAL[bg]):
                        b |= 0x80 >> x
                bits.append(b)
            cells.append((bits, fg, bg))

    # assign glyphs: standard block chars when close, custom ones otherwise,
    # sharing custom glyphs between near-identical patterns
    custom = []                      # bitmaps in FREE order
    out = []
    for bits, fg, bg in cells:
        best = min(STD, key=lambda c: hamming(STD[c], bits))
        if hamming(STD[best], bits) <= 6:
            out.append((best, fg, bg)); continue
        inv = [b ^ 0xFF for b in bits]
        besti = min(STD, key=lambda c: hamming(STD[c], inv))
        if hamming(STD[besti], inv) <= 6:
            out.append((besti, bg, fg)); continue
        hit = None
        for k, g in enumerate(custom):
            if hamming(g, bits) <= 8: hit = k; break
        if hit is None and len(custom) < BUDGET:
            custom.append(bits); hit = len(custom) - 1
        if hit is None:              # budget gone: nearest of anything
            best = min(STD, key=lambda c: hamming(STD[c], bits))
            out.append((best, fg, bg)); continue
        out.append((FREE[hit], fg, bg))

    with open(dst, "wb") as f:
        f.write(b"W86T" + bytes((COLS, ROWS, len(custom))))
        for k, g in enumerate(custom):
            f.write(bytes([FREE[k]] + g))
        for ch, fg, bg in out:
            f.write(bytes((ch, (bg << 4) | fg)))
    print(f"{dst}: {COLS}x{ROWS} cells, {len(custom)} custom glyphs")


if __name__ == "__main__":
    main()
