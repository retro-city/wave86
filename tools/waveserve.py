#!/usr/bin/env python3
"""waveserve - serve an eXoDOS collection to WAVE86 over plain HTTP/1.0.

    waveserve.py ~/Downloads/eXoDOS --port 8086

Finds game zips ("Title (Year).zip") under <root>/eXo/eXoDOS, and also
under the nested install root <root>/eXoDOS/ and the torrent's
<root>/Content/GameData/eXoDOS/. Each zip holds one 8.3-named folder of
plain game files. The per-game dosbox.conf in eXo/eXoDOS/!dos/<DIR>/
tells which program starts the game (and marks CD games, which are left
out unless --cd), and xml/all/MS-DOS.xml gives title, year, genre,
developer and notes. The index is rebuilt every --rescan seconds.

    GET /list          id|DIR|Title|year|genre|KB|EXE  (one game per line)
    GET /info/<id>     a few lines about one game
    GET /pack/<id>     the game's files: "F <bytes> <DOS path>\\n" + data
                       for each file, then "E\\n". Nothing to unzip on DOS.
"""
import os, re, sys, zipfile, argparse, unicodedata, threading, time
import xml.etree.ElementTree as ET
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

GAMES = []
LOCK = threading.Lock()
DOSNAME = re.compile(r'^[A-Z0-9_$~!#%&\-@^\'`(){}]{1,8}(\.[A-Z0-9_$~!#%&\-@^\'`(){}]{1,3})?$')


def ascii_text(s):
    s = unicodedata.normalize("NFKD", s).encode("ascii", "ignore").decode()
    return re.sub(r'[|\r\n]+', ' ', s).strip()


def dos_path(parts):
    out = []
    for p in parts:
        p = p.upper()
        if not DOSNAME.match(p):
            return None
        out.append(p)
    return "\\".join(out)


def roots_of(root):
    """the install roots that may hold eXo/eXoDOS, nearest first"""
    cands = [root, os.path.join(root, "eXoDOS")]
    return [c for c in cands if os.path.isdir(os.path.join(c, "eXo", "eXoDOS"))]


def zip_dirs(root):
    dirs = [os.path.join(r, "eXo", "eXoDOS") for r in roots_of(root)]
    gd = os.path.join(root, "Content", "GameData", "eXoDOS")
    if os.path.isdir(gd):
        dirs.append(gd)
    return dirs


def read_xml(root):
    """short folder name (lower) -> dict(genre, year, developer, notes)"""
    meta = {}
    path = None
    for r in roots_of(root) + [root]:
        p = os.path.join(r, "xml", "all", "MS-DOS.xml")
        if os.path.isfile(p):
            path = p
            break
    src = None
    if path:
        src = open(path, "rb")
    else:
        z = os.path.join(root, "Content", "XODOSMetadata.zip")
        if os.path.isfile(z):
            try:
                src = zipfile.ZipFile(z).open("xml/all/MS-DOS.xml")
            except (KeyError, zipfile.BadZipFile):
                src = None
    if not src:
        return meta
    for _, el in ET.iterparse(src):
        if el.tag != "Game":
            continue
        rf = (el.findtext("RootFolder") or "").replace("/", "\\").rstrip("\\")
        short = rf.rsplit("\\", 1)[-1].lower()
        if short:
            meta[short] = dict(
                genre=ascii_text(el.findtext("Genre") or ""),
                year=(el.findtext("ReleaseDate") or "")[:4],
                developer=ascii_text(el.findtext("Developer") or ""),
                notes=ascii_text(el.findtext("Notes") or ""))
        el.clear()
    return meta


SKIP_CMDS = {"cls", "exit", "mount", "imgmount", "cd", "c:", "d:", "rem", "echo", "pause", "loadfix", "cycles", "config", "keyb"}


def read_conf(root, short):
    """(exe base name or "", needs_cd) from the game's dosbox.conf autoexec"""
    for r in roots_of(root):
        p = os.path.join(r, "eXo", "eXoDOS", "!dos", short, "dosbox.conf")
        if not os.path.isfile(p):
            continue
        exe, cd, inauto = "", False, False
        for line in open(p, "r", encoding="latin-1", errors="replace"):
            t = line.strip()
            if t.lower().startswith("[autoexec]"):
                inauto = True
                continue
            if not inauto or not t or t.startswith("#"):
                continue
            t = t.lstrip("@")
            low = t.lower()
            if low.startswith("imgmount"):
                cd = True
            word = low.split()[0] if low.split() else ""
            if word in SKIP_CMDS or word.endswith(":"):
                continue
            if word == "call" and len(low.split()) > 1:
                word = low.split()[1]
            if not exe:
                exe = word
        return exe, cd
    return "", False


