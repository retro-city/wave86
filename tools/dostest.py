#!/usr/bin/env python3
"""Boot a real DOS in dosbox-x and run a batch test against the launcher.

DOSBox's own shell is not DOS: it has no device driver chain, keeps every
drive letter for itself and answers INT 21h in its own way. This boots a
real kernel from a floppy image instead, with the launcher build and a
games folder on a hard-disk image, runs a test batch and brings the
results back.

    python3 tools/dostest.py                      # smoke test on dos/FREEDOS.IMG
    python3 tools/dostest.py --boot ~/Downloads/dos/Disk1.img   # MS-DOS 6.22
    python3 tools/dostest.py --game SYNDICAT --script mytest.bat

The boot image is any bootable 1.44 MB floppy whose kernel runs
A:\\AUTOEXEC.BAT: dos/FREEDOS.IMG (in git), or the first setup disk of
MS-DOS 5/6 (which is a plain boot disk once its AUTOEXEC is replaced;
keep that one outside the repo). The test batch runs on C: with the
launcher in C:\\WAVE86, the games in C:\\GAMES, and writes what it wants
reported to C:\\RESULTS; the harness prints every .TXT from there. It
may end with PASS.TXT or FAIL.TXT to set the exit code.

Needs dosbox-x and mtools (brew install mtools).
"""
import argparse, os, re, shutil, struct, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MARKER = "WAVE86DONE-MARKER"
LAUNCHER_FILES = ["WAVE86.EXE", "WAVE.BAT", "SHCDHD86.EXE", "SHCDX86.COM",
                  "SHSUCDHD.EXE", "SHSUCDX.COM"]


def run(cmd, timeout=60, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, **kw)


def mtool(tool, img, off, *args, timeout=120):
    spec = f"{img}@@{off}" if off else img
    return run([tool, "-i", spec, *args], timeout=timeout)


def dosbox(args, cwd, log, wait=None):
    env = dict(os.environ, SDL_VIDEODRIVER="dummy")
    p = subprocess.Popen(["dosbox-x", "-nogui", "-fastlaunch", *args], cwd=cwd, env=env,
                         stdout=open(log, "w"), stderr=subprocess.STDOUT)
    if wait is not None:
        try:
            p.wait(timeout=wait)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
    return p


def bat(lines):
    return "\r\n".join(lines) + "\r\n"


def make_hd(work, size_mb):
    """IMGMAKE gives a partitioned FAT16 image with a geometry MS-DOS will
    accept; its own output says which one. Returns (path, partition offset,
    "sectors,heads,cylinders")."""
    img = os.path.join(work, "hd.img")
    if os.path.exists(img):
        os.remove(img)
    dosbox(["-c", "mount d .", "-c", f"imgmake hd.img -t hd -size {size_mb} -fat 16 > D:\\MK.TXT",
            "-c", "exit"], work, os.path.join(work, "imgmake.log"), wait=90)
    try:
        text = open(os.path.join(work, "MK.TXT"), errors="replace").read()
    except FileNotFoundError:
        text = ""
    m = re.search(r"(\d+) cylinders, (\d+) heads and (\d+) sectors", text)
    if not m or not os.path.exists(img):
        sys.exit("imgmake failed:\n" + text)
    cyl, heads, secs = m.groups()
    mbr = open(img, "rb").read(512)
    off = struct.unpack("<I", mbr[446 + 8:446 + 12])[0] * 512
    return img, off, f"{secs},{heads},{cyl}"


