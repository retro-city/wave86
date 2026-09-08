#!/usr/bin/env python3
"""waveserve - serve an eXoDOS collection to WAVE86 over plain HTTP/1.0.

    waveserve.py ~/Downloads/eXoDOS --port 8086

Finds game zips ("Title (Year).zip") under <root>/eXo/eXoDOS, and also
under the nested install root <root>/eXoDOS/ and the torrent's
<root>/Content/GameData/eXoDOS/. Each zip holds one 8.3-named folder of
plain game files. The per-game dosbox.conf in eXo/eXoDOS/!dos/<DIR>/
tells which program starts the game (and marks CD games: left out unless
--cd, which ships them without their CD images, since many only use the
CD for audio), and xml/all/MS-DOS.xml gives title, year, genre,
developer and notes. The index is rebuilt every --rescan seconds.

    GET /list          id|DIR|Title|year|genre|KB|EXE|CD|SRC  (CD = 1 when
                       the game needs a CD image; SRC = exodos or tdc)

A second root, --tdc DIR, adds a Total DOS Collection tree: <year>/<Title
(Year)(Publisher) [Genre]>/ folders of plain files. They get 8.3 folder
names made from the title and are keyed "tdc:DIR" so they never collide
with an eXoDOS folder of the same name.
    GET /info/<id>     a few lines about one game (<id> or DIR)
    GET /pack/<id>     the game's files: "F <bytes> <DOS path>\\n" + data
                       for each file, then "E\\n". Nothing to unzip on DOS.
"""
import os, re, sys, zipfile, argparse, unicodedata, threading, time, zlib, shutil, struct
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


# raw CD sector layouts a cue sheet can name: (skip, sector size)
SECTOR = {"MODE1/2048": (0, 2048), "MODE1/2352": (16, 2352),
          "MODE2/2352": (24, 2352), "MODE2/2336": (8, 2336)}


def parse_cue(text):
    """[[file, [[track, mode, first index in frames], ...]], ...]"""
    files = []
    for line in text.splitlines():
        w = line.split()
        if not w:
            continue
        key = w[0].upper()
        if key == "FILE":
            m = re.match(r'\s*FILE\s+"(.*)"\s+\S+\s*$', line, re.I)
            files.append([m.group(1) if m else w[1].strip('"'), []])
        elif key == "TRACK" and files and len(w) > 2:
            files[-1][1].append([int(w[1]), w[2].upper(), None])
        elif key == "INDEX" and files and files[-1][1] and len(w) > 2:
            try:
                mm, ss, ff = (int(x) for x in w[2].split(":"))
            except ValueError:
                continue
            fr = (mm * 60 + ss) * 75 + ff
            tr = files[-1][1][-1]
            if tr[2] is None or fr < tr[2]:
                tr[2] = fr
    return files


def iso_specs(z, cue_name, members):
    """The data tracks of a cue sheet inside a zip, as
    (bin member, skip, sector size, first sector, sectors): one ISO each.
    Audio tracks are dropped; the image is what a CD-ROM driver reads."""
    specs = []
    cue_dir = cue_name.rsplit("/", 1)[0] + "/" if "/" in cue_name else ""
    lower = {n.lower(): n for n in members}
    try:
        text = z.read(cue_name).decode("latin-1")
    except KeyError:
        return specs
    for fname, tracks in parse_cue(text):
        member = lower.get((cue_dir + fname).lower())
        if not member:
            continue
        size = z.getinfo(member).file_size
        for k, (no, mode, start) in enumerate(tracks):
            if mode not in SECTOR:
                continue
            skip, ssize = SECTOR[mode]
            start = start or 0
            end = tracks[k + 1][2] if k + 1 < len(tracks) and tracks[k + 1][2] is not None else size // ssize
            count = end - start
            # the image ends where its volume descriptor says, not at the
            # bin's post-gap, or SHSUCDHD warns about the size on every load
            try:
                with z.open(member) as f:
                    left = (start + 16) * ssize
                    while left > 0:
                        chunk = f.read(min(left, 1 << 20))
                        if not chunk:
                            break
                        left -= len(chunk)
                    pvd = f.read(ssize)[skip:skip + 2048]
                if pvd[1:6] == b"CD001":
                    vol = struct.unpack("<I", pvd[80:84])[0]
                    if 0 < vol < count:
                        count = vol
            except Exception:
                pass
            if count > 0:
                specs.append((member, skip, ssize, start, count))
            break                       # one data track per file is the rule
    return specs


