#!/usr/bin/env python3
"""A bootable hard-disk image for a PicoMem 2: EDR-DOS (or FreeDOS), the launcher, the
card's DOS tools, mTCP, booting straight into WAVE86.

    make picomem-image                  (unpacks the pieces from third-party/, then runs this)
    python3 tools/mkimage.py --dist dist/wave86 --picomem build/picomem \\
        --mtcp build/mtcp --cdmke build/cdmke --gus build/ultrasnd -o build/pmwave-fdos.img

The image is a raw disk with an MBR and one active FAT16 partition, in the
shape the PicoMem's BIOS assumes: 16 heads, 63 sectors, the cylinders from
the file size, and DOSBox's big-disk shift (heads doubled, cylinders halved)
above 1024 cylinders - the card's INT 13h is DOSBox's, CHS only, no LBA.
dosbox-x's IMGMAKE makes it with exactly that geometry, so its MBR and BPB
say what the card will say. IMGMAKE writes no operating-system boot sector,
so the FreeDOS FAT16 one (dos/FAT16.BS, assembled from the kernel's own
boot/boot.asm) goes over the partition's, keeping the BPB IMGMAKE wrote -
what SYS.COM does. KERNEL.SYS and COMMAND.COM come off dos/FREEDOS.IMG, the
tree from the folders given, and tools/fat16.py writes it all in one go.

Then the self-test, unless --no-test: a copy of the image boots in dosbox-x
(headless, from the hard disk, no floppy) with the launcher's WAVE.BAT
swapped for a stub that records what the image's own CONFIG.SYS and
AUTOEXEC.BAT produced - once as dosbox-x boots it (it has INT 13h
extensions, so the boot sector reads by LBA) and once with the boot sector
told to use CHS, the way the card will. The PicoMem programs and DHCP find
no card there and give up; the point is that the AUTOEXEC gets past them
to WAVE.BAT. Needs dosbox-x and mtools (brew install mtools). The work
files (the staged tree, the test clones, dosbox-x's logs, the results) go
under build/<image name>-work/ whatever -o says, so the image itself can be
written straight to the SD card.
"""
import argparse, os, re, shutil, struct, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fat16
from dostest import dosbox, mtool, bat, MARKER

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FREEDOS_IMG = os.path.join(ROOT, "dos", "FREEDOS.IMG")
BOOT_SECTOR = os.path.join(ROOT, "dos", "FAT16.BS")
CYL_BYTES = 16 * 63 * 512          # one cylinder as the card counts them

# what the launcher's INI says on this machine: the games and the discs on
# the SD card (W:, PMDFS), the discs loaded on the card - imgmount=PICOMEM
# with no cdmount_picomem= command, since the PicoMem 2 has none yet: the
# batch names the disc, asks for it to be loaded on the card, and waits for
# a key - on D:, and the server left for the owner to fill in.
INI_SETTINGS = [("gamedir", "W:\\EXODOS"), ("cdrom_storage", "W:\\CDROM"), ("theme", "exodos"),
                ("imgmount", "PICOMEM"), ("cdrom_letter", "D"), ("server", "")]

def picomem_view(size):
    """(cylinders, heads, sectors) as the card's BIOS reports a raw image
    of size bytes: pm_disk.cpp takes 16 heads x 63 sectors and as many
    cylinders as fit, then imageDisk::Set_Geometry (DOSBox's) shifts
    heads up and cylinders down while there are more than 1024."""
    cyl, heads = size // CYL_BYTES, 16
    if cyl > 16384:
        raise ValueError("more than 8 GB: the card cannot address that")
    shift = 4 if cyl > 8192 else 3 if cyl > 4096 else 2 if cyl > 2048 else 1 if cyl > 1024 else 0
    return cyl >> shift, heads << shift, 63


def image_geometry(size_mb):
    """The CHS to hand IMGMAKE for a size_mb image: the card's view of it,
    with cylinders capped at 1023, the most CHS can address (the file is
    then a whole cylinder shorter, and the card's view of that is the
    same shape)."""
    cyl, heads, secs = picomem_view(size_mb * 1048576)
    return min(cyl, 1023), heads, secs


def run_imgmake(work, size_mb):
    """IMGMAKE in dosbox-x, into work/hd.img. Returns (path, partition
    offset, "sectors,heads,cylinders" for imgmount)."""
    cyl, heads, secs = image_geometry(size_mb)
    img = os.path.join(work, "hd.img")
    for f in (img, os.path.join(work, "MK.TXT")):
        if os.path.exists(f):
            os.remove(f)
    dosbox(["-c", "mount d .", "-c", f"imgmake hd.img -t hd -chs {cyl},{heads},{secs} -fat 16 > D:\\MK.TXT",
            "-c", "exit"], work, os.path.join(work, "imgmake.log"), wait=600)
    try:
        text = open(os.path.join(work, "MK.TXT"), errors="replace").read()
    except FileNotFoundError:
        text = ""
    m = re.search(r"(\d+) cylinders, (\d+) heads and (\d+) sectors", text)
    if not m or not os.path.exists(img):
        sys.exit("mkimage: imgmake failed:\n" + text)
    got = tuple(int(x) for x in m.groups())
    if got != (cyl, heads, secs):
        sys.exit(f"mkimage: asked IMGMAKE for {cyl}x{heads}x{secs}, it made {got}")
    mbr = open(img, "rb").read(512)
    off = struct.unpack("<I", mbr[446 + 8:446 + 12])[0] * 512
    return img, off, f"{secs},{heads},{cyl}"


def chs3(b):
    """(cylinder, head, sector) from a partition entry's packed CHS."""
    return ((b[1] & 0xC0) << 2) | b[2], b[0], b[1] & 0x3F


