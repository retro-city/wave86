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
    python3 tools/dostest.py --run                # boot into the launcher, on screen

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
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fat16

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MARKER = "WAVE86DONE-MARKER"
LAUNCHER_FILES = ["WAVE86.EXE", "WAVE.BAT", "SHCDHD86.EXE", "SHCDX86.COM",
                  "SHSUCDHD.EXE", "SHSUCDX.COM", "WAVEGET.EXE", "DHCP.EXE", "MTCP.CFG", "NE2000.COM",
                  "NETDRIVE.SYS", "NETDRIVE.EXE", "DRVOFF.EXE"]
DOS_EXTRAS = ["CHOICE.EXE", "CTMOUSE.EXE", "JEMMEX.EXE"]        # from dos/: what eXoDOS start batches expect of a DOS install
NET_FLAGS = ["-set", "ne2000 ne2000=true", "-set", "ne2000 backend=slirp",
             "-set", "ne2000 nicbase=300", "-set", "ne2000 nicirq=3"]


def run(cmd, timeout=60, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, **kw)


def mtool(tool, img, off, *args, timeout=120):
    spec = f"{img}@@{off}" if off else img
    return run([tool, "-i", spec, *args], timeout=timeout)


def dosbox(args, cwd, log, wait=None, gui=False):
    env = dict(os.environ)
    head = ["dosbox-x", "-fastlaunch"]
    if not gui:
        env["SDL_VIDEODRIVER"] = "dummy"
        head.append("-nogui")
    p = subprocess.Popen([*head, *args], cwd=cwd, env=env,
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


def fill_hd(img, off, build, games_dir, games, ini_extra, test, run=False):
    work = os.path.dirname(img)
    stage = os.path.join(work, "stage")
    shutil.rmtree(stage, ignore_errors=True)
    w = os.path.join(stage, "WAVE86")
    os.makedirs(w)
    for f in LAUNCHER_FILES:
        src = os.path.join(build, f)
        if os.path.exists(src):
            shutil.copy(src, w)
    for f in DOS_EXTRAS:                    # the boot floppies carry no CHOICE
        src = os.path.join(ROOT, "dos", f)
        if os.path.exists(src):
            shutil.copy(src, w)
    if os.path.isdir(os.path.join(build, "MUSIC")):   # WAVEGET plays it too
        shutil.copytree(os.path.join(build, "MUSIC"), os.path.join(w, "MUSIC"))
    if run:                                 # the real thing: shipped INI, pictures
        shutil.copy(os.path.join(build, "WAVE86.INI"), w)
        if os.path.isdir(os.path.join(build, "THUMBS")):
            shutil.copytree(os.path.join(build, "THUMBS"), os.path.join(w, "THUMBS"))
    else:
        ini = ["gamedir=C:\\GAMES", "music=0", "netdrive=E"] + ini_extra
        for name in games:
            ini += ["", f"[{name}]"]
        open(os.path.join(w, "WAVE86.INI"), "w", newline="").write(bat(ini))
    if test:
        open(os.path.join(w, "TEST.BAT"), "w", newline="").write(test)
    g = os.path.join(stage, "GAMES")
    os.makedirs(g)
    for name in games:
        shutil.copytree(os.path.join(games_dir, name), os.path.join(g, name),
                        ignore=shutil.ignore_patterns(".*", "._*"), dirs_exist_ok=True)
    os.makedirs(os.path.join(stage, "RESULTS"))
    fat16.fill_tree(img, off, stage)
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
              "dir D:\\ > C:\\RESULTS\\CDD.TXT",        # NetDrive may hold D:, then the disc is on E:
              "dir E:\\ > C:\\RESULTS\\CDE.TXT",
              "C:\\WAVE86\\SHCDX86.COM /U /Q > NUL",
              "C:\\WAVE86\\SHCDHD86.EXE /U /Q > NUL",
              "set WAVE86=LAUNCH",
              f"WAVE86 /launch {name} > C:\\RESULTS\\LAUNCH.TXT",
              "copy WAVERUN.BAT C:\\RESULTS\\RUNGAME.TXT > NUL",
              "del WAVERUN.BAT"]
    return bat(t), cd


