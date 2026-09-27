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

With --torrent FILE the collection need not be on the disk: every game in
the torrent is listed, from metadata alone, and the pieces a game's zip
lies in are fetched from the swarm when somebody asks for it (see "the
collection as a torrent" below, and tools/torrentfs.py). A client that
asks with ?wait=1 is kept informed while that happens:
    W <pct> <text>     the server is waiting for the swarm; text is for showing
    T <files> <bytes>  what the pack holds, which the list may have guessed at
    X <text>           there will be no pack, and why

Started from a terminal it shows what it is doing - the transfers under
way with a bar and the rate, the log, the game list on g - and q stops it.
With no terminal (a log file, a service, the test harness) or --headless
it logs to stderr as before; --log FILE keeps the log either way.
"""
import os, re, sys, zipfile, argparse, unicodedata, threading, time, zlib, shutil, struct, collections, pickle, hashlib, signal
import xml.etree.ElementTree as ET
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fat16, tempfile

NETDRIVE_DIR = None
NETDRIVE_PORT = 2002
TORRENT = None                  # a torrentfs.Tree, with --torrent
SWARM = None                    # and the torrentfs.Swarm under it
TPREFIX = "torrent:"            # a game's zip that is in there, not on the disk
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
    """short folder name (lower) -> dict(genre, year, developer, notes,
    art): from the eXoDOS folder, else from the metadata zip in the torrent"""
    meta = {}
    path = None
    for r in (roots_of(root) + [root]) if root else []:
        p = os.path.join(r, "xml", "all", "MS-DOS.xml")
        if os.path.isfile(p):
            path = p
            break
    src = None
    if path:
        src = open(path, "rb")
    else:
        z = os.path.join(root, "Content", "XODOSMetadata.zip") if root else ""
        if os.path.isfile(z):
            try:
                src = zipfile.ZipFile(z).open("xml/all/MS-DOS.xml")
            except (KeyError, zipfile.BadZipFile):
                src = None
        elif TORRENT and TORRENT.isfile(ART_ZIP):    # 4 MB packed: two pieces, once
            try:
                src = zipfile.ZipFile(TORRENT.open(ART_ZIP)).open("xml/all/MS-DOS.xml")
            except (KeyError, zipfile.BadZipFile, OSError) as e:
                MON.log(f"no game details: the torrent's metadata zip has no readable XML ({e})")
    if not src:
        return meta
    for _, el in ET.iterparse(src):
        if el.tag != "Game":
            continue
        rf = (el.findtext("RootFolder") or "").replace("/", "\\").rstrip("\\")
        short = rf.rsplit("\\", 1)[-1].lower()
        if short:
            meta[short] = dict(
                art=el.findtext("Title") or "",       # the pictures are named after it, as is
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
                  'if not "%WAVECDCMD%"=="" goto cardset',
                  "rem no command for the card: the disc is loaded on it by hand",
                  f"echo   PLEASE LOAD {img} ON YOUR CARD NOW, and press any key when it has it.",
                  "pause > NUL",
                  ":cardset",
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
    with open_zip(zippath, readahead=2) as z, fat16.Volume(tmp) as v:
        v.add_file(iso_name, size, iso_chunks(z, spec))
    os.replace(tmp, path)
    MON.log(f"NetDrive image {os.path.basename(path)} ({size // 1048576} MB)")


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
            with open_zip(g["zip"], readahead=2) if g["zip"] else open(os.devnull) as z:
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
        MON.log(f"game disk {name} ({mb} MB)")
    open(path + ".session_scoped", "a").close()
    return name


def conf_text(root, short):
    """a game's dosbox.conf: from the eXoDOS folder, else from the metadata
    zip in the torrent"""
    for r in roots_of(root) if root else []:
        p = os.path.join(r, "eXo", "eXoDOS", "!dos", short, "dosbox.conf")
        if os.path.isfile(p):
            with open(p, "r", encoding="latin-1", errors="replace") as f:
                return f.read()
    if TMETA:
        return TMETA.conf(short)
    return None


def read_conf(root, short):
    """(candidate program names in autoexec order, CD drive letter or "",
    the folder the conf steps into)"""
    text = conf_text(root, short)
    if text is not None:
        cands, cd, inauto, cds = [], "", False, []
        for line in text.split("\n"):        # not splitlines: a conf may hold a stray \x85
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


ZIPNAME = re.compile(r'^(.*) \((\d{4})\)\.zip$')


def open_zip(path, readahead=0):
    """A game's zip: a file on the disk, or "torrent:<its path in there>",
    which costs the swarm only the pieces that get read. readahead is for
    reading one from end to end; looking into one asks for nothing extra."""
    if path.startswith(TPREFIX):
        return zipfile.ZipFile(TORRENT.open(path[len(TPREFIX):], readahead))
    return zipfile.ZipFile(path)


def game_from_zip(path, fn, root, meta, max_mb, include_cd):
    """Everything the server needs to know to serve one game, out of its zip
    and its eXoDOS conf - or None for a zip that is not one, a game over the
    size limit, a CD game when those are left out."""
    m = ZIPNAME.match(fn)
    try:
        with open_zip(path) as z:
            infos = [i for i in z.infolist() if not i.is_dir()]
    except zipfile.BadZipFile:
        return None
    if not infos:
        return None
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
        return None           # the limit is for the game, not its CD
    cands, cd, subdir = read_conf(root, top)
    if cd and not include_cd:
        return None
    # the CD, as one ISO per data track, made from the cue/bin while
    # streaming; the DOS side mounts CD\NAME.ISO when the game runs
    cd_kb, used, isos = 0, set(), []
    raw_discs, raw_kb = [], 0           # the discs as they came: cue/bin with their audio
    if include_cd and cd:               # the conf mounts a disc: cue/bin pairs, or plain ISOs
        with open_zip(path) as z:
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
    net, net_later = None, None
    if isos and NETDRIVE_DIR:
        img = f"{top.upper()}.IMG"
        spec = next(e[3] for e in files if e[0] == isos[0])
        try:
            if path.startswith(TPREFIX):    # it needs the whole disc: when somebody asks for it
                net_later = (os.path.join(NETDRIVE_DIR, img), path, spec, isos[0].split("\\")[-1])
            else:
                netdrive_image(os.path.join(NETDRIVE_DIR, img), path, spec, isos[0].split("\\")[-1])
            net = ("%NDSRV%", img)
        except Exception as e:          # one bad disc must not take the server down
            MON.log(f"no NetDrive image for {top}: {e}")
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
                    with open_zip(path) as z:
                        inner = start_batch(z.read(entry[1]).decode("cp437"), "", cd or "D").encode("cp437")
                    files[k] = (entry[0], None, len(inner), inner)
                    total += len(inner) - entry[2]
                    break
        exe_file = wrapper
    elif isos and is_bat:
        for k, entry in enumerate(files):
            if entry[0].upper() == exe_file and entry[1]:
                with open_zip(path) as z:
                    body = start_batch(z.read(entry[1]).decode("cp437"), "@call IMGMOUNT.BAT\r\n",
                                       cd or "D").encode("cp437")
                files[k] = (entry[0], None, len(body), body)
                total += len(body) - entry[2]
                break
    if path.startswith(TPREFIX) and exe_file and exe_file != STUB_EXE:
        # a list made before this zip was opened says STUB_EXE starts the game
        body = f"@echo off\r\n{'call ' if exe_file.endswith('.BAT') else ''}{exe_file}\r\n".encode("ascii")
        files.append((STUB_EXE, None, len(body), body))
        total += len(body)
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
    g = dict(title=ascii_text(m.group(1)), year=m.group(2), dir=top.upper(),
             zip=path, src="exodos", kb=(total + 1023) // 1024, files=files, exe=exe_file,
             cd=cd, cd_kb=cd_kb, netcd=netcd, files_net=files_net,
             files_raw=files_raw, raw_kb=raw_kb, genre=md.get("genre", ""), developer=md.get("developer", ""),
             notes=md.get("notes", ""), art=md.get("art", ""))
    if net_later:
        g["net_later"] = net_later
    return g


def index(root, max_mb, include_cd, meta=None):
    meta = (read_xml(root) if root else {}) if meta is None else meta
    seen, games = set(), []
    if not root:
        return games
    for zdir in zip_dirs(root):
        for fn in sorted(os.listdir(zdir)):
            if not ZIPNAME.match(fn) or fn in seen:
                continue
            g = game_from_zip(os.path.join(zdir, fn), fn, root, meta, max_mb, include_cd)
            if g:
                seen.add(fn)
                games.append(g)
    games.sort(key=lambda g: g["title"].lower())
    return games


# ---- the collection as a torrent --------------------------------------------
#
# With --torrent the list is every game in the torrent, and a game's zip is
# opened - which is what costs the swarm anything - the first time somebody
# asks about that game. Until then it is a stub made from metadata: the zip's
# name gives title and year, the eXoDOS conf the CD flag, and the folder under
# !dos that holds "<Title (Year)>.bat" is the game's 8.3 folder. What a stub
# cannot know is the size unpacked (guessed from the zip's) and whether the
# program is a .BAT, an .EXE or a .COM - so a stub says STUB_EXE, and every
# pack of a game from the torrent carries a batch of that name.

STUB_EXE = "WAVE86.BAT"
TMETA = None
OPTS = dict(root=None, max_mb=30, cd=False)     # what main() was told, for resolve()
STUBS = {}
RESOLVED = {}                   # zip name -> the game, for the ones that have been opened
RESOLVE_LOCKS = collections.defaultdict(threading.Lock)
INDEX_FILE = None               # RESOLVED, kept for the next start


class TorrentMeta:
    """Which folder a zip unpacks into, and the conf of a game, without its
    zip: from the !dos folder of an eXoDOS install (Lite will do), else from
    Content/!DOSmetadata.zip in the torrent - 16 MB, fetched once."""
    ZIP = "Content/!DOSmetadata.zip"
    DOS = "eXo/eXoDOS/!dos/"

    def __init__(self, root):
        self.short = {}             # "Title (Year)" -> the folder, as eXoDOS spells it
        self.z, self.confs = None, {}
        for r in roots_of(root) if root else []:
            dos = os.path.join(r, "eXo", "eXoDOS", "!dos")
            for d in os.listdir(dos) if os.path.isdir(dos) else []:
                try:
                    names = os.listdir(os.path.join(dos, d))
                except NotADirectoryError:
                    continue
                for n in names:
                    if n.lower().endswith(".bat") and ZIPNAME.match(n[:-4] + ".zip"):
                        self.short.setdefault(unicodedata.normalize("NFC", n[:-4]), d)
        if self.short or not TORRENT or not TORRENT.isfile(self.ZIP):
            return
        MON.log("no eXoDOS folder with a !dos in it: reading the torrent's metadata zip (16 MB)")
        self.z = zipfile.ZipFile(TORRENT.open(self.ZIP, readahead=2))
        for n in self.z.namelist():
            parts = n[len(self.DOS):].split("/") if n.startswith(self.DOS) else []
            if len(parts) != 2:
                continue
            if parts[1].lower() == "dosbox.conf":
                self.confs[parts[0].lower()] = n
            elif parts[1].lower().endswith(".bat") and ZIPNAME.match(parts[1][:-4] + ".zip"):
                self.short.setdefault(unicodedata.normalize("NFC", parts[1][:-4]), parts[0])

    def conf(self, short):
        n = self.confs.get(short.lower())
        return self.z.read(n).decode("latin-1").replace("\r\n", "\n") if n else None


def stub_game(fn, tpath, size, root, meta, max_mb, include_cd):
    """a game nobody has asked about yet: what the metadata says"""
    m = ZIPNAME.match(fn)
    short = TMETA.short.get(unicodedata.normalize("NFC", fn[:-4]))
    if not short:
        return None                 # no telling where it unpacks to
    cands, cd, _ = read_conf(root, short)
    if cd and not include_cd:
        return None
    if not cd and size > max_mb * 1048576:
        return None                 # larger than the limit before it is even unpacked
    md = meta.get(short.lower(), {})
    # unpacked: a DOS game's files halve in a zip, a disc image much less
    kb = int(size * (1.4 if cd else 2.2)) // 1024 + 1
    return dict(title=ascii_text(m.group(1)), year=m.group(2), dir=short.upper(), zip=TPREFIX + tpath,
                src="exodos", kb=kb, files=[], exe=STUB_EXE if cands else "", cd=bool(cd), cd_kb=0,
                netcd=False, files_net=None, files_raw=None, raw_kb=0, genre=md.get("genre", ""),
                developer=md.get("developer", ""), notes=md.get("notes", ""), art=md.get("art", ""), stub=(fn, size))


def index_torrent(root, max_mb, include_cd, meta):
    """the games that are in the torrent and not on the disk"""
    games = []
    if not TORRENT:
        return games
    on_disk = {unicodedata.normalize("NFC", fn) for zdir in (zip_dirs(root) if root else []) for fn in os.listdir(zdir)}
    folder = "eXo/eXoDOS"
    for fn in TORRENT.listdir(folder) if TORRENT.isdir(folder) else []:
        if not ZIPNAME.match(fn) or unicodedata.normalize("NFC", fn) in on_disk:
            continue
        tpath = f"{folder}/{fn}"
        g = RESOLVED.get(fn)
        if g is None:
            if fn not in STUBS:     # made once: the metadata does not change under a running server
                STUBS[fn] = stub_game(fn, tpath, TORRENT.size(tpath), root, meta, max_mb, include_cd)
            g = STUBS[fn]
        elif g is False or g["kb"] - g.get("cd_kb", 0) > max_mb * 1024 or (g["cd"] and not include_cd):
            continue                # opened, and not one to list
        if g:
            games.append(g)
    return games


def resolve(g, save=True):
    """Open a stub's zip - its directory, a cue sheet, the start of a disc:
    a few pieces - and make the stub the real thing, in place, so whoever
    holds it sees it change. False when it turns out not to be a game we
    list (the pack of it is then a 404)."""
    if "stub" not in g:
        return True
    fn, _ = g["stub"]
    with RESOLVE_LOCKS[fn]:
        if "stub" not in g:
            return True
        t0 = time.time()
        with LOCK:
            meta = META
        real = game_from_zip(g["zip"], fn, OPTS["root"], meta, OPTS["max_mb"], OPTS["cd"])
        RESOLVED[fn] = real or False
        if save:
            save_resolved()
        if not real:
            MON.log(f"{g['dir']}: not a game to list, now that its zip is open")
            return False
        MON.log(f"{real['dir']}: looked into its zip in {time.time() - t0:.1f} s: {len(real['files'])} files, "
                f"{real['kb']} KB, starts with {real['exe'] or '?'}")
        with LOCK:
            g.update(real)
            del g["stub"]
    return True


def pack_missing(g, entries):
    """how many pieces of this pack the swarm has yet to deliver: what
    the console's ALL HERE / WAITING FOR TORRENT tag is made of"""
    if TORRENT is None or not g["zip"].startswith(TPREFIX):
        return 0
    raw = TORRENT.raw(g["zip"][len(TPREFIX):])
    need = set()
    with open_zip(g["zip"]) as z:
        for e in entries:
            member = e[1] or (e[3][0] if len(e) == 4 and isinstance(e[3], tuple) else None)
            if member:
                i = z.getinfo(member)
                need.update(raw.pieces(i.header_offset, min(i.compress_size + 30 + len(i.filename) + 1024,
                                                             raw.size - i.header_offset)))
    return sum(1 for p in need if not SWARM.have(p))


def source_tag(g, entries):
    """the tag: where the pack is coming from, for the console"""
    if TORRENT is None or not g["zip"].startswith(TPREFIX):
        return "ALL HERE"
    left = pack_missing(g, entries)
    return "ALL HERE" if not left else f"WAITING FOR TORRENT: {left} pieces"


def prefetch(g, entries):
    """Tell the swarm what this pack is going to read, all of it and now:
    a 486 takes an hour over what the swarm delivers in minutes, so the
    data is there long before the stream gets to it. Only what the entries
    name - a pack that leaves the disc on the server does not fetch it."""
    if not g["zip"].startswith(TPREFIX):
        return 0
    raw = TORRENT.raw(g["zip"][len(TPREFIX):])
    asked = 0
    with open_zip(g["zip"]) as z:
        for e in entries:
            member = e[1] or (e[3][0] if len(e) == 4 and isinstance(e[3], tuple) else None)
            if member:
                i = z.getinfo(member)
                asked += raw.prefetch(i.header_offset, min(i.compress_size + 30 + len(i.filename) + 1024,
                                                           raw.size - i.header_offset))
    return asked


def save_resolved():
    if INDEX_FILE:
        tmp = INDEX_FILE + ".new"
        with LOCK, open(tmp, "wb") as f:
            pickle.dump(dict(key=INDEX_KEY, games=RESOLVED), f)
        os.replace(tmp, INDEX_FILE)


def load_resolved():
    """What earlier runs found in the zips they opened. If this file has
    changed since, or the options that shape a game, what they found may
    not be what we would find: then only the names are returned, for
    refresh() to open again."""
    try:
        with open(INDEX_FILE, "rb") as f:
            d = pickle.load(f)
        if d.get("key") == INDEX_KEY:
            RESOLVED.update(d["games"])
            return []
        return sorted(d["games"])
    except (OSError, pickle.PickleError, EOFError, AttributeError, KeyError, TypeError):
        return []


def refresh(names):
    """open again the zips an earlier run opened; the pieces that takes are
    here already, so it costs the disk a little and the swarm nothing"""
    MON.log(f"looking again into {len(names)} zips an earlier run opened")
    for fn in names:
        with LOCK:
            g = next((x for x in GAMES if x.get("stub", ("",))[0] == fn), None)
        try:
            if g:
                resolve(g, save=False)
        except Exception as e:
            MON.log(f"{fn}: {e}")
    save_resolved()


META = {}
INDEX_KEY = None


# ---- a picture for the details pane -----------------------------------------
#
# The collection has artwork for nearly every game - screenshots, box art,
# logos - named after the game's title. A pack can end with one of them
# turned into the launcher's 26x7 text-mode picture (tools/makethumb.py):
# ?art=<kind> says which, ?theme=<name> which palette, and the file goes in
# as THUMBS\<THEME>.THM in the game's folder, where the launcher looks after
# its own THUMBS folder. Made once per game, kind and theme, into --thumbs.

ART = {"title": "Screenshot - Game Title", "gameplay": "Screenshot - Gameplay",
       "select": "Screenshot - Game Select", "over": "Screenshot - Game Over",
       "box": "Box - Front", "back": "Box - Back", "3d": "Box - 3D",
       "logo": "Clear Logo", "disc": "Disc"}
ART_ORDER = ["title", "gameplay", "box", "logo"]    # when the kind asked for is missing
ART_ZIP = "Content/XODOSMetadata.zip"              # the same pictures, in the torrent
BAD_IN_NAMES = ':/?*"<>|\'' + chr(92)
THUMBS_DIR = None
ART_SRC = None
ART_NAME = re.compile(r"^(.*)-\d\d\.(png|jpe?g|gif)$", re.I)


def art_stem(title):
    """the file name LaunchBox gives a title's pictures, before -01.png:
    what a file name cannot hold becomes _"""
    return unicodedata.normalize("NFC", "".join("_" if c in BAD_IN_NAMES else c for c in title)).lower()


class Art:
    """The collection's pictures, by kind and title: from Images/MS-DOS in
    the eXoDOS folder, else from the metadata zip in the torrent, where
    one picture costs one piece (and the zip's directory, once, two)."""

    def __init__(self, root):
        self.dirs = [d for r in (roots_of(root) + [root] if root else [])
                     for d in [os.path.join(r, "Images", "MS-DOS")] if os.path.isdir(d)]
        self.index = {}             # kind -> {stem: file path}, the disk
        self.z, self.zindex = None, None     # the torrent's zip, and its folder -> {stem: member}
        self.lock = threading.Lock()
        self.broken = ""

    def _disk(self, kind):
        if kind not in self.index:
            found = {}
            for d in self.dirs:
                folder = os.path.join(d, ART[kind])
                for fn in sorted(os.listdir(folder)) if os.path.isdir(folder) else []:
                    m = ART_NAME.match(fn)
                    if m:
                        found.setdefault(unicodedata.normalize("NFC", m.group(1)).lower(), os.path.join(folder, fn))
            self.index[kind] = found
        return self.index[kind]

    def _zip(self):
        if self.zindex is None:
            self.zindex = {}
            if TORRENT and TORRENT.isfile(ART_ZIP):
                MON.log("no Images folder here: the torrent's metadata zip has the pictures (its directory: 16 MB)")
                self.z = zipfile.ZipFile(TORRENT.open(ART_ZIP))
                for n in self.z.namelist():
                    parts = n.split("/")
                    m = ART_NAME.match(parts[-1]) if len(parts) == 4 and parts[0] == "Images" and parts[1] == "MS-DOS" else None
                    if m:
                        self.zindex.setdefault(parts[2], {}).setdefault(unicodedata.normalize("NFC", m.group(1)).lower(), n)
        return self.zindex

    def find(self, kind, title):
        """the picture's bytes, and where it came from"""
        stem = art_stem(title)
        for k in [kind] + [o for o in ART_ORDER if o != kind]:
            path = self._disk(k).get(stem)
            if path:
                with open(path, "rb") as f:
                    return f.read(), path
            if not self.dirs:
                member = self._zip().get(ART[k], {}).get(stem)
                if member:
                    return self.z.read(member), "torrent:" + member
        return None, None

    def thumb(self, g, kind, theme, cols=26, rows=7):
        """the .THM for a game, from the cache or made now; None when there
        is no picture, no ffmpeg, or nothing to make it with"""
        import makethumb
        if kind not in ART or theme not in makethumb.PALETTES or not g.get("art") or self.broken:
            return None
        path = os.path.join(THUMBS_DIR, theme, kind, f"{cols}x{rows}", g["dir"] + ".THM")
        try:
            with open(path, "rb") as f:
                return f.read()
        except OSError:
            pass
        t0 = time.time()
        with self.lock:                     # one zip, one file position
            data, where = self.find(kind, g["art"])
        if data is None:
            return None
        try:
            thm = makethumb.thumb(data, theme, cols, rows)
        except FileNotFoundError as e:      # no ffmpeg
            self.broken = str(e)
            MON.log("no pictures: makethumb needs ffmpeg on the PATH")
            return None
        except Exception as e:
            MON.log(f"{g['dir']}: no picture from {where}: {e}")
            return None
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path + ".new", "wb") as f:
            f.write(thm)
        os.replace(path + ".new", path)
        MON.log(f"{g['dir']}: picture from {os.path.basename(where)} in {time.time() - t0:.1f} s")
        return thm
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---- waveserve.ini ----------------------------------------------------------
#
# The settings live in waveserve.ini next to the Makefile (or --config FILE),
# one per line, the way WAVE86.INI does it; a flag on the command line
# overrides a line for one run. Each key is one option of main()'s parser.

