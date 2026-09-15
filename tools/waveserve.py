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
    GET /update        the same stream, holding WAVE86 itself: the programs
                       from build/ plus anything in --update DIR, so a DOS
                       machine can fetch a fresh build (WAVEGET UPDATE, or
                       U in the network view) instead of a floppy.
"""
import os, re, sys, zipfile, argparse, unicodedata, threading, time, zlib, shutil, struct
import xml.etree.ElementTree as ET
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fat16, tempfile

NETDRIVE_DIR = None
NETDRIVE_PORT = 2002
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


def iso_name(base, used):
    """the game's folder name, then NAME2, NAME3 for more discs"""
    base = re.sub(r"[^A-Z0-9_-]", "", base.upper())[:8] or "DISC"
    name, n = base, 1
    while name in used:
        n += 1
        name = f"{base[:7]}{n}"
    used.add(name)
    return name


def imgmount_bat(iso, letter, net=None):
    """IMGMOUNT.BAT: the game's CD image as a CD-ROM drive, whichever DOS it
    finds itself on, and the letter it got in CD (the start batch is
    rewritten to use %CD% where the eXoDOS conf said D:). Called again
    with /U it takes everything down. DOS needs the WAVE86 folder on the
    PATH for the drivers; DOSBox has IMGMOUNT on its Z:, called by full path
    since a bare IMGMOUNT would find this batch first and loop.

    net = (server:port, image) keeps the ISO on the server: mTCP NetDrive
    attaches the volume holding it as a drive (%WAVEND%, D: unless set)
    and SHSUCDHD reads the ISO from there, so the disc lands on the next
    free letter, E: normally."""
    lines = ["@echo off", 'if "%1"=="/U" goto unmount',
             f"rem WAVE86: this game wants its CD image ({letter}: in the eXoDOS conf).",
             "rem waveserve made this; the start batch calls it first and the launcher",
             "rem calls it with /U afterwards. CD gets the letter the disc is on.",
             "rem WAVECDROM: where the images are (default the game's CD folder);",
             "rem IMGMOUNT: SOFTWARE (SHSUCDHD+SHSUCDX) or a card with its own",
             "rem CD-ROM emulation (PICOGUS, PICOMEM): %WAVECDCMD% loads the image",
             "rem and the disc turns up on %WAVECDL%.",
             'if "%WAVECDROM%"=="" set WAVECDROM=CD']
    if net:
        srv, img = net
        lines += [f'if "%WAVENDSRV%"=="" set WAVENDSRV={srv}',
                  "if exist Z:\\IMGMOUNT.COM goto nodosbox",
                  "if exist Z:\\SYSTEM\\IMGMOUNT.COM goto nodosbox",
                  "rem NetDrive's letter: whatever WAVEND says if that really is one,",
                  "rem else the first of D: to H: that NETDRIVE STATUS accepts",
                  'if "%WAVEND%"=="" goto ndfind',
                  "NETDRIVE STATUS %WAVEND%: > NUL",
                  "if not errorlevel 1 goto ndok",
                  "set WAVEND=",
                  ":ndfind"]
        for l in "DEFGH":
            lines += [f'if "%WAVEND%"=="" NETDRIVE STATUS {l}: > NUL',
                      f'if "%WAVEND%"=="" if not errorlevel 1 set WAVEND={l}']
        lines += ['if not "%WAVEND%"=="" goto ndok',
                  "echo No NetDrive letter found: is NETDRIVE.SYS in CONFIG.SYS?",
                  "goto done",
                  ":ndok",
                  f"NETDRIVE C %WAVENDSRV% {img} %WAVEND%: -ro",
                  "if errorlevel 1 goto ndfail",
                  f"LH SHCDHD86 /F:%WAVEND%:\\{iso} /Q",
                  f"LH SHCDX86 /D:SHSU-CDH,{letter} /I /Q",
                  "goto letter",
                  ":ndfail",
                  "echo NetDrive could not attach the disc from %WAVENDSRV% on %WAVEND%:.",
                  "goto done",
                  ":nodosbox",
                  "echo This game's CD stays on the server (mTCP NetDrive), which DOSBox's",
                  "echo own shell cannot reach. Run it under a real DOS: make dosrun.",
                  "goto done"]
    else:
        img = iso.split("\\")[-1]
        lines += ["if exist Z:\\IMGMOUNT.COM goto dosbox",
                  "if exist Z:\\SYSTEM\\IMGMOUNT.COM goto dosboxx",
                  'if "%IMGMOUNT%"=="" goto software',
                  'if "%IMGMOUNT%"=="SOFTWARE" goto software',
                  "goto card",
                  ":software"]
        if img.upper().endswith(".CUE"):
            lines += [f"echo {img} is a cue sheet, which keeps the CD audio, and SHSUCDHD",
                      "echo mounts plain ISO images only. Set imgmount= to a card that",
                      "echo takes cue/bin, or install the game again in software mode.",
                      "goto done"]
        else:
            lines += [f"LH SHCDHD86 /F:%WAVECDROM%\\{img} /Q",
                      f"LH SHCDX86 /D:SHSU-CDH,{letter} /I /Q",
                      "goto letter"]
        lines += [
                  ":card",
                  "rem the card loads the image itself; WAVECDN=1 gives its command",
                  "rem the bare file name instead of the whole path. The test comes",
                  "rem first: a shell handed an empty %WAVECDCMD% would otherwise be",
                  "rem asked to run an IF with no command at all, and say so.",
                  'if "%WAVECDCMD%"=="" goto cardl',
                  'if "%WAVECDN%"=="1" goto cardname',
                  f"%WAVECDCMD% %WAVECDROM%\\{img}",
                  "goto cardl",
                  ":cardname",
                  f"%WAVECDCMD% {img}",
                  ":cardl",
                  'if "%WAVECDL%"=="" set WAVECDL=D',
                  "set CD=%WAVECDL%",
                  "goto done",
                  ":dosbox",
                  f"Z:\\IMGMOUNT.COM {letter} %WAVECDROM%\\{img} -t iso",
                  f"set CD={letter}",
                  "goto done",
                  ":dosboxx",
                  f"Z:\\SYSTEM\\IMGMOUNT.COM {letter} %WAVECDROM%\\{img} -t iso",
                  f"set CD={letter}",
                  "goto done"]
    # SHSUCDX /L:1 returns the first drive's number (A: = 1) as the errorlevel
    lines += [":letter", "SHCDX86 /L:1 /QQ"]
    lines += [f"if errorlevel {n} set CD={chr(64 + n)}" for n in range(3, 27)]
    lines += ["if errorlevel 27 set CD=",
              f'if "%CD%"=="" set CD={letter}',
              "goto done",
              ":unmount"]
    if net:
        lines += ["SHCDX86 /U /Q", "SHCDHD86 /U /Q", "NETDRIVE D %WAVEND%:"]
    else:
        lines += ["if exist Z:\\IMGMOUNT.COM goto unsoft",
                  "if exist Z:\\SYSTEM\\IMGMOUNT.COM goto unsoft",
                  'if "%IMGMOUNT%"=="" goto unsoft',
                  'if "%IMGMOUNT%"=="SOFTWARE" goto unsoft',
                  'if "%WAVECDCMDU%"=="" goto uncard',
                  "%WAVECDCMDU%",
                  ":uncard",
                  "set CD=",
                  "goto done",
                  ":unsoft",
                  f"if exist Z:\\IMGMOUNT.COM Z:\\IMGMOUNT.COM -u {letter}",
                  f"if exist Z:\\SYSTEM\\IMGMOUNT.COM Z:\\SYSTEM\\IMGMOUNT.COM -u {letter}",
                  "if exist Z:\\IMGMOUNT.COM goto done",
                  "if exist Z:\\SYSTEM\\IMGMOUNT.COM goto done",
                  "SHCDX86 /U /Q", "SHCDHD86 /U /Q"]
    lines += ["set CD=", ":done"]
    return ("\r\n".join(lines) + "\r\n").encode("ascii")