def index(root, max_mb, include_cd):
    meta = read_xml(root)
    seen, games = set(), []
    for zdir in zip_dirs(root):
        for fn in sorted(os.listdir(zdir)):
            m = re.match(r'^(.*) \((\d{4})\)\.zip$', fn)
            if not m or fn in seen:
                continue
            path = os.path.join(zdir, fn)
            try:
                with zipfile.ZipFile(path) as z:
                    infos = [i for i in z.infolist() if not i.is_dir()]
            except zipfile.BadZipFile:
                continue
            if not infos:
                continue
            top = infos[0].filename.split("/")[0]
            files, total, names = [], 0, set()
            for i in infos:
                parts = i.filename.split("/")
                if parts[0] != top or len(parts) < 2 or parts[-1].lower().endswith(".exo"):
                    continue
                rel = dos_path(parts[1:])
                if rel is None:
                    continue
                files.append((rel, i.filename, i.file_size))
                total += i.file_size
                names.add(parts[-1].upper())
            if not files or total > max_mb * 1024 * 1024:
                continue
            exe, cd = read_conf(root, top)
            if cd and not include_cd:
                continue
            exe_file = ""
            if exe:
                for ext in ("BAT", "EXE", "COM"):
                    cand = f"{exe.upper()}.{ext}"
                    if cand in names:
                        exe_file = cand
                        break
            md = meta.get(top.lower(), {})
            seen.add(fn)
            games.append(dict(title=ascii_text(m.group(1)), year=m.group(2), dir=top.upper(),
                              zip=path, kb=(total + 1023) // 1024, files=files, exe=exe_file,
                              cd=cd, genre=md.get("genre", ""), developer=md.get("developer", ""),
                              notes=md.get("notes", "")))
    games.sort(key=lambda g: g["title"].lower())
    return games


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def _head(self, ctype, length=None):
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        if length is not None:
            self.send_header("Content-Length", str(length))
        self.send_header("Connection", "close")
        self.end_headers()

    def do_GET(self):
        with LOCK:
            games = GAMES
        p = self.path
        if p == "/list":
            body = "".join(f"{i}|{g['dir']}|{g['title'][:40]}|{g['year']}|{g['genre'][:14]}|{g['kb']}|{g['exe']}\r\n"
                           for i, g in enumerate(games)).encode("ascii", "replace")
            self._head("text/plain", len(body))
            self.wfile.write(body)
            return
        m = re.match(r'^/(info|pack)/(\d+)$', p)
        if not m or int(m.group(2)) >= len(games):
            self.send_error(404)
            return
        g = games[int(m.group(2))]
        if m.group(1) == "info":
            lines = [f"{g['title']} ({g['year']})", f"DIR {g['dir']}", f"EXE {g['exe'] or '?'}",
                     f"GENRE {g['genre']}", f"BY {g['developer']}", f"FILES {len(g['files'])}",
                     f"KB {g['kb']}", f"CD {'yes' if g['cd'] else 'no'}", ""]
            notes = g["notes"]
            while notes:
                cut = notes.rfind(" ", 0, 76) if len(notes) > 76 else len(notes)
                if cut <= 0:
                    cut = 76
                lines.append(notes[:cut].strip())
                notes = notes[cut:].strip()
            body = ("\r\n".join(lines) + "\r\n").encode("ascii", "replace")
            self._head("text/plain", len(body))
            self.wfile.write(body)
            return
        self._head("application/octet-stream")
        with zipfile.ZipFile(g["zip"]) as z:
            for rel, name, size in g["files"]:
                self.wfile.write(f"F {size} {rel}\n".encode())
                with z.open(name) as f:
                    while True:
                        chunk = f.read(65536)
                        if not chunk:
                            break
                        self.wfile.write(chunk)
        self.wfile.write(b"E\n")

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.client_address[0], fmt % args))


def main():
    global GAMES
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("--port", type=int, default=8086)
    ap.add_argument("--max-mb", type=int, default=30)
    ap.add_argument("--cd", action="store_true", help="include games that need a CD image")
    ap.add_argument("--rescan", type=int, default=300, help="seconds between re-indexing")
    a = ap.parse_args()

    def reindex(first=False):
        global GAMES
        games = index(a.root, a.max_mb, a.cd)
        with LOCK:
            changed = [g["title"] for g in games] != [g["title"] for g in GAMES]
            GAMES = games
        if first or changed:
            print(f"waveserve: {len(games)} games from {a.root} "
                  f"({sum(1 for g in games if g['exe'])} with a known exe), port {a.port}", flush=True)

    reindex(first=True)

    def loop():
        while True:
            time.sleep(a.rescan)
            try:
                reindex()
            except Exception as e:      # keep serving on a bad scan
                print("waveserve: rescan failed:", e, flush=True)
    threading.Thread(target=loop, daemon=True).start()
    ThreadingHTTPServer(("0.0.0.0", a.port), H).serve_forever()


if __name__ == "__main__":
    main()