def check_layout(img, own_boot=True):
    """The MBR and the partition's boot sector, checked against what the
    card will assume. Prints the lot; a mismatch is fatal."""
    size = os.path.getsize(img)
    with open(img, "rb") as f:
        mbr = f.read(512)
        e = mbr[446:462]
        start = struct.unpack("<I", e[8:12])[0]
        length = struct.unpack("<I", e[12:16])[0]
        f.seek(start * 512)
        bs = f.read(512)
    bps, spc, rsvd, nfat, nroot, tot16, media, spf, spt, heads, hidden, tot32 = \
        struct.unpack("<HBHBHHBHHHII", bs[11:36])
    total = tot16 or tot32
    pcyl, pheads, psecs = picomem_view(size)
    end = chs3(e[5:8])
    print(f"mkimage: {size:,} bytes = {size // CYL_BYTES} cylinders of 16 x 63 to the card, "
          f"reported as {pcyl} x {pheads} x {psecs}")
    print(f"  MBR: boot flag {e[0]:02X}, type {e[4]:02X}, start LBA {start} (CHS {chs3(e[1:4])}), "
          f"{length} sectors, end CHS {end}, signature {mbr[510:512].hex().upper()}")
    print(f"  boot sector: OEM {bs[3:11].decode('ascii', 'replace')!r}, {bps} B/sector, {spc} sectors/cluster, "
          f"{rsvd} reserved, {nfat} FATs x {spf}, {nroot} root entries, media {media:02X}, "
          f"{spt} sectors/track, {heads} heads, {hidden} hidden, {total} sectors, drive {bs[0x24]:02X}, "
          f"{bs[0x36:0x3E].decode('ascii', 'replace').strip()!r}, signature {bs[510:512].hex().upper()}")
    problems = []
    if mbr[510:512] != b"\x55\xAA" or bs[510:512] != b"\x55\xAA":
        problems.append("a boot signature is missing")
    if e[0] != 0x80:
        problems.append("the partition is not active")
    if e[4] not in (0x04, 0x06):
        problems.append(f"partition type {e[4]:02X} is not a CHS FAT16 type")
    if bps != 512 or not bs[0x36:0x3E].startswith(b"FAT16"):
        problems.append("the partition is not FAT16 with 512-byte sectors")
    if (spt, heads) != (psecs, pheads):
        problems.append(f"the BPB says {heads} heads x {spt} sectors, the card will say {pheads} x {psecs}")
    if hidden != start or start + total > pcyl * pheads * psecs or end[0] != pcyl - 1:
        problems.append("the partition does not fit the cylinders the card will report")
    if own_boot and (bs[0x24] != 0x80 or bs[3:11] != b"FRDOS5.1"):
        problems.append("the FreeDOS boot sector is not in place")
    if problems:
        sys.exit("mkimage: the image would not boot on the card:\n  " + "\n  ".join(problems))


# the volume label, in the BPB and the root directory: it names the DOS on the image
LABELS = {"fdos": "WAVE_FD", "edrdos": "WAVE_EDR", "msdos": "WAVE_MS", "pcdos": "WAVE_PC", "dos": "WAVE_DOS"}


def install_boot_sector(img, off, label):
    """The FreeDOS FAT16 boot sector over IMGMAKE's non-bootable one, the
    way SYS.COM does it: the BPB (bytes 0x0B-0x3D) stays as written but for
    the volume label, the OEM name becomes FRDOS5.1, the BIOS drive 80h."""
    new = bytearray(open(BOOT_SECTOR, "rb").read())
    if len(new) != 512 or new[510:512] != b"\x55\xAA" or b"KERNEL  SYS" not in new:
        sys.exit(f"mkimage: {BOOT_SECTOR} is not the FreeDOS boot sector")
    with open(img, "r+b") as f:
        f.seek(off)
        old = f.read(512)
        if old[11:13] != b"\x00\x02" or not old[0x36:0x3E].startswith(b"FAT16"):
            sys.exit("mkimage: the partition IMGMAKE made is not FAT16")
        new[11:62] = old[11:62]
        new[3:11] = b"FRDOS5.1"
        new[0x24] = 0x80
        new[0x2B:0x36] = label.encode("ascii").ljust(11)[:11]
        f.seek(off)
        f.write(new)


FLAVOURS = {"fdos": "FreeDOS", "edrdos": "EDR-DOS (the SvarDOS kernel)", "msdos": "MS-DOS", "pcdos": "PC DOS", "dos": "DOS"}


def flavour_of(a):
    """what the image runs: fdos, edrdos, or the DOS on the disks given"""
    if not a.dos_disks:
        return a.kernel if a.kernel == "edrdos" else "fdos"
    names = mtool("mdir", a.dos_disks[0], None, "-a", "-b", "::/").stdout.upper()
    if "IO.SYS" in names:
        return "msdos"
    if "IBMBIO.COM" in names:
        return "pcdos"
    return "dos"


# the boot menu: its items, the CONFIG.SYS block names for the DOSes whose
# menus set CONFIG to a name (FreeDOS's MENU sets the number), and WMODE,
# which AUTOEXEC.BAT sets for the launcher
MENU_ITEMS = ("PicoMEM - Standard Mode (CD, DFS and Network)",
              "PicoMEM - Local Mode (CD and DFS)",
              "PicoMEM - Network Mode",
              "PicoMEM - Memory Optimized",
              "Safe Mode")
MENU_NAMES = ("STD", "LOCAL", "NET", "MEM", "SAFE")
WMODES = ("Standard", "Local", "Network", "Memory", "Safe")
JEMM = "DEVICE=C:\\DOS\\JEMM386.EXE RAM X=D000-D7FF"
NETDRIVE = "DEVICEHIGH=C:\\WAVE86\\NETDRIVE.SYS -d:2"
CDMKE = "DEVICEHIGH=C:\\PICOMEM\\CDMKE.SYS /D:MSCD000 /P:250 /Q"