INI_KEYS = {                    # key in the file -> (option, kind)
    "cache": ("cache", "path"), "cd": ("cd", "bool"), "exodos": ("root", "path"),
    "headless": ("headless", "bool"), "log": ("log", "path"), "max_mb": ("max_mb", "int"),
    "netdrive": ("netdrive", "path"), "netdrive_port": ("netdrive_port", "int"),
    "netdrive_server": ("netdrive_server", "path"), "port": ("port", "int"), "rescan": ("rescan", "int"),
    "tdc": ("tdc", "path"), "thumbs": ("thumbs", "path"), "torrent": ("torrent", "path"),
    "torrent_port": ("torrent_port", "int"), "torrent_portmap": ("torrent_portmap", "bool"),
    "torrent_upload": ("torrent_upload", "text"), "torrent_download": ("torrent_download", "int"),
    "torrent_interface": ("torrent_interface", "text"), "torrent_dht": ("torrent_dht", "bool"),
    "torrent_connections": ("torrent_connections", "int"), "interface": ("interface", "text"),
    "update": ("update", "path"),
}


def read_ini(path):
    """the settings file as parser defaults: paths are made absolute from
    the file's own folder, so a relative one means the same wherever the
    server is started from"""
    out = {}
    if not path or path.lower() == "none" or not os.path.isfile(path):
        return out
    base = os.path.dirname(os.path.abspath(path))
    with open(path, encoding="utf-8", errors="replace") as f:
        for n, line in enumerate(f, 1):
            t = line.strip()
            if not t or t[0] in ";#" or "=" not in t:
                continue
            key, _, val = t.partition("=")
            key, val = key.strip().lower(), val.strip()
            if key not in INI_KEYS:
                print(f"{path}:{n}: no such setting: {key}", file=sys.stderr)
                continue
            opt, kind = INI_KEYS[key]
            if kind == "int":
                try:
                    out[opt] = int(val)
                except ValueError:
                    print(f"{path}:{n}: {key} wants a number, not {val!r}", file=sys.stderr)
            elif kind == "bool":
                out[opt] = val.lower() in ("1", "yes", "on", "true")
            elif kind == "text":
                out[opt] = val
            else:                       # a path, or nothing
                if not val or val.lower() == "none":
                    out[opt] = None
                else:
                    val = os.path.expanduser(val)
                    out[opt] = val if os.path.isabs(val) else os.path.normpath(os.path.join(base, val))
    return out


