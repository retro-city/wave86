#!/usr/bin/env python3
"""The mTCP sources were written for DOS, where PACKET.H answers to
#include "packet.h" and "Arp.h" alike. On Linux they do not, so this makes
a folder of links named exactly as the includes spell them, each pointing
at the real header; the Makefile puts it first on the include path.

    python3 tools/mtcp_inc.py build/mtcp-inc
"""
import os, re, sys

ROOT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "net")
out = sys.argv[1]
os.makedirs(out, exist_ok=True)
files = [os.path.join(r, f) for r, _, fs in os.walk(ROOT) for f in fs]
by_lower = {}
for f in files:
    by_lower.setdefault(os.path.basename(f).lower(), f)
wanted = set()
for f in files:
    if not f.lower().endswith((".cpp", ".c", ".h", ".asm", ".cfg")):
        continue
    text = open(f, "rb").read().decode("latin-1")
    wanted.update(re.findall(r'^\s*#\s*include\s+"([^"]+)"', text, re.M))
made = 0
for name in sorted(wanted):
    real = by_lower.get(os.path.basename(name).lower())
    if not real or "/" in name or "\\" in name:
        continue
    link = os.path.join(out, name)
    if os.path.islink(link) or os.path.exists(link):
        os.remove(link)
    os.symlink(os.path.abspath(real), link)
    made += 1
open(os.path.join(out, ".stamp"), "w").close()
print(f"mtcp_inc: {made} names linked in {out}")