def menu_names(flavour):
    return tuple(str(i + 1) for i in range(len(MENU_ITEMS))) if flavour == "fdos" else MENU_NAMES


SHELL = "SHELL=C:\\COMMAND.COM C:\\ /E:2048 /P"


def config_sys(have_cdmke, flavour):
    """CONFIG.SYS after the D6 package's, with a five-item boot menu, ten
    seconds then 1. 1 Standard: HIMEMX and JEMM386 RAM (XMS, EMS, upper
    memory; the card's window kept out), DOS high, NetDrive and the card's
    CD-ROM driver high. 2 Local: without NetDrive. 3 Network: without the
    CD-ROM driver (discs come over NetDrive, mounted in software). 4 Memory
    Optimized: the memory manager with the mono text area as 32 KB more
    upper memory, no drivers. 5 Safe: nothing. The FreeDOS kernel has MENU;
    EDR-DOS's has ECHO, TIMEOUT and SWITCH (no countdown: its wait polls the
    keyboard and prints nothing, so the prompt says the default and the
    time), and SET CONFIG= gives AUTOEXEC.BAT the same %CONFIG%."""
    if flavour in ("msdos", "pcdos", "dos"):
        return config_sys_dos(have_cdmke)
    cd = [CDMKE] if have_cdmke else []
    common = ["FILES=30", "BUFFERS=20", "LASTDRIVE=Z"]
    himem = "DEVICE=C:\\DOS\\HIMEMX.EXE"
    if flavour == "fdos":
        c = ["MENU"] + [f"MENU {i + 1} - {t}" for i, t in enumerate(MENU_ITEMS)] + ["MENU", "MENUDEFAULT=1,10",
             "1234?" + himem,
             "123?" + JEMM,
             "4?" + JEMM + " I=B000-B7FF",
             "1234?DOS=HIGH,UMB"] + common + ["13?" + NETDRIVE] + ["12?" + x for x in cd] + \
            [SHELL + "=C:\\AUTOEXEC.BAT"]
    else:
        std, local, net, mem, safe = MENU_NAMES
        c = [f"ECHO {i + 1} - {t}" for i, t in enumerate(MENU_ITEMS)] + \
            ["ECHO Press 1-5 (1 in 10 seconds)", "TIMEOUT=10", "SWITCH=" + ",".join(MENU_NAMES), "GOTO=BOOT",
             f":{std}", f"SET CONFIG={std}", himem, JEMM, "DOS=HIGH,UMB", NETDRIVE] + cd + ["RETURN",
             f":{local}", f"SET CONFIG={local}", himem, JEMM, "DOS=HIGH,UMB"] + cd + ["RETURN",
             f":{net}", f"SET CONFIG={net}", himem, JEMM, "DOS=HIGH,UMB", NETDRIVE, "RETURN",
             f":{mem}", f"SET CONFIG={mem}", himem, JEMM + " I=B000-B7FF", "DOS=HIGH,UMB", "RETURN",
             f":{safe}", f"SET CONFIG={safe}", "RETURN",
             ":BOOT"] + common + [SHELL]
    return bat(c)


def config_sys_dos(have_cdmke):
    """the same for a licensed DOS whose SYS put it on the image: its own
    HIMEM and EMM386 from C:\\DOS, and DOS 6's [menu]"""
    cd = [CDMKE] if have_cdmke else []
    himem = "DEVICE=C:\\DOS\\HIMEM.SYS /TESTMEM:OFF"
    emm = "DEVICE=C:\\DOS\\EMM386.EXE RAM X=D000-D7FF"
    std, local, net, mem, safe = MENU_NAMES
    c = ["[menu]"] + [f"menuitem={n}, {t}" for n, t in zip(MENU_NAMES, MENU_ITEMS)] + [f"menudefault={std},10",
         "[common]", "FILES=30", "BUFFERS=20", "STACKS=9,256", "LASTDRIVE=Z",
         f"[{std}]", himem, emm, "DOS=HIGH,UMB", NETDRIVE] + cd + \
        [f"[{local}]", himem, emm, "DOS=HIGH,UMB"] + cd + \
        [f"[{net}]", himem, emm, "DOS=HIGH,UMB", NETDRIVE,
         f"[{mem}]", himem, emm + " I=B000-B7FF", "DOS=HIGH,UMB",
         f"[{safe}]",
         "[common]", SHELL]
    return bat(c)


# what a licensed DOS's setup disks hold that the image wants in C:\DOS: the
# name on the image, and the packed name EXPAND knows (MS-DOS 6.22's setup
# disks; a disk with the plain file is fine too, as PC DOS's are)
DOS_TOOLS = ["HIMEM.SYS", "EMM386.EXE", "SMARTDRV.EXE", "MEM.EXE", "XCOPY.EXE", "DELTREE.EXE", "TREE.COM",
             "MORE.COM", "LABEL.EXE", "FIND.EXE", "ATTRIB.EXE", "DOSKEY.COM", "EDIT.COM", "EDIT.HLP", "QBASIC.EXE",
             "MSCDEX.EXE", "SYS.COM", "EXPAND.EXE", "FORMAT.COM", "FDISK.EXE", "CHKDSK.EXE", "SCANDISK.EXE",
             "SCANDISK.INI", "MODE.COM", "CHOICE.COM", "MOVE.EXE", "FC.EXE", "SORT.EXE", "DEBUG.EXE"]


def packed_name(n):
    base, ext = n.rsplit(".", 1)
    return f"{base}.{ext[:2]}_"