def fat_fill(img, off, src):
    """Write the tree under src into the (empty) FAT16 partition at byte
    offset off of img, with plain 8.3 entries and no long names: what a
    DOS of the era expects, and a few thousand files take under a second
    (mtools spends 0.4 s on each one)."""
    f = open(img, "r+b")
    f.seek(off)
    bs = f.read(512)
    bps, spc, rsv, nfat, nroot, tot16, _, spf = struct.unpack("<HBHBHHBH", bs[11:24])
    total = tot16 or struct.unpack("<I", bs[32:36])[0]
    fat_off = off + rsv * bps
    root_off = fat_off + nfat * spf * bps
    data_off = root_off + nroot * 32
    nclus = (total - rsv - nfat * spf - nroot * 32 // bps) // spc
    csize = spc * bps
    f.seek(fat_off)
    fat = bytearray(f.read(spf * bps))
    state = {"next": 2}

    def alloc(n):
        chain, c = [], state["next"]
        while len(chain) < n:
            if c >= nclus + 2:
                sys.exit("dostest: the disk image is full (use --size)")
            if struct.unpack_from("<H", fat, c * 2)[0] == 0:
                chain.append(c)
            c += 1
        state["next"] = c
        for i, c in enumerate(chain):
            struct.pack_into("<H", fat, c * 2, chain[i + 1] if i + 1 < len(chain) else 0xFFFF)
        return chain

    def write_chain(chain, data):
        for i, c in enumerate(chain):
            f.seek(data_off + (c - 2) * csize)
            f.write(data[i * csize:(i + 1) * csize])

    def entry(name, ext, attr, first, size, mtime):
        t = time.localtime(mtime)
        d = (max(t.tm_year, 1980) - 1980) << 9 | t.tm_mon << 5 | t.tm_mday
        tm = t.tm_hour << 11 | t.tm_min << 5 | t.tm_sec // 2
        return (name.ljust(8) + ext.ljust(3)).encode("ascii") + bytes([attr]) + bytes(10) + \
            struct.pack("<HHHI", tm, d, first, size)

    ok = "A-Z0-9_\\-!#$%&@^`{}~\'"

    def dos_name(fn, used):
        base, _, ext = fn.rpartition(".") if "." in fn else (fn, "", "")
        base = re.sub(f"[^{ok}]", "_", base.upper())[:8] or "_"
        ext = re.sub(f"[^{ok}]", "", ext.upper())[:3]
        cand, n = (base, ext), 1
        while cand in used:
            n += 1
            cand = (base[:8 - len(f"~{n}")] + f"~{n}", ext)
        used.add(cand)
        return cand

    def put_dir(path, first, parent_first, fixed=None):
        names = sorted(n for n in os.listdir(path) if not n.startswith("."))
        # subdirectories start with . and ..; the root has neither
        entries = entry(".", "", 0x10, first, 0, os.path.getmtime(path)) + \
            entry("..", "", 0x10, parent_first, 0, os.path.getmtime(path)) if fixed else b""
        used = set()
        for n in names:
            p = os.path.join(path, n)
            name, ext = dos_name(n, used)
            st = os.stat(p)
            if os.path.isdir(p):
                count = 2 + sum(1 for x in os.listdir(p) if not x.startswith("."))
                chain = alloc(max(1, -(-count * 32 // csize)))
                entries += entry(name, ext, 0x10, chain[0], 0, st.st_mtime)
                put_dir(p, chain[0], first, chain)
            else:
                data = open(p, "rb").read()
                chain = alloc(-(-len(data) // csize)) if data else []
                if chain:
                    write_chain(chain, data)
                entries += entry(name, ext, 0x20, chain[0] if chain else 0, len(data), st.st_mtime)
        if fixed is None:                       # the root directory area
            if len(entries) > nroot * 32:
                sys.exit("dostest: too many entries in the root directory")
            f.seek(root_off)
            f.write(entries.ljust(nroot * 32, b"\0"))
        else:
            write_chain(fixed, entries.ljust(len(fixed) * csize, b"\0"))

    put_dir(src, 0, 0)
    for i in range(nfat):
        f.seek(fat_off + i * spf * bps)
        f.write(fat)
    f.close()


def fill_hd(img, off, build, games_dir, games, ini_extra, test):
    stage = os.path.join(os.path.dirname(img), "stage")
    shutil.rmtree(stage, ignore_errors=True)
    w = os.path.join(stage, "WAVE86")
    os.makedirs(w)
    for f in LAUNCHER_FILES:
        src = os.path.join(build, f)
        if os.path.exists(src):
            shutil.copy(src, w)
    ini = ["gamedir=C:\\GAMES", "music=0"] + ini_extra
    for name in games:
        ini += ["", f"[{name}]"]
    open(os.path.join(w, "WAVE86.INI"), "w", newline="").write(bat(ini))
    open(os.path.join(w, "TEST.BAT"), "w", newline="").write(test)
    g = os.path.join(stage, "GAMES")
    os.makedirs(g)
    for name in games:
        shutil.copytree(os.path.join(games_dir, name), os.path.join(g, name),
                        ignore=shutil.ignore_patterns(".*", "._*"), dirs_exist_ok=True)
    os.makedirs(os.path.join(stage, "RESULTS"))
    fat_fill(img, off, stage)
    return w


def smoke_test(games_dir, games, build):
    """The default test: the launcher's diagnostics, and for the first game
    that carries a CD image, the driver chain and the generated batch."""
    t = ["@echo off", "ver > C:\\RESULTS\\VER.TXT",
         "if exist A:\\CHKDSK.EXE A:\\CHKDSK C: > C:\\RESULTS\\CHKDSK.TXT",
         "WAVE86 /diag > C:\\RESULTS\\DIAG.TXT"]
    cd = None
    for name in games:
        cdir = os.path.join(games_dir, name, "CD")
        if os.path.isdir(cdir):
            isos = [f for f in os.listdir(cdir) if f.upper().endswith(".ISO")]
            if isos:
                cd = (name, isos[0].upper())
                break
    if cd:
        name, iso = cd
        t += [f"C:\\WAVE86\\SHCDHD86.EXE /F:C:\\GAMES\\{name}\\CD\\{iso} > C:\\RESULTS\\CDHD.TXT",
              "C:\\WAVE86\\SHCDX86.COM /D:SHSU-CDH,D > C:\\RESULTS\\CDX.TXT",
              "dir D:\\ > C:\\RESULTS\\CDDIR.TXT",
              "C:\\WAVE86\\SHCDX86.COM /U /Q > NUL",
              "C:\\WAVE86\\SHCDHD86.EXE /U /Q > NUL",
              "set WAVE86=LAUNCH",
              f"WAVE86 /launch {name} > C:\\RESULTS\\LAUNCH.TXT",
              "copy RUNGAME.BAT C:\\RESULTS\\RUNGAME.TXT > NUL",
              "del RUNGAME.BAT"]
    return bat(t), cd


def check_smoke(results, cd):
    """What the smoke test must have produced."""
    problems = []
    diag = results.get("DIAG.TXT", "")
    if "DOS" not in diag.upper():
        problems.append("DIAG.TXT: no DOS version line (did WAVE86 run?)")
    chk = results.get("CHKDSK.TXT", "")
    if chk and re.search(r"lost|error|cross-linked|invalid", chk, re.I):
        problems.append("CHKDSK.TXT: the file system we wrote has problems")
    if cd:
        name, iso = cd
        if "Drives Assigned" not in results.get("CDX.TXT", ""):
            problems.append("CDX.TXT: SHSUCDX did not assign a drive")
        if not re.search(r"^\S+\s+\S+\s+[\d,]+", results.get("CDDIR.TXT", ""), re.M):
            problems.append("CDDIR.TXT: no files listed on D:")
        rg = results.get("RUNGAME.TXT", "")
        if f"/F:C:\\GAMES\\{name}\\CD\\{iso}" not in rg or "/D:SHSU-CDH,D" not in rg:
            problems.append("RUNGAME.TXT: the launcher did not emit the SHSUCDHD/SHSUCDX lines")
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--boot", help="bootable floppy image (default dos/FREEDOS.IMG)")
    ap.add_argument("--games", default=os.path.join(ROOT, "GAMES"), help="games folder (default GAMES/)")
    ap.add_argument("--game", action="append", help="only these game folders (repeatable)")
    ap.add_argument("--script", help="DOS batch to run instead of the smoke test")
    ap.add_argument("--seconds", type=int, default=90, help="how long the guest may run")
    ap.add_argument("--size", type=int, help="hard-disk image size in MB")
    ap.add_argument("--ini", action="append", default=[], help="extra global INI line, e.g. sound=sb")
    ap.add_argument("--build", default=os.path.join(ROOT, "build"))
    a = ap.parse_args()

    for tool in ("dosbox-x", "mcopy"):
        if not shutil.which(tool):
            sys.exit(f"{tool} not found (dosbox-x; brew install mtools)")
    boot = a.boot or os.path.join(ROOT, "dos", "FREEDOS.IMG")
    if not os.path.exists(boot):
        sys.exit(f"no boot image at {boot}; pass --boot")
    if not os.path.exists(os.path.join(a.build, "WAVE86.EXE")):
        sys.exit("build first: make")
    games = a.game or sorted(d for d in os.listdir(a.games) if os.path.isdir(os.path.join(a.games, d))
                             and not d.startswith("."))
    for name in games:
        if not os.path.isdir(os.path.join(a.games, name)):
            sys.exit(f"no game folder {name} under {a.games}")

    work = os.path.join(a.build, "dostest")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    total = sum(os.path.getsize(os.path.join(r, f)) for name in games
                for r, _, fs in os.walk(os.path.join(a.games, name)) for f in fs)
    size_mb = a.size or max(64, int(total / 1048576 * 1.3) + 16)

    t0 = time.time()
    print(f"dostest: {os.path.basename(boot)}, {len(games)} game(s), {size_mb} MB disk")
    if a.script:
        test = open(a.script, newline="").read().replace("\r\n", "\n").replace("\n", "\r\n")
        cd = None
    else:
        test, cd = smoke_test(a.games, games, a.build)
    hd, off, geom = make_hd(work, size_mb)
    fill_hd(hd, off, a.build, a.games, games, a.ini, test)

    # the floppy: our AUTOEXEC runs the test, then leaves the marker
    floppy = os.path.join(work, "boot.img")
    shutil.copy(boot, floppy)
    auto = bat(["@echo off", "set MK=MARKER", "set PATH=C:\\WAVE86;A:\\", "C:", "cd \\WAVE86",
                "call C:\\WAVE86\\TEST.BAT", f"echo {MARKER.replace('MARKER', '%MK%')} > C:\\RESULTS\\DONE.TXT"])
    conf = ["FILES=20", "BUFFERS=20", "LASTDRIVE=Z"]
    listing = mtool("mdir", floppy, None, "::/").stdout.upper()
    if "KERNEL   SYS" in listing:            # FreeDOS: FreeCOM runs the batch we name
        conf.append("SHELL=A:\\COMMAND.COM A:\\ /E:1024 /P=A:\\AUTOEXEC.BAT")
    for name in ("AUTOEXEC.BAT", "FDAUTO.BAT"):
        open(os.path.join(work, name), "w", newline="").write(auto)
    for name in ("CONFIG.SYS", "FDCONFIG.SYS"):
        open(os.path.join(work, name), "w", newline="").write(bat(conf))
    for name in ("AUTOEXEC.BAT", "FDAUTO.BAT", "CONFIG.SYS", "FDCONFIG.SYS"):
        mtool("mcopy", floppy, None, "-o", os.path.join(work, name), f"::/{name}")
    print(f"dostest: images ready in {time.time() - t0:.0f} s, booting")

    # boot: nothing mounted from the host, or DOSBox hands the guest a
    # host folder on the first free letter and the CD drivers lose D:
    p = dosbox(["-c", f"imgmount c {hd} -size 512,{geom}",
                "-c", f"imgmount a {floppy} -t floppy",
                "-c", "boot -l a"], work, os.path.join(work, "dosbox.log"))
    t1 = time.time()
    finished = False
    try:
        while time.time() - t1 < a.seconds and p.poll() is None:
            time.sleep(4)
            try:
                r = mtool("mtype", hd, off, "::/RESULTS/DONE.TXT", timeout=15)
                if MARKER in r.stdout:
                    finished = True
                    break
            except subprocess.TimeoutExpired:
                pass
        time.sleep(2)
    finally:
        p.kill()
        p.wait()
    took = time.time() - t1

    # results
    out = os.path.join(work, "results")
    os.makedirs(out, exist_ok=True)
    mtool("mcopy", hd, off, "-n", "::/RESULTS/*", out)
    results = {}
    for f in sorted(os.listdir(out)):
        text = open(os.path.join(out, f), errors="replace").read().replace("\r", "")
        results[f] = text
        if f.upper().endswith(".TXT") and f != "DONE.TXT":
            body = "\n".join("    " + l for l in text.strip().splitlines()[:40])
            print(f"--- {f}\n{body}")
    if not finished:
        print(f"dostest: the guest did not finish in {a.seconds} s (results so far above)")
        sys.exit(2)
    problems = [] if a.script else check_smoke(results, cd)
    if "FAIL.TXT" in results:
        problems.append("FAIL.TXT: " + results["FAIL.TXT"].strip())
    if problems:
        print("dostest: FAILED after %.0f s" % took)
        for pr in problems:
            print("  " + pr)
        sys.exit(1)
    print(f"dostest: PASS ({took:.0f} s in the guest, results in {out})")


if __name__ == "__main__":
    main()