def iso_name(cue_name, used):
    base = re.sub(r"[^A-Z0-9_-]", "", cue_name.rsplit("/", 1)[-1].rsplit(".", 1)[0].upper())[:8] or "DISC"
    name, n = base, 1
    while name in used:
        n += 1
        name = f"{base[:7]}{n}"
    used.add(name)
    return name


def imgmount_bat(iso, letter):
    """IMGMOUNT.BAT: the game's CD image on its drive letter, whichever DOS
    it finds itself on. DOS needs the WAVE86 folder on the PATH for the
    drivers; DOSBox has IMGMOUNT on its Z:, called by full path here
    because a bare IMGMOUNT would find this batch first and loop."""
    lines = ["@echo off",
             f"rem WAVE86: this game wants its CD image on {letter}:. waveserve made",
             "rem this from the eXoDOS dosbox.conf, and the start batch calls it first.",
             "if exist Z:\\IMGMOUNT.COM goto dosbox",
             "if exist Z:\\SYSTEM\\IMGMOUNT.COM goto dosboxx",
             f"SHCDHD86 /F:{iso} /Q",
             f"SHCDX86 /D:SHSU-CDH,{letter} /Q",
             "goto done",
             ":dosbox",
             f"Z:\\IMGMOUNT.COM {letter} {iso} -t iso",
             "goto done",
             ":dosboxx",
             f"Z:\\SYSTEM\\IMGMOUNT.COM {letter} {iso} -t iso",
             ":done"]
    return ("\r\n".join(lines) + "\r\n").encode("ascii")


def read_conf(root, short):
    """(candidate program names in autoexec order, CD drive letter or "")"""
    for r in roots_of(root):
        p = os.path.join(r, "eXo", "eXoDOS", "!dos", short, "dosbox.conf")
        if not os.path.isfile(p):
            continue
        cands, cd, inauto = [], "", False
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
                m = re.match(r"imgmount\s+([a-z])\b", low)
                cd = m.group(1).upper() if m else "D"
            word = low.split()[0] if low.split() else ""
            if word in SKIP_CMDS or word.endswith(":"):
                continue
            if word == "call" and len(low.split()) > 1:
                word = low.split()[1]
            word = word.rsplit(".", 1)[0] if "." in word else word
            if word and word not in cands:
                cands.append(word)
        return cands, cd
    return [], ""


TDC_RE = re.compile(r'^(?P<title>.*?)\s*\((?P<year>\d{4}|\d{3}x|\d{2}xx)\)\((?P<pub>[^)]*)\)\s*(?:\[(?P<genre>[^\]]*)\])?(?:\s*\[[^\]]*\])*\s*$')
PLACEHOLDER_AFTER = 1577836800          # 2020-01-01: no DOS game file is newer
TDC_TAGS = r'\[[^\]]*\]|\((?:Installer|Alt|Demo|Beta|Fix|demo|alt)[^)]*\)'


def short_name(title, full, used):
    """a stable 8.3 folder name for a TDC game: from the title, hash on clash"""
    base = re.sub(r'[^A-Z0-9]', '', title.upper())[:8] or "GAME"
    cand = base
    if cand in used and used[cand] != full:
        cand = base[:6] + "%02X" % (zlib.crc32(full.encode()) & 0xFF)
        if cand in used and used[cand] != full:
            cand = base[:4] + "%04X" % (zlib.crc32(full.encode()) & 0xFFFF)
    used[cand] = full
    return cand