def sys_from_disks(work, img, off, geom, disks, seconds):
    """A licensed DOS's own SYS C: onto the empty partition, and its tools
    into C:\\DOS, done by that DOS itself: a copy of its boot floppy, with the
    packed tools from the other disks added and an AUTOEXEC that does the
    work, booted in dosbox-x with the image as C:. The disks are read, never
    written. Returns the list of tools that were not found."""
    floppy = os.path.join(work, "sysdisk.img")
    if os.path.exists(floppy):                      # a copy an earlier run left read-only
        os.chmod(floppy, 0o644)
        os.remove(floppy)
    shutil.copy(disks[0], floppy)
    os.chmod(floppy, 0o644)                         # the copy keeps a read-only disk's mode, and mcopy would fail on it
    listing = {}
    for d in disks:
        for n in mtool("mdir", d, None, "-b", "::/").stdout.upper().split():
            listing.setdefault(n.rsplit("/", 1)[-1], d)
    if "SYS.COM" not in listing or "EXPAND.EXE" not in listing:
        sys.exit(f"mkimage: {disks[0]} has no SYS.COM and EXPAND.EXE; a boot disk with both, please")
    on_first = set(mtool("mdir", floppy, None, "-b", "::/").stdout.upper().replace("::/", "").split())
    lines = ["@echo off", "A:\\SYS.COM C:", "md C:\\DOS"]
    missing = []
    for n in DOS_TOOLS:
        p = packed_name(n)
        if n in listing:
            src, packed = listing[n], False
        elif p in listing:
            src, packed = listing[p], True
        else:
            missing.append(n)
            continue
        fn = p if packed else n
        if fn not in on_first:                      # onto the work floppy from the other disk
            r = mtool("mcopy", src, None, "-n", "-o", f"::/{fn}", os.path.join(work, fn))
            if r.returncode:
                sys.exit(f"mkimage: could not read {fn} off {src}: {r.stderr.strip()}")
            r = mtool("mcopy", floppy, None, "-o", os.path.join(work, fn), f"::/{fn}")
            if r.returncode:
                sys.exit(f"mkimage: no room on the work floppy for {fn}: {r.stderr.strip()}")
            os.remove(os.path.join(work, fn))
        if packed:
            lines.append(f"A:\\EXPAND.EXE A:\\{fn} C:\\DOS\\{n}")
        else:
            lines.append(f"copy A:\\{fn} C:\\DOS\\{n} > NUL")
    lines += ["echo " + MARKER + " > C:\\SYSDONE.TXT"]
    open(os.path.join(work, "SYSAUTO.BAT"), "w", newline="").write(bat(lines))
    open(os.path.join(work, "SYSCONF.SYS"), "w", newline="").write(bat(["FILES=20"]))
    for src, dst in (("SYSAUTO.BAT", "AUTOEXEC.BAT"), ("SYSCONF.SYS", "CONFIG.SYS")):
        r = mtool("mcopy", floppy, None, "-o", os.path.join(work, src), f"::/{dst}")
        if r.returncode:
            sys.exit(f"mkimage: could not put {dst} on the work floppy: {r.stderr.strip()}")
    p = dosbox(["-c", f"imgmount c {os.path.basename(img)} -size 512,{geom}",
                "-c", "imgmount a sysdisk.img -t floppy", "-c", "boot -l a"],
               work, os.path.join(work, "dosbox-sys.log"))
    t0 = time.time()
    done = False
    try:
        while time.time() - t0 < seconds and p.poll() is None:
            time.sleep(3)
            try:
                if MARKER in mtool("mtype", img, off, "::/SYSDONE.TXT", timeout=15).stdout:
                    done = True
                    break
            except subprocess.TimeoutExpired:
                pass
        time.sleep(1)
    finally:
        p.kill()
        p.wait()
    if not done:
        sys.exit(f"mkimage: that DOS did not finish SYS C: in {seconds} s (dosbox-sys.log in {work})")
    mtool("mdel", img, off, "::/SYSDONE.TXT")
    root = mtool("mdir", img, off, "-a", "-b", "::/").stdout.upper()     # -a: SYS hides its files
    if not any(n in root for n in ("IO.SYS", "IBMBIO.COM", "KERNEL.SYS")):
        sys.exit("mkimage: SYS C: left no system files on the image")
    for n in ("DBLSPACE.BIN", "DRVSPACE.BIN"):    # SYS brings the DoubleSpace/DriveSpace driver, and IO.SYS
        if n in root:                              # would load it at every boot: 40 KB for a drive we never compress
            mtool("mattrib", img, off, "-r", "-s", "-h", f"::/{n}")
            mtool("mdel", img, off, f"::/{n}")
            print(f"mkimage: {n} taken off the root (no DoubleSpace at boot)")
    print(f"mkimage: {os.path.basename(disks[0])}: SYS C: done in {time.time() - t0:.0f} s, "
          f"{len(DOS_TOOLS) - len(missing)} tools into C:\\DOS" + (f"; not on the disks: {', '.join(missing)}" if missing else ""))
    return missing


def fill_with_mtools(img, off, stage):
    """the tree onto a volume that already holds a DOS: mtools keeps what is there"""
    for n in sorted(os.listdir(stage)):
        p = os.path.join(stage, n)
        r = mtool("mcopy", img, off, "-s", "-o", "-Q", p, "::/", timeout=600)
        if r.returncode:
            sys.exit(f"mkimage: mcopy {n}: {r.stderr.strip()}")