def start_batch(text, prefix, letter):
    """The game's start batch with the IMGMOUNT.BAT call first and the CD
    letter of the eXoDOS conf replaced by %CD%, which IMGMOUNT.BAT sets
    to wherever the disc actually landed (E: when NetDrive holds D:, or a
    real CD-ROM does)."""
    pat = re.compile(r"(?<![A-Za-z0-9_\\/:.%\-])" + re.escape(letter) + r":", re.I)
    return prefix + pat.sub("%CD%:", text)


def iso_chunks(z, spec):
    """The 2048-byte payloads of a data track, 64 KB at a time."""
    member, skip, ssize, start, count = spec
    buf = bytearray()
    with z.open(member) as f:
        left = start * ssize                 # zip streams cannot seek
        while left > 0:
            chunk = f.read(min(left, 1 << 20))
            if not chunk:
                return
            left -= len(chunk)
        for i in range(count):
            s = f.read(ssize)
            if len(s) < ssize:
                buf += bytes(2048 * (count - i))     # short bin: pad
                break
            buf += s[skip:skip + 2048]
            if len(buf) >= 65536:
                yield bytes(buf)
                buf = bytearray()
    if buf:
        yield bytes(buf)


def netdrive_image(path, zippath, spec, iso_name):
    """A NetDrive volume holding one ISO, built if it is not there yet."""
    size = spec[4] * 2048
    if os.path.exists(path) and os.path.getsize(path) >= size + 512:
        return
    mb = -(-(size + 2 * 256 * 512 + 32 * 512 + 512) // 1048576) + 1
    tmp = path + ".part"
    fat16.format_volume(tmp, max(3, mb))
    with zipfile.ZipFile(zippath) as z, fat16.Volume(tmp) as v:
        v.add_file(iso_name, size, iso_chunks(z, spec))
    os.replace(tmp, path)
    print(f"waveserve: NetDrive image {os.path.basename(path)} ({size // 1048576} MB)", file=sys.stderr)


def game_disk(g):
    """A NetDrive volume holding the whole game as the LOCAL pack would land
    it (files, generated batches, the disc as CD\\NAME.ISO), built on first
    request; session scoped, so every player writes into a private journal
    and the master stays clean. Returns the image name."""
    name = f"{g['dir']}.DSK"
    path = os.path.join(NETDRIVE_DIR, name)
    if not os.path.exists(path):
        with tempfile.TemporaryDirectory(dir=NETDRIVE_DIR) as work:
            tree = os.path.join(work, "tree")
            os.makedirs(tree)
            with zipfile.ZipFile(g["zip"]) if g["zip"] else open(os.devnull) as z:
                for entry in g["files"]:
                    rel, member, size = entry[:3]
                    out = os.path.join(tree, *rel.split("\\"))
                    os.makedirs(os.path.dirname(out), exist_ok=True)
                    with open(out, "wb") as o:
                        if len(entry) == 4 and isinstance(entry[3], bytes):
                            o.write(entry[3])
                        elif len(entry) == 4:
                            for chunk in iso_chunks(z, entry[3]):
                                o.write(chunk)
                        elif g["zip"]:
                            with z.open(member) as f:
                                shutil.copyfileobj(f, o, 65536)
                        else:
                            with open(member, "rb") as f:
                                shutil.copyfileobj(f, o, 65536)
            mb, nroot = fat16.volume_size_mb(tree)
            tmp = path + ".part"
            try:
                fat16.format_volume(tmp, mb, nroot)
                fat16.fill_tree(tmp, 0, tree)
                os.replace(tmp, path)
            finally:
                if os.path.exists(tmp):
                    os.remove(tmp)
        print(f"waveserve: game disk {name} ({mb} MB)", file=sys.stderr)
    open(path + ".session_scoped", "a").close()
    return name


def read_conf(root, short):
    """(candidate program names in autoexec order, CD drive letter or "")"""
    for r in roots_of(root):
        p = os.path.join(r, "eXo", "eXoDOS", "!dos", short, "dosbox.conf")
        if not os.path.isfile(p):
            continue
        cands, cd, inauto, cds = [], "", False, []
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
            if word == "cd" and len(low.split()) > 1:
                cds.append(t.split(None, 1)[1].strip().strip('"').replace("/", "\\"))
            if word in SKIP_CMDS or word.endswith(":"):
                continue
            if word == "call" and len(low.split()) > 1:
                word = low.split()[1]
            word = word.rsplit(".", 1)[0] if "." in word else word
            if word and word not in cands:
                cands.append(word)
        return cands, cd, "\\".join(c for c in cds if c not in ("\\", "..", "."))
    return [], "", ""


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


def raw_disc_entries(z, top, cues, raw_isos, members):
    """The game's discs as the collection has them, for a card that mounts
    cue/bin itself (a PicoGUS plays the audio tracks the ISO conversion
    throws away). Names are cut to 8.3 after the game folder - the cue's
    own FILE names are long and full of spaces - and each cue is rewritten
    to point at its renamed image. One entry list: the cue sheets as bytes,
    the images streamed from the zip as they are."""
    out, used = [], set()
    lower = {n.lower(): n for n in members}
    for cue in cues:
        try:
            text = z.read(cue).decode("latin-1")
        except KeyError:
            continue
        cue_dir = cue.rsplit("/", 1)[0] + "/" if "/" in cue else ""
        base = iso_name(top, used)
        images, lines = [], []
        for line in text.splitlines():
            m = re.match(r'(\s*FILE\s+)"(.*)"(\s+\S+\s*)$', line, re.I)
            if m:
                member = lower.get((cue_dir + m.group(2)).lower())
                if not member:
                    images = None           # a cue that points at nothing: skip it
                    break
                # one image: NAME.BIN; more: the first six letters and a number
                name = f"{base}.BIN" if not images else f"{base[:6]}{len(images) + 1:02d}.BIN"
                images.append((f"CD\\{name}", member, z.getinfo(member).file_size))
                line = f'{m.group(1)}"{name}"{m.group(3)}'
            lines.append(line.rstrip())
        if not images:
            continue
        sheet = ("\r\n".join(lines) + "\r\n").encode("latin-1")
        out.append((f"CD\\{base}.CUE", None, len(sheet), sheet))
        out += images
    for member, size in raw_isos:           # plain ISOs need no conversion anyway
        out.append((f"CD\\{iso_name(top, used)}.ISO", member, size))
    return out


def disc_last(files):
    """The disc image goes at the end of a pack. It dwarfs the rest, so a
    transfer that stops part way then leaves a complete game folder and
    only the disc to fetch again, and the batches that mount it are on
    disk before there is anything to mount."""
    return sorted(files, key=lambda e: 1 if e[0].upper().startswith("CD\\") else 0)


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
            files = disc_last(files)
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
            files, total, names, cues, raw_isos = [], 0, set(), [], []
            for i in infos:
                parts = i.filename.split("/")
                if parts[0] != top or len(parts) < 2 or parts[-1].lower().endswith(".exo"):
                    continue
                if parts[-1].lower().endswith(".cue"):
                    cues.append(i.filename)
                elif parts[-1].lower().endswith(".iso"):
                    raw_isos.append((i.filename, i.file_size))
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
            cands, cd, subdir = read_conf(root, top)
            if cd and not include_cd:
                continue
            # the CD, as one ISO per data track, made from the cue/bin while
            # streaming; the DOS side mounts CD\NAME.ISO when the game runs
            cd_kb, used, isos = 0, set(), []
            raw_discs, raw_kb = [], 0           # the discs as they came: cue/bin with their audio
            if include_cd and cd:               # the conf mounts a disc: cue/bin pairs, or plain ISOs
                with zipfile.ZipFile(path) as z:
                    members = z.namelist()
                    specs = [s for cue in sorted(cues) for s in iso_specs(z, cue, members)]
                    specs += [(name, 0, 2048, 0, size // 2048) for name, size in sorted(raw_isos)]
                    for spec in specs:
                        size = spec[4] * 2048
                        isos.append(f"CD\\{iso_name(top, used)}.ISO")
                        files.append((isos[-1], None, size, spec))
                        total += size
                        cd_kb += (size + 1023) // 1024
                    raw_discs = raw_disc_entries(z, top, sorted(cues), sorted(raw_isos), members)
                    raw_kb = sum((e[2] + 1023) // 1024 for e in raw_discs)
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
            # at the top of that batch, so it also runs from a plain prompt.
            # With NetDrive the ISO stays here in a volume the server hands
            # out; the pack then carries no image at all.
            net = None
            if isos and NETDRIVE_DIR:
                img = f"{top.upper()}.IMG"
                spec = next(e[3] for e in files if e[0] == isos[0])
                try:
                    netdrive_image(os.path.join(NETDRIVE_DIR, img), path, spec, isos[0].split("\\")[-1])
                    net = ("%NDSRV%", img)
                except Exception as e:          # one bad disc must not take the server down
                    print(f"waveserve: no NetDrive image for {top}: {e}", file=sys.stderr)
            # where the start program really is: the conf's cd chain, else the
            # first place a file of that name turns up
            exe_rel = ""
            if exe_file:
                rels = [e[0] for e in files if e[0].upper().split("\\")[-1] == exe_file]
                want = (subdir.upper() + "\\" + exe_file).lstrip("\\") if subdir else exe_file
                exe_rel = next((r for r in rels if r.upper() == want), rels[0] if rels else "")
            exe_dir = exe_rel.rsplit("\\", 1)[0] if "\\" in exe_rel else ""
            if isos:                            # the local variant; the net one is swapped in below
                mount = imgmount_bat(isos[0], cd or "D", None)
                files.append(("IMGMOUNT.BAT", None, len(mount), mount))
            is_bat = exe_file.endswith(".BAT")
            if exe_file and (exe_dir or (isos and not is_bat)):
                # a wrapper at the root: mount, step into the folder, run, step out
                roots = {e[0].upper() for e in files if "\\" not in e[0]}
                wrapper = "START.BAT" if "START.BAT" not in roots else "WAVE86.BAT"
                lines = ["@echo off"] + (["@call IMGMOUNT.BAT"] if isos else []) + \
                        ([f"cd {exe_dir}"] if exe_dir else []) + \
                        [("call " if is_bat else "") + exe_file] + \
                        ([f"cd {'..' if exe_dir.count(chr(92)) == 0 else chr(92).join(['..'] * (exe_dir.count(chr(92)) + 1))}"] if exe_dir else [])
                body = ("\r\n".join(lines) + "\r\n").encode("ascii")
                files.append((wrapper, None, len(body), body))
                total += len(body)
                if isos and is_bat:                 # the inner batch still needs %CD%
                    for k, entry in enumerate(files):
                        if entry[0] == exe_rel and entry[1]:
                            with zipfile.ZipFile(path) as z:
                                inner = start_batch(z.read(entry[1]).decode("cp437"), "", cd or "D").encode("cp437")
                            files[k] = (entry[0], None, len(inner), inner)
                            total += len(inner) - entry[2]
                            break
                exe_file = wrapper
            elif isos and is_bat:
                for k, entry in enumerate(files):
                    if entry[0].upper() == exe_file and entry[1]:
                        with zipfile.ZipFile(path) as z:
                            body = start_batch(z.read(entry[1]).decode("cp437"), "@call IMGMOUNT.BAT\r\n",
                                               cd or "D").encode("cp437")
                        files[k] = (entry[0], None, len(body), body)
                        total += len(body) - entry[2]
                        break
            cd_letter = cd or "D"
            cd = bool(cd_kb)
            netcd = bool(net)
            files_net = None
            if net:                             # same pack without the disc, mounting over NetDrive
                mount = imgmount_bat(isos[0].split("\\")[-1], cd_letter, net)
                files_net = [("IMGMOUNT.BAT", None, len(mount), mount) if e[0] == "IMGMOUNT.BAT" else e
                             for e in files if e[0] not in isos]
            files_raw = None
            if raw_discs:                       # same pack with the discs untouched, for a card
                mount = imgmount_bat(raw_discs[0][0], cd_letter, None)
                files_raw = [e for e in files if e[0] not in isos and e[0] != "IMGMOUNT.BAT"]
                files_raw += [("IMGMOUNT.BAT", None, len(mount), mount)] + raw_discs
                files_raw = disc_last(files_raw)
            files = disc_last(files)            # the disc after everything else
            if files_net:
                files_net = disc_last(files_net)
            md = meta.get(top.lower(), {})
            seen.add(fn)
            games.append(dict(title=ascii_text(m.group(1)), year=m.group(2), dir=top.upper(),
                              zip=path, src="exodos", kb=(total + 1023) // 1024, files=files, exe=exe_file,
                              cd=cd, cd_kb=cd_kb, netcd=netcd, files_net=files_net,
                              files_raw=files_raw, raw_kb=raw_kb, genre=md.get("genre", ""), developer=md.get("developer", ""),
                              notes=md.get("notes", "")))
    games.sort(key=lambda g: g["title"].lower())
    return games


HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UPDATE_NAMES = ["WAVE86.EXE", "WAVEGET.EXE", "DRVOFF.EXE"]
UPDATE_DIR = None


def update_set():
    """What a client gets from /update: the freshly built programs in build/,
    plus everything under --update DIR, which wins on a name clash (that is
    where you put a WAVE86.INI or anything else you want pushed out).

    Deliberately not in the default list: WAVE.BAT, because COMMAND.COM is
    reading it line by line while the update runs; MTCP.CFG, because DHCP
    keeps the lease in it; WAVE86.INI, because it is the machine's own
    settings and the launcher writes to it."""
    out = {}
    for n in UPDATE_NAMES:
        fp = os.path.join(HERE, "build", n)
        if os.path.exists(fp):
            out[n] = fp
    if UPDATE_DIR:
        for r, _, fs in os.walk(UPDATE_DIR):
            for fn in sorted(fs):
                if fn.startswith("."):
                    continue
                rel = os.path.relpath(os.path.join(r, fn), UPDATE_DIR)
                out[rel.replace("/", "\\").upper()] = os.path.join(r, fn)
    return sorted(out.items())


class CrcWriter:
    """Passes bytes through to the socket and keeps their CRC32, so each
    file in a pack can be followed by the number the client should have
    arrived at. Same polynomial as zlib and as WAVEGET's table."""

    def __init__(self, w):
        self.w, self.crc = w, 0

    def write(self, b):
        self.crc = zlib.crc32(b, self.crc)
        return self.w.write(b)


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
        if p.startswith("/update"):  # WAVEGET UPDATE: the programs themselves
            files = update_set()
            crc = p.endswith("crc=1")
            print("waveserve: update ->", ", ".join(n for n, _ in files), file=sys.stderr)
            self._head("application/octet-stream")
            for name, fp in files:
                self.wfile.write(f"F {os.path.getsize(fp)} {name}\n".encode())
                cw = CrcWriter(self.wfile)
                with open(fp, "rb") as f:
                    shutil.copyfileobj(f, cw, 65536)
                if crc:
                    self.wfile.write(f"C {cw.crc & 0xFFFFFFFF}\n".encode())
            self.wfile.write(b"E\n")
            return
        if p == "/list":
            body = "".join(f"{i}|{g['dir']}|{g['title'][:40]}|{g['year']}|{g['genre'][:14]}|{g['kb']}|{g['exe']}|{int(g['cd']) | (2 if g.get('partial') else 0) | (4 if g.get('netcd') else 0) | (8 if NETDRIVE_DIR else 0)}|{g['src']}|{g.get('cd_kb', 0)}|{g.get('raw_kb', 0)}\r\n"
                           for i, g in enumerate(games)).encode("ascii", "replace")
            self._head("text/plain", len(body))
            self.wfile.write(body)
            return
        m = re.match(r'^/(info|pack|disk)/([^/]+)$', p)
        g = None
        if m:
            key, _, query = m.group(2).partition("?")
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
                     f"KB {g['kb']}", f"CD {str(g.get('cd_kb', 0) // 1024) + ' MB image' + (', also on the server: ?cd=net' if g.get('netcd') else '') if g['cd'] else 'no'}",
                     f"PARTIAL {'yes' if g.get('partial') else 'no'}",
                     f"PLAY {'off the server: /disk/' + g['dir'] if NETDRIVE_DIR else 'no'}", ""]
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
        if m.group(1) == "disk":
            if not NETDRIVE_DIR:
                self.send_error(404, "no NetDrive on this server")
                return
            try:
                name = game_disk(g)
            except Exception as e:
                print(f"waveserve: game disk for {g['dir']}: {e}", file=sys.stderr)
                self.send_error(500, str(e))
                return
            body = f"OK {name}\r\n".encode()
            self._head("text/plain", len(body))
            self.wfile.write(body)
            return
        # ?cd=net|local picks the pack, ?from=N resumes one that stopped
        # part way: the first N files are already on the DOS disk, so they
        # are left out and the client is told what it missed.
        args = dict((kv.split("=", 1) + [""])[:2] for kv in query.split("&") if kv)
        entries = g["files"]
        if args.get("cd") == "raw" and g.get("files_raw"):
            entries = g["files_raw"]             # the discs untouched, audio and all
        elif g.get("files_net") and args.get("cd") not in ("local", "raw"):
            entries = g["files_net"]
        skip = 0
        try:
            skip = max(0, min(int(args.get("from", 0)), len(entries)))
        except ValueError:
            skip = 0
        self._head("application/octet-stream")
        if skip:
            done = sum(e[2] for e in entries[:skip])
            entries = entries[skip:]
            self.wfile.write(f"S {skip} {done}\n".encode())
        # ?crc=1: every file is followed by "C <crc32>", which the client
        # checks against what it wrote. A client that does not ask gets the
        # stream it has always got.
        want_crc = args.get("crc") == "1"

        def end_file(cw):
            if want_crc:
                self.wfile.write(f"C {cw.crc & 0xFFFFFFFF}\n".encode())

        if g["zip"]:
            with zipfile.ZipFile(g["zip"]) as z:
                for entry in entries:
                    rel, name, size = entry[:3]
                    cw = CrcWriter(self.wfile)
                    if len(entry) == 4 and isinstance(entry[3], bytes):
                        body = entry[3]
                        if b"%NDSRV%" in body:      # the address this client reached us on
                            body = body.replace(b"%NDSRV%", f"{self.my_address()}:{NETDRIVE_PORT}".encode())
                        self.wfile.write(f"F {len(body)} {rel}\n".encode())
                        cw.write(body)
                        end_file(cw)
                        continue
                    self.wfile.write(f"F {size} {rel}\n".encode())
                    if len(entry) == 4:
                        if isinstance(entry[3], bytes):
                            cw.write(entry[3])
                        else:
                            self.send_iso(z, entry[3], cw)
                        end_file(cw)
                        continue
                    with z.open(name) as f:
                        shutil.copyfileobj(f, cw, 65536)
                    end_file(cw)
        else:                           # plain files on disk (TDC)
            for rel, fp, size in entries:
                cw = CrcWriter(self.wfile)
                self.wfile.write(f"F {size} {rel}\n".encode())
                with open(fp, "rb") as f:
                    shutil.copyfileobj(f, cw, 65536)
                end_file(cw)
        self.wfile.write(b"E\n")

    def my_address(self):
        """This machine as the client sees it: the Host header, else the
        address it connected to, unless that is a loopback (dosbox-x's
        slirp hands the emulator's requests to 127.0.0.1), then the
        address we would use to reach the outside."""
        host = (self.headers.get("Host") or "").split(":")[0].strip()
        if host and not host.startswith("127."):
            return host
        local = self.connection.getsockname()[0]
        if not local.startswith("127."):
            return local
        try:
            import socket
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("10.255.255.255", 1))     # no packet is sent
            local = s.getsockname()[0]
            s.close()
        except OSError:
            pass
        return local

    def send_iso(self, z, spec, out=None):
        """stream the 2048-byte payload of each sector of a data track"""
        member, skip, ssize, start, count = spec
        out = out or self.wfile
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
                    out.write(buf)
                    buf = bytearray()
        if buf:
            out.write(buf)

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
    ap.add_argument("--netdrive", metavar="DIR", help="keep CD images here as mTCP NetDrive volumes instead of shipping them")
    ap.add_argument("--netdrive-port", type=int, default=2002, help="UDP port of the NetDrive server (default 2002)")
    ap.add_argument("--netdrive-server", metavar="BIN", help="the NetDrive server binary to run (default build/netdrive, else PATH)")
    ap.add_argument("--rescan", type=int, default=300, help="seconds between re-indexing")
    ap.add_argument("--update", metavar="DIR", help="extra files for WAVEGET UPDATE (a WAVE86.INI, drivers...); "
                                                    "the built programs go out anyway")
    a = ap.parse_args()
    global UPDATE_DIR
    if a.update:
        UPDATE_DIR = os.path.abspath(a.update)
    global NETDRIVE_DIR, NETDRIVE_PORT
    if a.netdrive:
        NETDRIVE_DIR, NETDRIVE_PORT = os.path.abspath(a.netdrive), a.netdrive_port
        os.makedirs(NETDRIVE_DIR, exist_ok=True)
        here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        binary = a.netdrive_server or next((p for p in (os.path.join(here, "build", "netdrive"),
                                                        shutil.which("netdrive")) if p and os.path.exists(p)), None)
        if binary:
            import subprocess, atexit
            sessions = os.path.join(NETDRIVE_DIR, "sessions")     # per-session write journals
            os.makedirs(sessions, exist_ok=True)
            nd = subprocess.Popen([binary, "serve", "-headless", "-image_dir", NETDRIVE_DIR, "-port", str(NETDRIVE_PORT),
                                   "-session_scoped_writes_dir", sessions],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            atexit.register(nd.kill)
            print(f"waveserve: NetDrive server on UDP {NETDRIVE_PORT}, images in {NETDRIVE_DIR}", file=sys.stderr)
        else:
            print("waveserve: no NetDrive server binary (make netdrive); run one yourself on port", NETDRIVE_PORT, file=sys.stderr)

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