def off(v):
    """a path option that says none, on the command line, turns the file's off"""
    return None if v is None or str(v).lower() in ("", "none", "off", "no", "0") else os.path.expanduser(str(v))


UPDATE_NAMES = ["WAVE86.EXE", "WAVEGET.EXE", "DRVOFF.EXE", "MEMLIM.EXE", "SLOWDOWN.COM", "SLOWDOWN.DOC", "WAVE86.DEF"]
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


class Monitor:
    """What the console shows and the log keeps: the transfers under way,
    the last few hundred lines of what happened, when we started. The
    request threads report in here; the console reads a snapshot four
    times a second. Headless, every line goes to stderr as it always did."""

    def __init__(self):
        self.lock = threading.Lock()
        self.lines = collections.deque(maxlen=500)
        self.transfers = {}
        self.seq = 0
        self.started = time.time()
        self.headless = True
        self.logfile = None

    def log(self, text):
        line = time.strftime("%H:%M:%S") + "  " + text
        with self.lock:
            self.lines.append(line)
        if self.logfile:
            try:
                self.logfile.write(line + "\n")
                self.logfile.flush()
            except OSError:
                pass
        if self.headless:
            print(line, file=sys.stderr, flush=True)

    def begin(self, client, what, total=0, note=""):
        rec = dict(client=client, what=what, total=total, bytes=0, file="", note=note,
                   started=time.time(), ended=None, status="")
        with self.lock:
            self.seq += 1
            rec["id"] = self.seq
            self.transfers[rec["id"]] = rec
        return rec

    def end(self, rec, status):
        if rec is None or rec["ended"]:
            return
        rec["ended"] = time.time()
        rec["status"] = status
        with self.lock:                 # keep a few finished ones on screen
            done = sorted((r for r in self.transfers.values() if r["ended"]), key=lambda r: r["ended"])
            for r in done[:-6]:
                del self.transfers[r["id"]]

    def snapshot(self):
        with self.lock:
            return ([dict(r) for r in sorted(self.transfers.values(), key=lambda r: r["id"])],
                    list(self.lines))