def autoexec_bat(flavour, doskey=False):
    """AUTOEXEC.BAT after the D6 package's: the card's numbers and the
    environment, then the boot menu's item, by the name (or number) the
    kernel put in CONFIG. 1 the sound cards (PMINIT, all in one call), the
    SD card on W:, the CD-ROM on D:, the mouse, the packet driver and DHCP;
    2 without the network; 3 without W: and the CD-ROM; 4 the sound cards
    and W: only; 5 a prompt in C:\\WAVE86. WMODE names the mode for the
    launcher, which opens on the network view in Network mode. With doskey,
    items 1 to 4 load Enhanced DOSKEY high: history and Tab completion at
    the prompt."""
    pminit = "C:\\PICOMEM\\PMINIT /K /SB 1 /GUS 1 /MPU 1"
    pmdfs = "C:\\PICOMEM\\PMDFS S-W"
    # ?: quiet when the driver has no drive to give. Low on MS-DOS: LOADHIGH into an
    # EMM386 block died in the boot test (a fault loop); JEMM386 takes it high
    shcdx = ("" if flavour in ("msdos", "pcdos", "dos") else "LH ") + "C:\\WAVE86\\SHCDX86.COM /D:?MSCD000,D"
    mouse = "LH C:\\WAVE86\\EXTRAS\\CTMOUSE"
    drvoff = "C:\\WAVE86\\DRVOFF D:"
    net = ["LH C:\\PICOMEM\\PM2000 0x60", "C:\\WAVE86\\DHCP -retries 2 -timeout 10"]
    key = ["LH C:\\DOS\\DOSKEY"] if doskey else []
    std, local, netm, mem, safe = menu_names(flavour)
    return bat([
        "@ECHO OFF",
        "SET BLASTER=A220 I5 D1 T3",
        "SET ULTRASND=240,1,1,5,5",
        "SET ULTRADIR=C:\\ULTRASND",
        "SET MTCPCFG=C:\\WAVE86\\MTCP.CFG",
        "SET TEMP=C:\\TEMP",
        "SET TMP=C:\\TEMP",
        "PATH C:\\WAVE86;C:\\DOS;C:\\MTCP;C:\\PICOMEM",
        "PROMPT $P$G",
        "C:",
        "CD \\WAVE86",
        "GOTO %CONFIG%",
        f":{std}",
        "SET WMODE=Standard",
        drvoff, pminit, pmdfs, shcdx, mouse] + key + net + [
        "GOTO WAVE",
        f":{local}",
        "SET WMODE=Local",
        pminit, pmdfs, shcdx, mouse] + key + [
        "GOTO WAVE",
        f":{netm}",
        "SET WMODE=Network",
        drvoff, pminit, mouse] + key + net + [
        "GOTO WAVE",
        f":{mem}",
        "SET WMODE=Memory",
        pminit, pmdfs] + key + [
        "GOTO WAVE",
        f":{safe}",
        "SET WMODE=Safe",
        "ECHO Safe Mode: nothing is loaded. PMDFS S-W puts the SD card on W:, WAVE86 starts the launcher.",
        "GOTO END",
        ":WAVE",
        "CALL WAVE.BAT",
        ":END"])


def rewrite_ini(text, settings):
    """The global section of a WAVE86.INI with these keys set: a live key
    is replaced where it is; otherwise a commented-out example of it
    (; key=value, the value one token) is brought to life - the one already
    showing this value if there are several, else the first; a key with
    neither goes in before the first [section]. A comment that only
    mentions the key ("; cdrom_storage= names one folder for...") is prose
    and stays. Returns (text, [(old line, new line)])."""
    lines = text.replace("\r\n", "\n").split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    end = next((i for i, l in enumerate(lines) if l.lstrip().startswith("[")), len(lines))
    changes = []
    for key, value in settings:
        pat = re.compile(r"^(\s*;\s*)?" + re.escape(key) + r"\s*=(.*)$", re.I)
        hits = [(i, m) for i, m in ((i, pat.match(lines[i])) for i in range(end)) if m]
        live = [i for i, m in hits if not m.group(1)]
        examples = [(i, m.group(2).split(";")[0].strip()) for i, m in hits if m.group(1)]
        examples = [(i, v) for i, v in examples if not re.search(r"\s", v)]
        same = [i for i, v in examples if v.upper() == value.upper()]
        new = f"{key}={value}"
        if live or same or examples:
            i = (live or same or [examples[0][0]])[0]
            changes.append((lines[i], new))
            lines[i] = new
        else:
            lines.insert(end, new)
            changes.append(("", new))
            end += 1
    return bat(lines), changes

def copy_tree(src, dst):
    shutil.copytree(src, dst, ignore=shutil.ignore_patterns(".*", "._*", "__MACOSX"), dirs_exist_ok=True)


