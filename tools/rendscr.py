#!/usr/bin/env python3
"""Render a NEON86 /dump (SCREEN.BIN + FONT.BIN + PAL.BIN) to a PNG
and an ANSI terminal preview. Pure stdlib - no Pillow needed.

SCREEN.BIN: 80x25 words (char, attr)
FONT.BIN:   256 glyphs x 16 rows x 8 px (VGA 8x16 bitmap font)
PAL.BIN:    16 x RGB, 6-bit values
"""
import sys, zlib, struct, argparse


def write_png(path, w, h, rgb):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3]
                   for y in range(h))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("screen")
    ap.add_argument("font")
    ap.add_argument("pal")
    ap.add_argument("-o", "--out", default="screen.png")
    ap.add_argument("--no-ansi", action="store_true")
    ap.add_argument("--thumb", help=".THM whose glyphs were loaded into font RAM")
    args = ap.parse_args()

    scr = open(args.screen, "rb").read()
    font = open(args.font, "rb").read()
    pal6 = open(args.pal, "rb").read()
    assert len(scr) == 4000, f"screen dump is {len(scr)} bytes, want 4000"
    assert len(font) >= 4096
    fontB = font
    if args.thumb:
        t = open(args.thumb, "rb").read()
        fa, fb = bytearray(font), bytearray(font)
        pos = 8
        for bank in (fa, fb):
            for k in range(t[6] if bank is fa else t[7]):
                code = t[pos]
                bank[code * 16:code * 16 + 16] = t[pos + 1:pos + 17]
                pos += 17
        font, fontB = bytes(fa), bytes(fb)
    pal = [(pal6[i * 3] * 255 // 63,
            pal6[i * 3 + 1] * 255 // 63,
            pal6[i * 3 + 2] * 255 // 63) for i in range(16)]

    W, H = 80 * 8, 25 * 16
    img = bytearray(W * H * 3)
    for cy in range(25):
        for cx in range(80):
            ch = scr[(cy * 80 + cx) * 2]
            at = scr[(cy * 80 + cx) * 2 + 1]
            fg, bg = pal[at & 15], pal[(at >> 4) & 15]
            glyph = (fontB if at & 8 else font)[ch * 16:ch * 16 + 16]
            for gy in range(16):
                row = glyph[gy]
                base = ((cy * 16 + gy) * W + cx * 8) * 3
                for gx in range(8):
                    c = fg if row & (0x80 >> gx) else bg
                    o = base + gx * 3
                    img[o:o + 3] = bytes(c)
    write_png(args.out, W, H, bytes(img))
    print(f"wrote {args.out} ({W}x{H})")

    if not args.no_ansi:
        for cy in range(25):
            line = []
            for cx in range(80):
                ch = scr[(cy * 80 + cx) * 2]
                at = scr[(cy * 80 + cx) * 2 + 1]
                fg, bg = pal[at & 15], pal[(at >> 4) & 15]
                u = bytes([ch]).decode("cp437")
                line.append(f"\x1b[38;2;{fg[0]};{fg[1]};{fg[2]}m"
                            f"\x1b[48;2;{bg[0]};{bg[1]};{bg[2]}m{u}")
            print("".join(line) + "\x1b[0m")


if __name__ == "__main__":
    main()