MON = Monitor()


class Meter:
    """The socket writer, counting for the console."""

    def __init__(self, w, rec):
        self.w, self.rec = w, rec

    def write(self, b):
        n = self.w.write(b)
        self.rec["bytes"] += len(b)
        return n

    def flush(self):
        self.w.flush()

    def __getattr__(self, name):        # closed, close: the writer's own
        return getattr(self.w, name)


def lan_address():
    """The address we would use to reach the outside: what to tell people."""
    import socket
    try:
        so = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        so.connect(("10.255.255.255", 1))     # no packet is sent
        addr = so.getsockname()[0]
        so.close()
        return addr
    except OSError:
        return "127.0.0.1"


class Server(ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        """A client that hung up mid-stream is not a traceback."""
        e = sys.exc_info()[1]
        MON.log(f"{client_address[0]}  {type(e).__name__}: {e}")


class CrcWriter:
    """Passes bytes through to the socket and keeps their CRC32, so each
    file in a pack can be followed by the number the client should have
    arrived at. Same polynomial as zlib and as WAVEGET's table."""

    def __init__(self, w):
        self.w, self.crc = w, 0

    def write(self, b):
        self.crc = zlib.crc32(b, self.crc)
        return self.w.write(b)


class SwarmQuiet(Exception):
    """the swarm stopped delivering a file's pieces: said to the client with an X line"""


class SkipWriter(CrcWriter):
    """The same, for a file the client already has the start of: every byte
    goes into the CRC, so the one sent at the end covers the whole file, but
    only the bytes past the first `skip` go down the wire."""

    def __init__(self, w, skip):
        super().__init__(w)
        self.skip = skip

    def write(self, b):
        self.crc = zlib.crc32(b, self.crc)
        if self.skip:
            if len(b) <= self.skip:
                self.skip -= len(b)
                return len(b)
            b, self.skip = b[self.skip:], 0
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
        self.rec = None
        try:
            self.get()
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError, TimeoutError) as e:
            MON.end(self.rec, "dropped")
            MON.log(f"{self.client_address[0]}  went away ({type(e).__name__})")
        except OSError as e:
            MON.end(self.rec, f"failed: {e}")
            MON.log(f"{self.client_address[0]}  failed: {e}")
        else:
            MON.end(self.rec, "done")

    def get(self):
        with LOCK:
            games = GAMES
        p = self.path
        client = self.client_address[0]
        if p.startswith("/update"):  # WAVEGET UPDATE: the programs themselves
            files = update_set()
            crc = p.endswith("crc=1")
            MON.log(f"update -> {', '.join(n for n, _ in files)}")
            self._head("application/octet-stream")
            self.rec = MON.begin(client, "update", sum(os.path.getsize(fp) for _, fp in files))
            self.wfile = Meter(self.wfile, self.rec)
            for name, fp in files:
                self.rec["file"] = name
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
        m = re.match(r'^/(info|pack|disk|thumb)/([^/]+)$', p)
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
        # A game from the torrent has its zip opened here, the first time
        # anybody asks: seconds if the swarm is quick. A client that said
        # wait=1 gets its answer started first and "W" lines while it waits;
        # any other just waits, and a 404 if there turns out to be no game.
        args = dict((kv.split("=", 1) + [""])[:2] for kv in query.split("&") if kv)
        if m.group(1) == "thumb":       # the picture alone, the moment an install starts: no zip opened for it
            kind, theme = args.get("art", "title").lower(), args.get("theme", "exodos").lower()
            m_ = re.match(r"^(\d+)x(\d+)$", args.get("thumb", ""))
            cols, rows = (max(8, min(38, int(m_.group(1)))), max(4, min(10, int(m_.group(2))))) if m_ else (26, 7)
            thm = ART_SRC.thumb(g, kind, theme, cols, rows) if THUMBS_DIR and g.get("art") else None
            if not thm:
                self.send_error(404)
                return
            self._head("application/octet-stream", len(thm))
            self.wfile.write(thm)
            return
        patient = m.group(1) == "pack" and args.get("wait") == "1"
        if not patient and not resolve(g):
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
                MON.log(f"game disk for {g['dir']}: {e}")
                self.send_error(500, str(e))
                return
            body = f"OK {name}\r\n".encode()
            self._head("text/plain", len(body))
            self.wfile.write(body)
            return
        mode = args.get("cd", "")
        if patient:
            self._head("application/octet-stream")
            self.rec = MON.begin(client, g["dir"] + (f" cd={mode}" if mode else ""), 0,
                                 "looking into its zip" if "stub" in g else "")
            why = "that game cannot be served"
            try:
                found = self.waiting(lambda: resolve(g))
            except (zipfile.BadZipFile, OSError) as e:      # a bad zip, or the swarm has failed us
                found, why = False, str(e)
                MON.log(f"{g['dir']}: {e}")
            if not found:
                self.wfile.write(f"X {why[:70]}\n".encode("ascii", "replace"))
                MON.end(self.rec, f"failed: {why}")
                self.rec = None
                return
        # ?cd=net|local picks the pack, ?from=N resumes one that stopped
        # part way: the first N files are already on the DOS disk, so they
        # are left out and the client is told what it missed.
        entries = g["files"]
        if args.get("cd") == "raw" and g.get("files_raw"):
            entries = g["files_raw"]             # the discs untouched, audio and all
        elif g.get("files_net") and args.get("cd") not in ("local", "raw"):
            entries = g["files_net"]
            if g.get("net_later"):              # out of the torrent: the image is made now
                if patient:
                    self.rec["note"] = "making its NetDrive image"
                    self.waiting(lambda: netdrive_image(*g["net_later"]))
                else:
                    netdrive_image(*g["net_later"])
        # ?art=<kind>&theme=<name>: a picture for the details pane, made from
        # the collection's artwork, as the last file of the pack (so a resume
        # note from before there was one still counts the files right; the
        # launcher fetches it first on its own, through /thumb/)
        kind, theme = args.get("art", "").lower(), args.get("theme", "exodos").lower()
        if kind and THUMBS_DIR and g.get("art"):
            m_ = re.match(r"^(\d+)x(\d+)$", args.get("thumb", ""))    # the size the launcher shows
            cols, rows = (max(8, min(38, int(m_.group(1)))), max(4, min(10, int(m_.group(2))))) if m_ else (26, 7)
            thm = ART_SRC.thumb(g, kind, theme, cols, rows)
            if thm:
                entries = entries + [("THUMBS" + chr(92) + theme.upper() + ".THM", None, len(thm), thm)]
        skip = at = 0
        try:
            skip = max(0, min(int(args.get("from", 0)), len(entries)))
            at = max(0, int(args.get("at", 0)))     # ... and B bytes of the next one
        except ValueError:
            skip = at = 0
        if patient:                             # what the list may only have guessed at
            self.wfile.write(f"T {len(entries)} {sum(e[2] for e in entries)}\n".encode())
        else:
            self._head("application/octet-stream")
        if skip:
            done = sum(e[2] for e in entries[:skip])
            entries = entries[skip:]
            self.wfile.write(f"S {skip} {done}\n".encode())
        total = sum(e[2] for e in entries)
        if at and entries:
            total -= min(at, entries[0][2])
        note = ""
        if skip or at:
            note = f"resumed: {skip} files" + (f", {at // 1048576} MB in" if at else "")
        if patient:
            self.rec.update(total=total, note=note)
        else:
            self.rec = MON.begin(client, g["dir"] + (f" cd={mode}" if mode else ""), total, note)
        self.wfile = Meter(self.wfile, self.rec)
        # ?crc=1: every file is followed by "C <crc32>", which the client
        # checks against what it wrote. A client that does not ask gets the
        # stream it has always got.
        want_crc = args.get("crc") == "1"

        def end_file(cw):
            if want_crc:
                self.wfile.write(f"C {cw.crc & 0xFFFFFFFF}\n".encode())

        # ?at=B: the first file sent is one the client has B bytes of. Its
        # header is "P <size> <B> <name>" instead of "F <size> <name>" and only
        # the rest of it follows; the CRC after it is still the whole file's.
        first = [at > 0]

        def head(size, rel):
            self.rec["file"] = rel
            if first[0]:
                first[0] = False
                b = min(at, size)
                self.wfile.write(f"P {size} {b} {rel}\n".encode())
                return SkipWriter(self.wfile, b)
            self.wfile.write(f"F {size} {rel}\n".encode())
            return CrcWriter(self.wfile)

        if g["zip"]:
            asked = prefetch(g, entries)
            self.rec["src"] = source_tag(g, entries)
            self.rec["pack"] = (g, entries)     # for the tag to be kept up to date
            if asked:
                MON.log(f"{g['dir']}: {asked} pieces ({asked * TORRENT.t.piece_length // 1048576} MB) to get from the swarm")
                self.rec["note"] = (self.rec.get("note", "") + "  from the swarm").strip()
            # four pieces ahead of the read: 32 MB, some minutes of a 486's
            # time, for the swarm to come up with the next one
            try:
                self.send_entries(g, entries, patient, head, end_file)
            except SwarmQuiet as e:         # between files: the client can be told
                self.wfile.write(f"X {str(e)[:70]}\n".encode("ascii", "replace"))
                MON.log(f"{g['dir']}: {e}")
                MON.end(self.rec, f"failed: {e}")
                self.rec = None
                return
        else:                           # plain files on disk (TDC)
            for rel, fp, size in entries:
                cw = head(size, rel)
                with open(fp, "rb") as f:
                    shutil.copyfileobj(f, cw, 65536)
                end_file(cw)
        self.wfile.write(b"E\n")

    def send_entries(self, g, entries, patient, head, end_file):
        """the files of a zip-backed game, in order; from a torrent, each one
        whole on this side before its header goes out (await_member)"""
        from_swarm = TORRENT is not None and g["zip"].startswith(TPREFIX)
        with open_zip(g["zip"], readahead=4) as z:
            for entry in entries:
                rel, name, size = entry[:3]
                member = name or (entry[3][0] if len(entry) == 4 and isinstance(entry[3], tuple) else None)
                if from_swarm and patient and member:
                    self.await_member(z, member)
                if len(entry) == 4 and isinstance(entry[3], bytes):
                    body = entry[3]
                    if b"%NDSRV%" in body:      # the address this client reached us on
                        body = body.replace(b"%NDSRV%", f"{self.my_address()}:{NETDRIVE_PORT}".encode())
                    cw = head(len(body), rel)
                    cw.write(body)
                    end_file(cw)
                    continue
                cw = head(size, rel)
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

    def waiting(self, work):
        """work() may sit waiting for the swarm. Meanwhile the client gets a
        "W" line every few seconds: it has something to show, its timeout
        does not run out - and a write that fails tells us it has gone.
        What was started carries on regardless; somebody wanted it."""
        box = {}

        def run():
            try:
                box["value"] = work()
            except Exception as e:
                box["error"] = e
        th = threading.Thread(target=run, daemon=True)
        th.start()
        while True:
            th.join(3)
            if not th.is_alive():
                break
            self.wait_line()
        if "error" in box:
            raise box["error"]
        return box.get("value")

    def wait_line(self, text=None, pct=0):
        if text is None:
            if SWARM is None:
                text = "the server is getting it ready"
            else:
                st = SWARM.status()
                text = (f"the server is fetching it: {max(1, st['waiting'])} pieces to go, {st['peers']} peers, "
                        f"{st['down'] / 1024:.0f} KB/s")
        self.wfile.write(f"W {pct} {text}\n".encode("ascii", "replace"))
        self.wfile.flush()

    def await_member(self, z, member):
        """Before a file's header goes out, have the whole of it here. A W line
        keeps the client waiting, but only between files: inside one there is
        nothing but its bytes, so a piece the swarm has not delivered by the
        time the stream reaches it is two minutes of silence and a client
        that gives up ("the server went quiet") - what happened at 14 MB into
        a game the swarm was still fetching. So the file's pieces are asked
        for, urgently and in order, and the header waits for the last of
        them, the client hearing how far along it is every few seconds. A
        swarm that delivers nothing for half an hour ends the pack with an X
        line, which the client shows."""
        raw = z.fp.raw
        i = z.getinfo(member)
        length = min(i.compress_size + 30 + len(i.filename) + 1024,     # the local header, with slack
                     raw.size - i.header_offset)                          # ... but not past the zip's end
        pieces = [p for p in raw.pieces(i.header_offset, length) if not SWARM.have(p)]
        if not pieces:
            return
        SWARM.want(pieces, urgent=True)
        name = member.rsplit("/", 1)[-1]
        total, left, quiet_since, tick = len(pieces), len(pieces), time.time(), 0
        self.rec["note"] = f"waiting for the swarm: {name}"
        MON.log(f"{self.rec.get('what', name)}: {total} pieces of {name} to come from the swarm before it goes out")
        while True:
            now_left = sum(1 for p in pieces if not SWARM.have(p))
            if not now_left:
                break
            if now_left < left:
                left, quiet_since = now_left, time.time()
            if time.time() - quiet_since > 1800:
                raise SwarmQuiet(f"the swarm delivered nothing of {name} for half an hour: try again later")
            time.sleep(1)
            tick += 1
            if tick % 3 == 0:               # every third second, whatever the clock says
                if "pack" in self.rec:
                    self.rec["src"] = source_tag(*self.rec["pack"])
                pct = 100 * (total - now_left) // total
                st = SWARM.status()
                self.wait_line(f"fetching {name}: {pct}%, {now_left} pieces to go, {st['peers']} peers, "
                               f"{st['down'] / 1024:.0f} KB/s", pct)
        self.rec["note"] = "from the swarm"
        if "pack" in self.rec:
            self.rec["src"] = source_tag(*self.rec["pack"])

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
        return lan_address()

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
        MON.log("%s  %s" % (self.client_address[0], fmt % args))


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
    ap.add_argument("--torrent", metavar="FILE", help="the collection's .torrent: list every game in it, and fetch "
                                                      "a game's pieces from the swarm when somebody asks for it")
    ap.add_argument("--cache", metavar="DIR", default="~/wave86-torrent", help="where the pieces are kept (default %(default)s)")
    ap.add_argument("--torrent-port", type=int, default=6881, help="the port other peers reach us on (default 6881)")
    ap.add_argument("--torrent-upload", default="1024", metavar="KB/S", help="what we give back to the swarm, at most "
                                                                              "(default 1024; 0 for no limit, off for nothing)")
    ap.add_argument("--torrent-download", type=int, default=0, metavar="KB/S", help="what we take, at most (0: no limit)")
    ap.add_argument("--torrent-interface", default="", metavar="ADDR", help="the address or device the swarm side "
                                                                            "listens and calls out on (default: all)")
    ap.add_argument("--torrent-dht", type=int, default=1, choices=(0, 1), help="1 finds peers through the DHT too, 0 only "
                                                                             "through the tracker (default 1)")
    ap.add_argument("--torrent-connections", type=int, default=200, help="peers at most (default 200)")
    ap.add_argument("--interface", default="", metavar="ADDR", help="the address WAVE86 reaches the server on "
                                                                     "(default: every address this machine has)")
    ap.add_argument("--thumbs", metavar="DIR", default="~/wave86-thumbs",
                    help="where the pictures made for the details pane are kept (default %(default)s; "
                         "'none' sends no pictures)")
    ap.add_argument("--torrent-portmap", action="store_true", help="ask the router (UPnP, NAT-PMP) to let peers in on that "
                                                                  "port; off, we only call out, which is enough to fetch")
    ap.add_argument("--headless", action="store_true", help="no console, log to stderr (automatic without a terminal)")
    ap.add_argument("--log", metavar="FILE", help="append the log to FILE as well")
    ap.add_argument("--config", metavar="FILE", default=os.path.join(HERE, "waveserve.ini"),
                    help="the settings file (default %(default)s; 'none' for none); the command line overrides it")
    ap.add_argument("--no-cd", dest="cd", action="store_false", help="leave the CD games out (cd=0 in the file)")
    pre, _ = ap.parse_known_args()
    ini = read_ini(pre.config)
    ap.set_defaults(**ini)
    a = ap.parse_args()
    for name in ("root", "tdc", "netdrive", "netdrive_server", "update", "torrent", "cache", "thumbs", "log"):
        setattr(a, name, off(getattr(a, name)))
    if ini:
        MON.log(f"settings from {pre.config}" + ("" if len(sys.argv) < 2 else ", the command line on top"))
    MON.headless = a.headless or not sys.stdout.isatty()
    if a.log:
        MON.logfile = open(a.log, "a")
    global UPDATE_DIR
    if a.update:
        UPDATE_DIR = os.path.abspath(a.update)
    global NETDRIVE_DIR, NETDRIVE_PORT
    if a.netdrive:
        NETDRIVE_DIR, NETDRIVE_PORT = os.path.abspath(os.path.expanduser(a.netdrive)), a.netdrive_port
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
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            atexit.register(nd.kill)
            time.sleep(1.0)                 # a port already taken shows at once (and it exits 0)
            if nd.poll() is not None:
                err = [l for l in (nd.stdout.read() or "").splitlines() if l.strip()]
                MON.log(f"the NetDrive server did not start (exit {nd.returncode}): {err[-1] if err else 'no message'}"
                        f" - is another one on UDP {NETDRIVE_PORT}? (lsof -iUDP:{NETDRIVE_PORT})")
            else:
                MON.log(f"NetDrive server on UDP {NETDRIVE_PORT}, images in {NETDRIVE_DIR}")
        else:
            MON.log(f"no NetDrive server binary (make netdrive); run one yourself on port {NETDRIVE_PORT}")

    if not a.root and not a.tdc and not a.torrent:
        ap.error("give an eXoDOS folder, --torrent FILE and/or --tdc DIR")
    global THUMBS_DIR, ART_SRC
    if a.thumbs:
        THUMBS_DIR = os.path.abspath(a.thumbs)
        os.makedirs(THUMBS_DIR, exist_ok=True)
    OPTS.update(root=a.root, max_mb=a.max_mb, cd=a.cd)
    swarm = None
    if a.torrent:
        global TORRENT, SWARM, TMETA, INDEX_FILE, INDEX_KEY
        try:
            import torrentfs
            t = torrentfs.Torrent(a.torrent)
            up = None if str(a.torrent_upload).strip().lower() in ("off", "none", "no") else int(a.torrent_upload)
            swarm = torrentfs.Swarm(t, a.cache, port=a.torrent_port, up_kb=up, down_kb=a.torrent_download,
                                    interface=a.torrent_interface, dht=a.torrent_dht, connections=a.torrent_connections,
                                    log=MON.log, portmap=a.torrent_portmap)
        except ImportError as e:
            sys.exit(f"--torrent needs libtorrent's Python bindings ({e}): brew install libtorrent-rasterbar, "
                     "or pip install libtorrent")
        TORRENT, SWARM = torrentfs.Tree(t, swarm), swarm
        cache = os.path.abspath(os.path.expanduser(a.cache))
        MON.log(f"torrent {t.name}: {len(t.files)} files, {t.size / 2 ** 30:.0f} GB in {t.num_pieces} pieces of "
                f"{t.piece_length // 1048576} MB; the ones we hold are in {cache}")
        MON.log(f"swarm: port {a.torrent_port}" + (f" on {a.torrent_interface}" if a.torrent_interface else "")
                + (", no uploading" if up is None else f", up {up or 'unlimited'} KB/s")
                + (f", down {a.torrent_download} KB/s" if a.torrent_download else "")
                + (", DHT" if a.torrent_dht else ", tracker only") + f", {a.torrent_connections} peers at most")
        TMETA = TorrentMeta(a.root)
        # what earlier runs learned from the zips they opened holds for as
        # long as this file and the options that shape a game stay the same
        with open(os.path.abspath(__file__), "rb") as f:
            INDEX_KEY = (hashlib.sha1(f.read()).hexdigest(), a.max_mb, a.cd, bool(a.netdrive))
        INDEX_FILE = os.path.join(cache, t.infohash + ".index")
        again = load_resolved()
    ART_SRC = Art(a.root)

    def reindex(first=False):
        global GAMES, META
        meta = read_xml(a.root)              # from the folder, or the torrent
        with LOCK:
            META = meta
        games = index(a.root, a.max_mb, a.cd, meta) + index_torrent(a.root, a.max_mb, a.cd, meta) + index_tdc(a.tdc, a.max_mb)
        games.sort(key=lambda g: g["title"].lower())
        with LOCK:
            changed = [g["title"] for g in games] != [g["title"] for g in GAMES]
            GAMES = games
        if first or changed:
            stubs = sum(1 for g in games if "stub" in g)
            MON.log(f"{len(games)} games ({sum(1 for g in games if g['src']=='exodos')} eXoDOS, "
                    f"{sum(1 for g in games if g['src']=='tdc')} TDC), port {a.port}"
                    + (f"; {stubs} of them still unopened in the torrent" if TORRENT else ""))

    reindex(first=True)
    if a.torrent and again:
        threading.Thread(target=refresh, args=(again,), daemon=True).start()

    def loop():
        while True:
            time.sleep(a.rescan)
            try:
                reindex()
            except Exception as e:      # keep serving on a bad scan
                MON.log(f"rescan failed: {e}")
    threading.Thread(target=loop, daemon=True).start()
    server = Server((a.interface or "0.0.0.0", a.port), H)
    if not MON.headless:
        try:
            import waveconsole
        except ImportError as e:            # no curses on this Python
            MON.headless = True
            MON.log(f"no console ({e}), logging here instead")
    if MON.headless:
        def stop(*_):                       # a kill is a q: what the swarm holds gets written down
            raise KeyboardInterrupt
        signal.signal(signal.SIGTERM, stop)
        signal.signal(signal.SIGINT, stop)  # a shell starts a background job with this one ignored
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            if swarm:
                MON.log("writing down what the swarm gave us ...")
                swarm.close()
        return

    def title():
        with LOCK:
            n, ex = len(GAMES), sum(1 for g in GAMES if g["src"] == "exodos")
        nd = f"   NetDrive UDP {NETDRIVE_PORT}" if NETDRIVE_DIR else ""
        up = int(time.time() - MON.started)
        return (f"waveserve  {lan_address()}:{a.port}   {n} games ({ex} eXoDOS, {n - ex} TDC){nd}"
                f"   up {up // 3600}:{up % 3600 // 60:02d}:{up % 60:02d}")

    def swarm_line():
        if not swarm:
            return None
        st = swarm.status()
        if st["error"]:
            return f"SWARM  {st['error']}"
        return (f"SWARM  {st['peers']} peers ({st['seeds']} seeds)   down {st['down'] / 1024:.0f} KB/s   "
                f"up {st['up'] / 1024:.0f} KB/s   holding {waveconsole.fmt_size(st['held'])}"
                + (f"   waiting for {st['waiting']} pieces" if st["waiting"] else ""))

    def held(g):
        """for the games view: how much of a torrent game's zip is here"""
        if TORRENT is None or not g["zip"].startswith(TPREFIX):
            return "local"
        try:
            raw = TORRENT.raw(g["zip"][len(TPREFIX):])
        except Exception:
            return "?"
        total = len(raw.pieces())
        missing = raw.missing()
        if not total:
            return "-"
        if not missing:
            return "all"
        if missing == total:
            return "none"
        return f"{total - missing}/{total}"

    def games():
        with LOCK:
            return GAMES

    def rescan():
        MON.log("re-indexing ...")
        threading.Thread(target=reindex, daemon=True).start()

    threading.Thread(target=server.serve_forever, daemon=True).start()
    MON.log("q quits, r re-indexes, g shows the games")
    try:
        waveconsole.run(MON, title, games, dict(rescan=rescan), swarm_line, held)
    except KeyboardInterrupt:
        pass
    server.shutdown()
    if swarm:
        swarm.close()


if __name__ == "__main__":
    main()