def index_tdc(root, max_mb):
    """Total DOS Collection: <year>/<Title (Year)(Publisher) [Genre]>/files"""
    games, used = [], {}
    if not root or not os.path.isdir(root):
        return games
    for ydir in sorted(os.listdir(root)):
        yp = os.path.join(root, ydir)
        if not os.path.isdir(yp):
            continue
        for fn in sorted(os.listdir(yp)):
            gp = os.path.join(yp, fn)
            if not os.path.isdir(gp):
                continue
            m = TDC_RE.match(re.sub(r'\.img$', '', fn, flags=re.I))
            if m:
                title, year, pub, genre = m.group("title"), m.group("year"), m.group("pub"), m.group("genre") or ""
            else:
                title, year, pub, genre = fn, ydir, "", ""
            tags = re.findall(TDC_TAGS, title)
            clean = re.sub(TDC_TAGS, '', title).strip() or title
            files, total, partial = [], 0, False
            for r_, _, fs_ in os.walk(gp):
                for n in fs_:
                    fp = os.path.join(r_, n)
                    rel = dos_path(os.path.relpath(fp, gp).split(os.sep))
                    if rel is None:
                        continue
                    st = os.stat(fp)
                    # a torrent client leaves files it has not fetched yet as
                    # 0 bytes dated the day the torrent was added; real
                    # 0-byte game files carry their old date. Placeholders
                    # mark the game incomplete and stay out of the pack.
                    if st.st_size == 0 and st.st_mtime > PLACEHOLDER_AFTER:
                        partial = True
                        continue
                    files.append((rel, fp, st.st_size))
                    total += st.st_size
            if not files or total == 0 or total > max_mb * 1024 * 1024:
                continue                # nothing downloaded yet
            files.sort()
            d = short_name(clean, fn, used)
            if not year.isdigit():
                year = ydir if ydir.isdigit() else ""
            games.append(dict(title=ascii_text(clean), year=year, dir=d, zip=None, src="tdc",
                              kb=(total + 1023) // 1024, files=files, exe="", cd=False,
                              partial=partial,
                              genre=ascii_text(genre), developer=ascii_text(pub),
                              notes=ascii_text(" ".join(tags))))
    return games


