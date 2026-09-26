#!/usr/bin/env python3
"""thirdparty.py - the other people's programs the build needs, kept under
third-party/ so that a build does not depend on a retro download site
being up that day (they come and go). The Makefile's update-* targets
call this to refresh a piece from where it came; check keeps CI honest.

    thirdparty.py fetch DEST URL     fetch URL into DEST and say whether it changed
    thirdparty.py add DEST NOTE      record a file put there by hand, with where it came from
    thirdparty.py check              every recorded file is there and unchanged

third-party/SOURCES.txt records, per file, its SHA-256, the day it was
fetched and where from (tab-separated, one line each). A fetch that brings
the same bytes leaves the file and its line alone; one that fails leaves
the old file in place and says so. A relative DEST is taken from the
repository's root, whatever the current directory.
"""
import argparse, datetime, hashlib, os, sys, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TP = os.path.join(ROOT, "third-party")
MANIFEST = os.path.join(TP, "SOURCES.txt")
HEADER = ("# What third-party/ holds and where each file came from: path, SHA-256,\n"
          "# the day it was fetched, the URL (or a note, for a file put there by hand).\n"
          "# tools/thirdparty.py keeps this file; make update-third-party refreshes the\n"
          "# pieces, make check-third-party verifies them. THIRD-PARTY.md has the terms.\n")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def full(path):
    """a path as given: relative ones are from the repository's root"""
    return path if os.path.isabs(path) else os.path.join(ROOT, path)


def rel(path):
    return os.path.relpath(os.path.abspath(full(path)), ROOT).replace(os.sep, "/")


def read_manifest():
    entries = {}
    if not os.path.exists(MANIFEST):
        return entries
    for line in open(MANIFEST, encoding="utf-8"):
        line = line.rstrip("\n")
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) != 4:
            sys.exit(f"thirdparty: SOURCES.txt: not four tab-separated fields: {line!r}")
        entries[parts[0]] = (parts[1], parts[2], parts[3])
    return entries


def write_manifest(entries):
    with open(MANIFEST, "w", encoding="utf-8") as f:
        f.write(HEADER)
        for path in sorted(entries):
            sha, day, origin = entries[path]
            f.write(f"{path}\t{sha}\t{day}\t{origin}\n")


def record(path, origin):
    entries = read_manifest()
    entries[rel(path)] = (sha256(full(path)), datetime.date.today().isoformat(), origin)
    write_manifest(entries)


def fetch(dest, url):
    dest = full(dest)
    part = dest + ".part"
    os.makedirs(os.path.dirname(os.path.abspath(dest)) or ".", exist_ok=True)
    req = urllib.request.Request(url, headers={"User-Agent": "wave86-thirdparty/1.0"})
    try:
        with urllib.request.urlopen(req, timeout=60) as r, open(part, "wb") as f:
            while True:
                block = r.read(1 << 16)
                if not block:
                    break
                f.write(block)
    except Exception as e:                      # the old file stays; the build still works
        if os.path.exists(part):
            os.remove(part)
        print(f"thirdparty: {rel(dest)}: could not fetch {url}: {e}")
        return 1
    try:
        new = sha256(part)
        if os.path.exists(dest) and sha256(dest) == new:
            entries = read_manifest()
            if rel(dest) not in entries:
                record(dest, url)
            print(f"thirdparty: {rel(dest)}: unchanged ({os.path.getsize(dest):,} bytes)")
            return 0
        was = os.path.exists(dest)
        os.replace(part, dest)
        record(dest, url)
        print(f"thirdparty: {rel(dest)}: {'UPDATED' if was else 'new'} ({os.path.getsize(dest):,} bytes, sha256 {new[:12]}...)")
        return 0
    finally:                                    # whatever happened, no .part is left behind
        if os.path.exists(part):
            os.remove(part)


def add(dest, note):
    dest = full(dest)
    if not os.path.isfile(dest):
        print(f"thirdparty: {rel(dest)}: no such file")
        return 1
    record(dest, note)
    print(f"thirdparty: {rel(dest)}: recorded ({os.path.getsize(dest):,} bytes)")
    return 0


def check():
    entries = read_manifest()
    bad = 0
    for path, (sha, day, origin) in sorted(entries.items()):
        full = os.path.join(ROOT, path)
        if not os.path.exists(full):
            print(f"thirdparty: MISSING {path} (from {origin})"); bad += 1
        elif sha256(full) != sha:
            print(f"thirdparty: CHANGED {path}: not the bytes fetched on {day} from {origin}"); bad += 1
    recorded = set(entries)
    for r, _, files in os.walk(TP):
        for f in files:
            p = rel(os.path.join(r, f))
            if p in recorded or f == ".DS_Store" or p in ("third-party/README.md", "third-party/SOURCES.txt"):
                continue
            print(f"thirdparty: UNRECORDED {p}: add it with thirdparty.py add"); bad += 1
    print(f"thirdparty: {len(entries)} files recorded, {bad} problem{'s' if bad != 1 else ''}")
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch"); f.add_argument("dest"); f.add_argument("url")
    a = sub.add_parser("add"); a.add_argument("dest"); a.add_argument("note")
    sub.add_parser("check")
    args = ap.parse_args()
    if args.cmd == "fetch":
        return fetch(args.dest, args.url)
    if args.cmd == "add":
        return add(args.dest, args.note)
    return check()


if __name__ == "__main__":
    sys.exit(main())