def stage_tree(work, a):
    """The disk's contents as a folder: the FreeDOS root, C:\\WAVE86 from the
    dist, C:\\MTCP, C:\\PICOMEM, C:\\ULTRASND. Returns (stage, notes)."""
    stage = os.path.join(work, "stage")
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(os.path.join(stage, "TEMP"))
    notes = []
    if a.dos_disks:
        pass                            # that DOS's own SYS puts its system files on the image
    elif a.kernel == "edrdos":          # EDR-DOS's kernel under the FreeDOS boot sector, a drop-in
        for n in ("KERNEL.SYS", "COMMAND.COM", "COUNTRY.SYS"):
            src = next((os.path.join(a.edr, x) for x in os.listdir(a.edr) if x.upper() == n), None)
            if src:
                shutil.copy(src, os.path.join(stage, n))
            elif n != "COUNTRY.SYS":
                sys.exit(f"mkimage: no {n} in {a.edr}: not the edrdos release zip?")
    else:
        for n in ("KERNEL.SYS", "COMMAND.COM"):
            r = mtool("mcopy", FREEDOS_IMG, None, "-n", f"::/{n}", os.path.join(stage, n))
            if r.returncode or not os.path.exists(os.path.join(stage, n)):
                sys.exit(f"mkimage: no {n} on {FREEDOS_IMG}: {r.stderr.strip()}")
    w = os.path.join(stage, "WAVE86")
    copy_tree(a.dist, w)
    for n in ("WAVE86.EXE", "WAVE.BAT", "WAVE86.INI", "SHCDX86.COM", "DRVOFF.EXE", "MTCP.CFG",
              os.path.join("EXTRAS", "JEMMEX.EXE"), "NETDRIVE.SYS"):
        if not os.path.exists(os.path.join(w, n)):
            sys.exit(f"mkimage: {n} missing from {a.dist} (make dist)")
    ini = open(os.path.join(w, "WAVE86.INI"), errors="replace").read()
    settings = INI_SETTINGS + ([("slowcache", "0")] if a.flavour in ("msdos", "pcdos", "dos") else [])
    ini, changes = rewrite_ini(ini, settings)      # slowcache: SLOWDOWN and MS-DOS's HIMEM/EMM386 disagree
    cut = re.search(r"^\[", ini, re.M)             # a game section: the launcher's own scribbles
    if cut:
        n = len(re.findall(r"^\[", ini, re.M))
        ini = ini[:cut.start()].rstrip("\r\n") + "\r\n"
        print(f"mkimage: WAVE86.INI: dropped {n} game section{'s' if n != 1 else ''}; the image starts with none")
    open(os.path.join(w, "WAVE86.INI"), "w", newline="").write(ini)
    for old, new in changes:
        print(f"mkimage: WAVE86.INI: {new}" + (f"  (was: {old})" if old else "  (added)"))
    m = os.path.join(stage, "MTCP")
    copy_tree(a.mtcp, m)
    if not any(n.upper() == "DHCP.EXE" for n in os.listdir(m)):
        sys.exit(f"mkimage: no DHCP.EXE in {a.mtcp}: not the mTCP client zip?")
    p = os.path.join(stage, "PICOMEM")
    copy_tree(a.picomem, p)
    have = {x.upper() for x in os.listdir(p)}
    for n in ("PMINIT.EXE", "PMDFS.EXE", "PM2000.COM"):
        if n not in have:
            notes.append(f"{n} is not in {a.picomem}, and AUTOEXEC.BAT runs it")
    if a.cdmke:
        for n in os.listdir(a.cdmke):
            if n.upper() == "CDMKE.SYS":
                shutil.copy(os.path.join(a.cdmke, n), os.path.join(p, "CDMKE.SYS"))
            elif n.upper() == "README.TXT":
                shutil.copy(os.path.join(a.cdmke, n), os.path.join(p, "CDMKE.TXT"))
    have_cdmke = any(n.upper() == "CDMKE.SYS" for n in os.listdir(p))
    if not have_cdmke:
        notes.append("no CDMKE.SYS: the boot menu's items 1 and 2 load no CD-ROM driver")
    g = os.path.join(stage, "ULTRASND")
    if a.gus:
        copy_tree(a.gus, g)
        if not any(n.upper() == "ULTRASND.INI" for n in os.listdir(g)):
            notes.append(f"{a.gus} has no ULTRASND.INI; AUTOEXEC.BAT sets ULTRASND all the same")
    else:
        os.makedirs(g)
        notes.append("C:\\ULTRASND is empty (no --gus folder); AUTOEXEC.BAT sets ULTRASND all the same")
    if a.dos and not a.dos_disks:       # a licensed DOS brings its own utilities
        copy_tree(a.dos, os.path.join(stage, "DOS"))
        for n in ("EDIT.EXE",):
            if not any(x.upper() == n for x in os.listdir(os.path.join(stage, "DOS"))):
                notes.append(f"{n} is not in {a.dos}")
    a.have_doskey = False
    if a.doskey and a.flavour != "fdos":   # FreeCOM has history and Tab completion of its own
        found = {x.upper(): x for x in os.listdir(a.doskey)}
        if "DOSKEY.COM" in found:
            d = os.path.join(stage, "DOS")
            os.makedirs(d, exist_ok=True)   # on MS-DOS it goes over that DOS's own DOSKEY
            for n in ("DOSKEY.COM", "DOSKEY.TXT"):
                if n in found:
                    shutil.copy(os.path.join(a.doskey, found[n]), os.path.join(d, n))
            a.have_doskey = True
        else:
            notes.append(f"no DOSKEY.COM in {a.doskey}: the prompt has no history or Tab completion")
    open(os.path.join(stage, "CONFIG.SYS"), "w", newline="").write(config_sys(have_cdmke, a.flavour))
    open(os.path.join(stage, "AUTOEXEC.BAT"), "w", newline="").write(autoexec_bat(a.flavour, a.have_doskey))
    return stage, notes


def tree_summary(stage):
    out = []
    for d in sorted(os.listdir(stage)):
        p = os.path.join(stage, d)
        if os.path.isdir(p):
            n = sum(len(fs) for _, _, fs in os.walk(p))
            size = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(p) for f in fs)
            out.append(f"C:\\{d} {n} files, {size / 1048576:.1f} MB")
    return out


def clone(src, dst):
    """A copy of the image for a test run; APFS clones it in no time."""
    if os.path.exists(dst):
        os.remove(dst)
    if subprocess.run(["cp", "-c", src, dst], capture_output=True).returncode:
        shutil.copyfile(src, dst)