def check_smoke(results, cd, games_dir=None):
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
        m = re.search(r"^\s+([A-Z]):\s+SHSU-CDH", results.get("CDX.TXT", ""), re.M)
        if not m:
            problems.append("CDX.TXT: SHSUCDX did not assign a drive")
        else:
            listing = results.get(f"CD{m.group(1)}.TXT", "")
            if not re.search(r"^\S+\s+\S+\s+[\d,]+", listing, re.M) or "README   TXT" in listing:
                problems.append(f"CD{m.group(1)}.TXT: no disc files listed on {m.group(1)}:")
        rg = results.get("RUNGAME.TXT", "")
        own = games_dir and os.path.exists(os.path.join(games_dir, name, "IMGMOUNT.BAT"))
        if own:                         # the game's batch mounts; the launcher only unmounts
            if "/F:" in rg or "/U /Q" not in rg:
                problems.append("RUNGAME.TXT: expected only the unmount lines, the game has IMGMOUNT.BAT")
        elif f"/F:C:\\GAMES\\{name}\\CD\\{iso}" not in rg or "/D:SHSU-CDH,D" not in rg:
            problems.append("RUNGAME.TXT: the launcher did not emit the SHSUCDHD/SHSUCDX lines")
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--boot", help="bootable floppy image (default dos/FREEDOS.IMG)")
    ap.add_argument("--games", default=os.path.join(ROOT, "GAMES"), help="games folder (default GAMES/)")
    ap.add_argument("--game", action="append", help="only these game folders (repeatable)")
    ap.add_argument("--script", help="DOS batch to run instead of the smoke test")
    ap.add_argument("--seconds", type=int, default=90, help="how long the guest may run")
    ap.add_argument("--size", type=int, help="hard-disk image size in MB (up to 2047; --run defaults to 500)")
    ap.add_argument("--ini", action="append", default=[], help="extra global INI line, e.g. sound=sb")
    ap.add_argument("--build", default=os.path.join(ROOT, "build"))
    ap.add_argument("--netdrive-sys", action=argparse.BooleanOptionalAction, default=True,
                    help="DEVICE=NETDRIVE.SYS in CONFIG.SYS (default on; --no-netdrive-sys to load it later)")
    ap.add_argument("--cycles", type=int, default=50000, help="dosbox-x CPU cycles for --run (default 50000; tests use auto)")
    ap.add_argument("--fixed-cycles", action="store_true",
                    help="run the test at --cycles instead of dosbox-x's auto (for timing comparisons)")
    ap.add_argument("--run", action="store_true",
                    help="no test: boot into WAVE.BAT in a dosbox-x window, with sound and network")
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
    if a.run and not a.game:            # a clean disk: Alley Cat to try, the rest from the server
        games = [g for g in games if g.upper() == "ALLEYCAT"]
    for name in games:
        if not os.path.isdir(os.path.join(a.games, name)):
            sys.exit(f"no game folder {name} under {a.games}")

    work = os.path.join(a.build, "dosrun" if a.run else "dostest")   # a test must not clobber an open window
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    total = sum(os.path.getsize(os.path.join(r, f)) for name in games
                for r, _, fs in os.walk(os.path.join(a.games, name)) for f in fs)
    # a test disk just fits its games; the interactive disk gets room for
    # downloads (500 MB; FAT16 and DOS 6.22 stop at 2047 MB, --size overrides)
    size_mb = a.size or max(500 if a.run else 64, int(total / 1048576 * 1.3) + 16)

    t0 = time.time()
    print(f"dostest: {os.path.basename(boot)}, {len(games)} game(s), {size_mb} MB disk")
    if a.run:
        test, cd = None, None
    elif a.script:
        test = open(a.script, newline="").read().replace("\r\n", "\n").replace("\n", "\r\n")
        cd = None
    else:
        test, cd = smoke_test(a.games, games, a.build)
    hd, off, geom = make_hd(work, size_mb)
    fill_hd(hd, off, a.build, a.games, games, a.ini, test, run=a.run)

    # the floppy: our AUTOEXEC runs the test, then leaves the marker
    floppy = os.path.join(work, "boot.img")
    shutil.copy(boot, floppy)
    if a.run:
        # the machine as the 486 would be: packet driver, DHCP, the launcher loop
        auto = bat(["@echo off", "set PATH=C:\\WAVE86;A:\\", "set BLASTER=A220 I7 D1 H5 T6", "C:", "cd \\WAVE86",
                    "if exist DRVOFF.EXE DRVOFF D:",
                    "if exist CTMOUSE.EXE LH CTMOUSE > NUL",
                    "if exist NE2000.COM LH NE2000 0x60 3 0x300", "set MTCPCFG=C:\\WAVE86\\MTCP.CFG",
                    "set WAVESRV=10.0.2.2:8086", "if exist DHCP.EXE DHCP", "call WAVE.BAT"])
    else:
        auto = bat(["@echo off", "set MK=MARKER", "set PATH=C:\\WAVE86;A:\\", "C:", "cd \\WAVE86",
                    "if exist DRVOFF.EXE DRVOFF D:",
                    "if exist CTMOUSE.EXE LH CTMOUSE > C:\\RESULTS\\MOUSE.TXT",
                    "call C:\\WAVE86\\TEST.BAT", f"echo {MARKER.replace('MARKER', '%MK%')} > C:\\RESULTS\\DONE.TXT"])
    # JEMMEX: XMS, EMS and upper memory; DOS and the drivers go up there
    conf = ["DEVICE=C:\\WAVE86\\JEMMEX.EXE", "DOS=HIGH,UMB", "FILES=20", "BUFFERS=20", "LASTDRIVE=Z"]
    if a.netdrive_sys and os.path.exists(os.path.join(a.build, "NETDRIVE.SYS")):   # mTCP NetDrive: reserves the letter after C:
        conf.append("DEVICEHIGH=C:\\WAVE86\\NETDRIVE.SYS -d:2")     # D: and E:; DRVOFF frees D: for the disc
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
    boot_cmds = ["-c", f"imgmount c {hd} -size 512,{geom}",
                 "-c", f"imgmount a {floppy} -t floppy",
                 "-c", "boot -l a"]
    if a.run:
        print("dostest: booting into the launcher; close the dosbox-x window to end."
              " Downloads land inside build/dosrun/hd.img.")
        p = dosbox(NET_FLAGS + ["-set", f"cpu cycles={a.cycles}"] + boot_cmds, work,
                   os.path.join(work, "dosbox.log"), gui=True)
        p.wait()
        return
    cyc = ["-set", f"cpu cycles={a.cycles}"] if a.fixed_cycles else []
    p = dosbox(NET_FLAGS + cyc + boot_cmds, work, os.path.join(work, "dosbox.log"))   # the NE2000 is there for scripts too
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
    problems = [] if a.script else check_smoke(results, cd, a.games)
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