def index(root, max_mb, include_cd):
    meta = read_xml(root) if root else {}
    seen, games = set(), []
    if not root:
        return games
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
            files, total, names, cues = [], 0, set(), []
            for i in infos:
                parts = i.filename.split("/")
                if parts[0] != top or len(parts) < 2 or parts[-1].lower().endswith(".exo"):
                    continue
                if parts[-1].lower().endswith(".cue"):
                    cues.append(i.filename)
                rel = dos_path(parts[1:])
                if rel is None:
                    continue
                if parts[1].upper() == "CD" or rel.rsplit(".", 1)[-1] in ("CUE", "BIN", "ISO", "IMG", "CCD", "SUB"):
                    continue          # raw images stay on the server; see below
                files.append((rel, i.filename, i.file_size))
                total += i.file_size
                names.add(parts[-1].upper())
            if not files or total > max_mb * 1024 * 1024:
                continue              # the limit is for the game, not its CD
            cands, cd = read_conf(root, top)
            if cd and not include_cd:
                continue
            # the CD, as one ISO per data track, made from the cue/bin while
            # streaming; the DOS side mounts CD\NAME.ISO when the game runs
            cd_kb, used, isos = 0, set(), []
            if include_cd:
                with zipfile.ZipFile(path) as z:
                    members = z.namelist()
                    for cue in sorted(cues):
                        for spec in iso_specs(z, cue, members):
                            size = spec[4] * 2048
                            isos.append(f"CD\\{iso_name(cue, used)}.ISO")
                            files.append((isos[-1], None, size, spec))
                            total += size
                            cd_kb += (size + 1023) // 1024
            exe_file = ""
            for exe in cands:           # first word that is a real program wins
                for ext in ("BAT", "EXE", "COM"):
                    cand = f"{exe.upper()}.{ext}"
                    if cand in names:
                        exe_file = cand
                        break
                if exe_file:
                    break
            # a CD game started by a batch gets IMGMOUNT.BAT and a call to it
            # at the top of that batch, so it also runs from a plain prompt
            if isos and exe_file.endswith(".BAT"):
                mount = imgmount_bat(isos[0], cd or "D")
                files.append(("IMGMOUNT.BAT", None, len(mount), mount))
                for k, entry in enumerate(files):
                    if entry[0].upper() == exe_file and entry[1]:
                        with zipfile.ZipFile(path) as z:
                            body = b"@call IMGMOUNT.BAT\r\n" + z.read(entry[1])
                        files[k] = (entry[0], None, len(body), body)
                        total += len(b"@call IMGMOUNT.BAT\r\n")
                        break
            cd = bool(cd_kb)
            md = meta.get(top.lower(), {})
            seen.add(fn)
            games.append(dict(title=ascii_text(m.group(1)), year=m.group(2), dir=top.upper(),
                              zip=path, src="exodos", kb=(total + 1023) // 1024, files=files, exe=exe_file,
                              cd=cd, cd_kb=cd_kb, genre=md.get("genre", ""), developer=md.get("developer", ""),
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
            body = "".join(f"{i}|{g['dir']}|{g['title'][:40]}|{g['year']}|{g['genre'][:14]}|{g['kb']}|{g['exe']}|{int(g['cd']) | (2 if g.get('partial') else 0)}|{g['src']}\r\n"
                           for i, g in enumerate(games)).encode("ascii", "replace")
            self._head("text/plain", len(body))
            self.wfile.write(body)
            return
        m = re.match(r'^/(info|pack)/([^/]+)$', p)
        g = None
        if m:
            key = m.group(2)
            if key.isdigit() and int(key) < len(games):
                g = games[int(key)]
            else:                       # "DIR" (eXoDOS) or "src:DIR"
                src, _, d = key.rpartition(":")
                src = src or "exodos"
                g = next((x for x in games if x["dir"] == d.upper() and x["src"] == src), None)
        if g is None:
            self.send_error(404)
            return
        if m.group(1) == "info":
            lines = [f"{g['title']} ({g['year']})" if g['year'] else g['title'], f"DIR {g['dir']}", f"SRC {g['src']}", f"EXE {g['exe'] or '?'}",
                     f"GENRE {g['genre']}", f"BY {g['developer']}", f"FILES {len(g['files'])}",
                     f"KB {g['kb']}", f"CD {str(g.get('cd_kb', 0) // 1024) + ' MB image' if g['cd'] else 'no'}",
                     f"PARTIAL {'yes' if g.get('partial') else 'no'}", ""]
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
        if g["zip"]:
            with zipfile.ZipFile(g["zip"]) as z:
                for entry in g["files"]:
                    rel, name, size = entry[:3]
                    self.wfile.write(f"F {size} {rel}\n".encode())
                    if len(entry) == 4:
                        if isinstance(entry[3], bytes):
                            self.wfile.write(entry[3])
                        else:
                            self.send_iso(z, entry[3])
                        continue
                    with z.open(name) as f:
                        shutil.copyfileobj(f, self.wfile, 65536)
        else:                           # plain files on disk (TDC)
            for rel, fp, size in g["files"]:
                self.wfile.write(f"F {size} {rel}\n".encode())
                with open(fp, "rb") as f:
                    shutil.copyfileobj(f, self.wfile, 65536)
        self.wfile.write(b"E\n")

    def send_iso(self, z, spec):
        """stream the 2048-byte payload of each sector of a data track"""
        member, skip, ssize, start, count = spec
        buf = bytearray()
        with z.open(member) as f:
            left = start * ssize             # zip streams cannot seek
            while left > 0:
                chunk = f.read(min(left, 1 << 20))
                if not chunk:
                    return
                left -= len(chunk)
            for _ in range(count):
                s = f.read(ssize)
                if len(s) < ssize:
                    buf += bytes(2048 * (count - _))   # short bin: pad
                    break
                buf += s[skip:skip + 2048]
                if len(buf) >= 65536:
                    self.wfile.write(buf)
                    buf = bytearray()
        if buf:
            self.wfile.write(buf)

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.client_address[0], fmt % args))


def main():
    global GAMES
    ap = argparse.ArgumentParser()
    ap.add_argument("root", nargs="?", help="eXoDOS folder")
    ap.add_argument("--tdc", help="Total DOS Collection folder (the <year> folders)")
    ap.add_argument("--port", type=int, default=8086)
    ap.add_argument("--max-mb", type=int, default=30)
    ap.add_argument("--cd", action="store_true", help="include games that need a CD image")
    ap.add_argument("--rescan", type=int, default=300, help="seconds between re-indexing")
    a = ap.parse_args()

    if not a.root and not a.tdc:
        ap.error("give an eXoDOS folder and/or --tdc DIR")

    def reindex(first=False):
        global GAMES
        games = index(a.root, a.max_mb, a.cd) + index_tdc(a.tdc, a.max_mb)
        games.sort(key=lambda g: g["title"].lower())
        with LOCK:
            changed = [g["title"] for g in games] != [g["title"] for g in GAMES]
            GAMES = games
        if first or changed:
            print(f"waveserve: {len(games)} games ({sum(1 for g in games if g['src']=='exodos')} eXoDOS, "
                  f"{sum(1 for g in games if g['src']=='tdc')} TDC), port {a.port}", flush=True)

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