def boot_test(img, off, geom, work, seconds, chs, doskey=False):
    """Boot a copy of the image in dosbox-x, headless, from the hard disk.
    With chs, the boot sector's INT 13h extension check is turned into a
    no (SYS /FORCE:CHS does the same byte), the path the card takes. With
    doskey, MEM /C must show DOSKEY resident.
    Returns (finished, results dict)."""
    tag = "chs" if chs else "lba"
    test = os.path.join(work, f"test-{tag}.img")
    clone(img, test)
    if chs:
        with open(test, "r+b") as f:
            f.seek(off + 0x178)
            if f.read(4) != b"\x84\xD2\x74\x19":       # test dl,dl / jz: LBA only off A:
                print("mkimage: this boot sector is not the one the CHS patch was written for; skipping")
                return None, {}
            f.seek(off + 0x178)
            f.write(b"\x30")                            # xor dl,dl: never LBA
    # the stub takes WAVE.BAT's place: AUTOEXEC reaching it is the test
    stub = bat(["@echo off", "set MK=MARKER", "md C:\\RESULTS",
                "ver > C:\\RESULTS\\VER.TXT", "set > C:\\RESULTS\\SET.TXT",
                "WAVE86 /diag > C:\\RESULTS\\DIAG.TXT",
                "mem /c > C:\\RESULTS\\MEM.TXT",
                "dir C:\\ > C:\\RESULTS\\DIRC.TXT",
                f"echo {MARKER.replace('MARKER', '%MK%')} > C:\\RESULTS\\DONE.TXT"])
    open(os.path.join(work, "WAVE.BAT"), "w", newline="").write(stub)
    r = mtool("mcopy", test, off, "-o", os.path.join(work, "WAVE.BAT"), "::/WAVE86/WAVE.BAT")
    if r.returncode:
        sys.exit("mkimage: could not put the test stub on the image: " + r.stderr.strip())
    p = dosbox(["-c", f"imgmount c test-{tag}.img -size 512,{geom}", "-c", "boot c:"],
               work, os.path.join(work, f"dosbox-{tag}.log"))
    t0 = time.time()
    finished = False
    try:
        while time.time() - t0 < seconds and p.poll() is None:
            time.sleep(3)
            try:
                if MARKER in mtool("mtype", test, off, "::/RESULTS/DONE.TXT", timeout=15).stdout:
                    finished = True
                    break
            except subprocess.TimeoutExpired:
                pass
        time.sleep(1)
    finally:
        p.kill()
        p.wait()
    took = time.time() - t0
    out = os.path.join(work, f"results-{tag}")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    mtool("mcopy", test, off, "-n", "::/RESULTS/*", out)
    results = {f: open(os.path.join(out, f), errors="replace").read().replace("\r", "")
               for f in sorted(os.listdir(out))}
    print(f"mkimage: boot test ({tag}): {'reached WAVE.BAT' if finished else 'NO MARKER'} after {took:.0f} s")
    for f in ("VER.TXT", "DIAG.TXT"):
        for l in results.get(f, "").strip().splitlines():
            print("    " + l)
    problems = []
    if not finished:
        problems.append(f"the marker never appeared (see {os.path.join(work, f'dosbox-{tag}.log')})")
    diag = results.get("DIAG.TXT", "")
    if "DOS    :" not in diag:
        problems.append("WAVE86 /diag did not run")
    if "XMS    :" in diag and "no driver" in diag:
        problems.append("no XMS: the memory manager in CONFIG.SYS did not load")
    env = re.search(r"Environ: \d+ of (\d+) bytes", diag)
    if not env or int(env.group(1)) < 2048:
        problems.append("the shell's environment is not the 2048 bytes CONFIG.SYS asks for")
    setting = results.get("SET.TXT", "").upper()
    path = "PATH=C:\\WAVE86;C:\\DOS;C:\\MTCP;C:\\PICOMEM\n"
    # WMODE=STANDARD: the boot menu's first item was taken (ten seconds, no key), and AUTOEXEC.BAT went its way
    for v in ("MTCPCFG=C:\\WAVE86\\MTCP.CFG", "BLASTER=A220 I5 D1 T3", "ULTRASND=240,1,1,5,5", "ULTRADIR=C:\\ULTRASND",
              "WMODE=STANDARD", path):
        if v not in setting:
            problems.append(f"AUTOEXEC.BAT did not leave {v.strip()} in the environment")
    if doskey and "DOSKEY" not in results.get("MEM.TXT", "").upper():
        problems.append("DOSKEY is not resident (MEM /C does not list it)")
    return not problems, problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dist", default=os.path.join(ROOT, "dist", "wave86"), help="the launcher, from make dist")
    ap.add_argument("--picomem", required=True, help="folder with the PicoMem DOS tools (PMINIT.EXE, PMDFS.EXE...)")
    ap.add_argument("--mtcp", required=True, help="folder with the mTCP client programs (dhcp.exe...)")
    ap.add_argument("--cdmke", help="folder with CDMKE.SYS, the MKE CD-ROM driver")
    ap.add_argument("--gus", help="folder with the Gravis UltraSound files for C:\\ULTRASND")
    ap.add_argument("--dos", help="folder with FreeDOS utilities (EDIT, MEM, XCOPY...) for C:\\DOS")
    ap.add_argument("--doskey", help="folder with Enhanced DOSKEY (DOSKEY.COM, DOSKEY.TXT) for C:\\DOS: history and "
                                     "Tab completion on the EDR-DOS and MS-DOS images (FreeDOS's shell has its own)")
    ap.add_argument("--kernel", choices=("freedos", "edrdos"), default="edrdos",
                    help="the kernel under the FreeDOS boot sector: EDR-DOS (default; --edr folder) or FreeDOS")
    ap.add_argument("--edr", default=os.path.join(ROOT, "build", "edrdos"),
                    help="folder with EDR-DOS's kernel.sys and command.com (the SvarDOS edrdos release zip, unpacked)")
    ap.add_argument("--dos-disk", dest="dos_disks", action="append", default=[], metavar="IMG",
                    help="your own licensed DOS instead: its bootable floppy image first (with SYS.COM and EXPAND.EXE), "
                         "then the other setup disks; that DOS's SYS puts itself on the image and its EXPAND unpacks "
                         "its tools into C:\\DOS. MS-DOS 6.22 and PC DOS layouts.")
    ap.add_argument("--size", type=int, default=512, help="image size in MB (default 512)")
    ap.add_argument("-o", "--out", help="the image to write (default build/pmwave-<flavour>.img: edrdos, fdos, msdos, pcdos)")
    ap.add_argument("--seconds", type=int, default=180, help="how long each boot test may take (default 180)")
    ap.add_argument("--no-test", action="store_true", help="skip the dosbox-x boot tests")
    a = ap.parse_args()

    for tool in ("dosbox-x", "mcopy", "mtype"):
        if not shutil.which(tool):
            sys.exit(f"mkimage: {tool} not found (dosbox-x; brew install mtools)")
    if a.kernel == "edrdos" and not a.edr:
        sys.exit("mkimage: --kernel edrdos needs --edr <folder with kernel.sys and command.com>")
    for d in (a.dist, a.picomem, a.mtcp, a.cdmke, a.gus, a.dos, a.doskey, a.edr):
        if d and not os.path.isdir(d):
            sys.exit(f"mkimage: no folder {d}")
    for d in a.dos_disks:
        if not os.path.isfile(d):
            sys.exit(f"mkimage: no disk image {d} (--dos-disk)")
    a.flavour = flavour_of(a)
    if not a.out:
        a.out = os.path.join(ROOT, "build", f"pmwave-{a.flavour}.img")
    if not 8 <= a.size <= 2047:
        sys.exit("mkimage: --size is 8 to 2047 MB (FAT16)")
    out = os.path.abspath(a.out)
    # the work files stay under build/ even when the image goes to the SD card
    work = os.path.join(ROOT, "build", os.path.splitext(os.path.basename(out))[0] + "-work")
    os.makedirs(work, exist_ok=True)
    t0 = time.time()

    stage, notes = stage_tree(work, a)
    cyl, heads, secs = image_geometry(a.size)
    print(f"mkimage: {a.size} MB as {cyl} x {heads} x {secs} ({cyl * heads * secs * 512:,} bytes), IMGMAKE running")
    img, off, geom = run_imgmake(work, a.size)
    if a.dos_disks:                     # that DOS's SYS writes its own boot sector and system files
        sys_from_disks(work, img, off, geom, a.dos_disks, a.seconds)
        check_layout(img, own_boot=False)
        fill_with_mtools(img, off, stage)
    else:
        install_boot_sector(img, off, LABELS[a.flavour])
        check_layout(img)
        fat16.fill_tree(img, off, stage)
    mtool("mlabel", img, off, "::" + LABELS[a.flavour])      # the root entry DIR shows, and the BPB field
    listing = mtool("mdir", img, off, "-a", "::/").stdout.upper()       # -a: MS-DOS's SYS hides its files
    system = {"msdos": "IO       SYS", "pcdos": "IBMBIO   COM", "dos": "COMMAND  COM"}.get(a.flavour, "KERNEL   SYS")
    for n in (system, "COMMAND  COM", "CONFIG   SYS", "AUTOEXEC BAT", "WAVE86", "MTCP", "PICOMEM", "ULTRASND"):
        if n not in listing:
            sys.exit(f"mkimage: {n} is not in the root directory:\n{listing}")
    print(f"mkimage: filled in {time.time() - t0:.0f} s: " + ", ".join(tree_summary(stage)))
    for n in notes:
        print("mkimage: note: " + n)

    if not a.no_test:
        ok = []
        for chs in (False, True):
            good, problems = boot_test(img, off, geom, work, a.seconds, chs, a.have_doskey)
            if good is None:
                continue
            for pr in problems:
                print("    ! " + pr)
            ok.append(good)
        if not all(ok):
            sys.exit(f"mkimage: the boot test failed; the image stays in {img}")
        for tag in ("lba", "chs"):
            t = os.path.join(work, f"test-{tag}.img")
            if os.path.exists(t):
                os.remove(t)
    if os.path.exists(out):
        os.remove(out)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    shutil.move(img, out)
    size = os.path.getsize(out)
    sdname = os.path.splitext(os.path.basename(out))[0].upper()[:13] + ".IMG"
    print(f"""
mkimage: {out}: {size:,} bytes, {FLAVOURS[a.flavour]}, volume label {LABELS[a.flavour]}, boots into WAVE86.

On the SD card (SDHC, FAT16/FAT32/exFAT), from its root:
    HDD\\{sdname:<14} this image (the name before .img: 13 characters at most)
    EXODOS\\            the games: gamedir=W:\\EXODOS (the card's SD is W: through PMDFS)
    CDROM\\             the disc images: cdrom_storage=W:\\CDROM (mounted by SHSUCDHD for
                       now; the card's own CD-ROM emulation is not in its firmware yet)
    wifi.txt           the Wi-Fi: SSID on line 1, password on line 2 (for the NE2000)
Then in the PicoMem BIOS Setup (S at the card's "Press S for Setup" prompt):
Disk menu, HDD0 = this image and PicoMEM Boot Code on; Other menu, NE2000 on
at port 300, IRQ 3. Jumpers on the board: IRQ 7 (the card's own) and DMA 1
(Sound Blaster, GUS). C:\\AUTOEXEC.BAT on the image has those numbers in its
commands, and WAVE86.INI's server= wants the address of the machine running
the server (make waveserve, or the release's server zip). The work files are in {work}.""")


if __name__ == "__main__":
    main()
