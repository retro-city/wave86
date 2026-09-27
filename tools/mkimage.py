#!/usr/bin/env python3
"""A bootable hard-disk image for a PicoMem 2: FreeDOS, the launcher, the
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
INI_SETTINGS = [("gamedir", "W:\\EXODOS"), ("cdrom_storage", "W:\\CDROM"), ("theme", "picomem"),
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


LABEL = "PM_WAVE"                 # the volume label, in the BPB and the root directory


def install_boot_sector(img, off):
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
        new[0x2B:0x36] = LABEL.encode("ascii").ljust(11)[:11]
        f.seek(off)
        f.write(new)


# 4DOS's settings: its environment, aliases and history up in the UMBs the
# memory manager provides, and no swapping - on the PicoMem machine 4DOS
# swapping itself out while a program runs, to XMS, EMS or disk alike, ended
# in a fault as it came back, so it stays in memory. 4DOS.HLP has every
# directive.
FOURDOS_INI = "\r\n".join([
    "; 4DOS.INI - WAVE86 on a PicoMem 2, written by make picomem-image.",
    "; 4DOS's environment, aliases and history go to upper memory where the",
    "; memory manager provides it, and 4DOS itself stays in memory while a",
    "; program runs: swapping it out - to XMS, EMS or disk - crashed on the",
    "; PicoMem machine when it came back. Swapping=XMS is the setting to try",
    "; on a machine where it works; it gives a game the memory back.",
    "; 4HELP, or F1 at the prompt, explains these and every other directive.",
    "UMBLoad=Yes",
    "UMBEnvironment=Yes",
    "UMBAlias=Yes",
    "UMBHistory=Yes",
    "Swapping=None",
    "EditMode=Insert",
    "LocalAliases=Yes",
    "LocalHistory=Yes",
    "",
]) + "\r\n"
# the same for the boot menu's item 4: nothing in upper memory either
# SWAP.INI: 4DOS as the shell an eXoDOS batch runs in under FreeCOM (the
# boot menu's item 3): swapping to XMS, so the game gets the memory. As
# the shell of the whole session, swapping crashed on the card when it
# came back over a TSR loaded meanwhile; a child that loads after every
# TSR and ends with the batch has none of that to come back over.
FOURDOS_SWAP_INI = FOURDOS_INI.replace("Swapping=None", "Swapping=XMS").replace(
    "; 4DOS.INI - WAVE86 on a PicoMem 2, written by make picomem-image.",
    "; SWAP.INI - 4DOS while an eXoDOS batch runs (the boot menu's item 3): swapping to XMS.")
FOURDOS_NOSWAP_INI = FOURDOS_INI.replace("UMBLoad=Yes", "UMBLoad=No").replace("UMBEnvironment=Yes", "UMBEnvironment=No") \
    .replace("UMBAlias=Yes", "UMBAlias=No").replace("UMBHistory=Yes", "UMBHistory=No") \
    .replace("; 4DOS.INI - WAVE86 on a PicoMem 2, written by make picomem-image.",
             "; NOSWAP.INI - 4DOS for the boot menu's item 4: nothing in upper memory, no swapping.")


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


def config_sys(have_cdmke, fourdos=False, flavour="fdos"):
    if flavour in ("msdos", "pcdos", "dos"):
        return config_sys_dos(have_cdmke, fourdos, flavour)
    menu = flavour == "fdos"            # EDR-DOS's kernel has no MENU lines

    def P(*items):                      # the prefix that gives a line to those menu items
        return "".join(str(n) for n in items) + "?" if menu else ""

    c = [f"; {FLAVOURS[flavour]} on a PicoMem 2 - written by make picomem-image (tools/mkimage.py)"]
    if menu:
        c += ["; A menu: five seconds, then 1. 1 is the setup meant for the card: the",
              "; pair Phil's Computer Lab's FreeDOS boots with, HIMEMX for XMS and",
              "; JEMM386 RAM for EMS and upper memory, the card's own range kept out,",
              "; and 4DOS as the shell, staying in memory (swapping itself out crashed",
              "; on the card, to XMS, EMS and disk alike - and, resident, it holds",
              "; some 290 KB of the 640). 2 adds LBACACHE, a disk cache (AUTOEXEC",
              "; loads it when CONFIG says 2). 3 has FreeCOM as the shell, and an",
              "; eXoDOS start batch (its menus want 4DOS) runs in a 4DOS of its own,",
              "; swapping to XMS, that ends with the batch (AUTOEXEC sets WAVESHELL",
              "; when CONFIG says 3, and the launcher's batch uses it): the game gets",
              "; the memory. 4 and 5 are the two other setups that ran on the card",
              "; while the fault was hunted: JEMMEX with no EMS and nothing in upper",
              "; memory, and no memory manager at all. 6 is 1 with I=B000-B7FF: the",
              "; mono text area, unused with a VGA in colour, as 32 KB more of upper",
              "; memory on a board whose ROMs leave JEMM386 one block. The lines",
              "; that start with digits belong to those choices; F8 still steps.",
              "MENU",
              "MENU  1 - HIMEMX + JEMM386 RAM X=D000-D7FF; 4DOS, not swapping",
              "MENU  2 - as 1, with LBACACHE, an 8 MB disk cache",
              "MENU  3 - as 1, FreeCOM as the shell; 4DOS, swapping, only while a batch runs",
              "MENU  4 - JEMMEX X=A000-FFFF NOEMS NOHI NOINVLPG; 4DOS not swapping, low",
              "MENU  5 - no memory manager; FreeCOM as the shell",
              "MENU  6 - as 1, with I=B000-B7FF: 32 KB more upper memory",
              "MENU",
              "MENUDEFAULT=1,5"]
    c += ["; HIMEMX for extended memory, JEMM386 RAM for EMS and upper memory, so",
          "; DOS, the drivers and the TSRs go up there and a game gets the",
          "; conventional memory. X= keeps its UMBs off the PicoMem's own 24 KB at",
          "; D000-D5FF (16 KB of BIOS, then 8 KB of RAM the card's tools talk",
          "; through) and the 8 KB after it, which the card's own D6 configuration",
          "; keeps free as well; the range moves with a BIOS line in the SD card's",
          "; config.txt. (The PicoMem wiki warns that EMM386 breaks the Sound Blaster",
          "; of a PicoMem 1, whose DMA was done in software; the 2 has real DMA. If",
          "; sound fails here, try NOEMS after X=, then HIMEMX alone.)",
          P(1, 2, 3, 6) + "DEVICE=C:\\DOS\\HIMEMX.EXE",
          P(1, 2, 3) + "DEVICE=C:\\DOS\\JEMM386.EXE RAM X=D000-D7FF",
          P(6) + "DEVICE=C:\\DOS\\JEMM386.EXE RAM X=D000-D7FF I=B000-B7FF"]
    if menu:
        c.append(P(4) + "DEVICE=C:\\WAVE86\\EXTRAS\\JEMMEX.EXE X=A000-FFFF NOEMS NOHI NOINVLPG")
    c += [P(1, 2, 3, 4, 6) + "DOS=HIGH,UMB",
          "FILES=30",
          "BUFFERS=20",
          "LASTDRIVE=Z",
          "; mTCP NetDrive reserves D: and E:; AUTOEXEC.BAT frees D: for the disc",
          "; (DRVOFF D:), so discs kept on the server come in on E:",
          P(1, 2, 3, 6) + "DEVICEHIGH=C:\\WAVE86\\NETDRIVE.SYS -d:2"]
    if menu:
        c.append(P(4, 5) + "DEVICE=C:\\WAVE86\\NETDRIVE.SYS -d:2")
    c += ["; the PicoMem 2's emulated CD-ROM drive: a Panasonic/MKE interface on",
          "; port 250 (no IRQ, no DMA), device MSCD000; SHSUCDX in AUTOEXEC.BAT",
          "; gives it D:. /Q: no Abort/Retry prompt when there is no drive - and",
          "; there is none until the card's firmware has the emulation (PM2-6-21-26",
          "; lists it as planned). WAVE86 (imgmount=PICOMEM) asks you to load a",
          "; game's disc on the card and waits for a key."]
    line = "DEVICE=C:\\PICOMEM\\CDMKE.SYS /D:MSCD000 /P:250 /Q"
    if have_cdmke:
        c.append(line)
    else:
        c += ["; CDMKE.SYS was not there when this image was made (make picomem-image",
              "; takes it from third-party/cdmke unless CDMKE_DIR= is empty). Put it",
              "; in C:\\PICOMEM and take the semicolon off:",
              "; " + line]
    if fourdos:
        c += ["; 4DOS 8.00 is the shell, from C:\\4DOS (its 4DOS.INI keeps it in memory,",
              "; its environment and history high); /P runs C:\\AUTOEXEC.BAT. COMMAND.COM",
              "; stays in the root as a fallback:",
              "; SHELL=C:\\COMMAND.COM C:\\ /E:2048 /P" + (r"=C:\AUTOEXEC.BAT" if menu else ""),
              P(1, 2, 6) + "SHELL=C:\\4DOS\\4DOS.COM C:\\4DOS /E:2048 /P"]
        if menu:
            c += [P(4) + "SHELL=C:\\4DOS\\4DOS.COM C:\\4DOS @C:\\4DOS\\NOSWAP.INI /E:2048 /P",
                  P(3, 5) + "SHELL=C:\\COMMAND.COM C:\\ /E:2048 /P=C:\\AUTOEXEC.BAT"]
    else:
        c.append("SHELL=C:\\COMMAND.COM C:\\ /E:2048 /P" + (r"=C:\AUTOEXEC.BAT" if menu else ""))
    return bat(c)


def config_sys_dos(have_cdmke, fourdos, flavour):
    """CONFIG.SYS for a licensed DOS whose SYS put it on the image: its own
    HIMEM and EMM386 from C:\\DOS instead of JEMMEX, the rest the same, and
    DOS 6's own [menu] for the three kinds of memory setup"""
    name = FLAVOURS[flavour]
    c = [f"REM {name} on a PicoMem 2 - written by make picomem-image (tools/mkimage.py)",
         "REM A menu: five seconds, then EMM. EMM is the setup meant for the card;",
         "REM XMS and NONE are for finding out what a hang or a memory-manager",
         "REM fault is about, one piece at a time: HIMEM alone, or neither.",
         "[menu]",
         "menuitem=EMM, HIMEM and EMM386: XMS, EMS, upper memory",
         "menuitem=XMS, HIMEM only: XMS, nothing in upper memory",
         "menuitem=NONE, no memory manager",
         "menudefault=EMM,5",
         "[common]",
         "FILES=30",
         "BUFFERS=20",
         "STACKS=9,256",
         "LASTDRIVE=Z",
         "[EMM]",
         "REM HIMEM for extended memory, EMM386 for EMS and upper memory (DOS, the",
         "REM drivers and the TSRs go up there), X= keeping its UMBs off the PicoMem's",
         "REM own 24 KB at D000-D5FF and the 8 KB after it, which the card's own D6",
         "REM configuration keeps free as well (the range moves with a BIOS line in",
         "REM the SD card's config.txt). If sound fails, try NOEMS after X=.",
         "DEVICE=C:\\DOS\\HIMEM.SYS",
         "DEVICE=C:\\DOS\\EMM386.EXE RAM X=D000-D7FF",
         "DOS=HIGH,UMB",
         "REM mTCP NetDrive reserves D: and E:; AUTOEXEC.BAT frees D: for the disc",
         "REM (DRVOFF D:), so discs kept on the server come in on E:",
         "DEVICEHIGH=C:\\WAVE86\\NETDRIVE.SYS -d:2",
         "[XMS]",
         "DEVICE=C:\\DOS\\HIMEM.SYS",
         "DOS=HIGH",
         "DEVICE=C:\\WAVE86\\NETDRIVE.SYS -d:2",
         "[NONE]",
         "DEVICE=C:\\WAVE86\\NETDRIVE.SYS -d:2",
         "[common]",
         "REM the PicoMem 2's emulated CD-ROM drive (see the FreeDOS image's notes):",
         "REM none until the card's firmware has it; WAVE86 asks for a disc by hand."]
    line = "DEVICE=C:\\PICOMEM\\CDMKE.SYS /D:MSCD000 /P:250 /Q"
    c.append(line if have_cdmke else "REM " + line + "   (no CDMKE.SYS was given)")
    if fourdos:
        c += ["REM 4DOS 8.00 is the shell, from C:\\4DOS; COMMAND.COM stays in the root:",
              "REM SHELL=C:\\COMMAND.COM C:\\ /E:2048 /P",
              "SHELL=C:\\4DOS\\4DOS.COM C:\\4DOS /E:2048 /P"]
    else:
        c.append("SHELL=C:\\COMMAND.COM C:\\ /E:2048 /P")
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


def autoexec_bat():
    return bat([
        "@ECHO OFF",
        "REM WAVE86 on a PicoMem 2 - written by make picomem-image (tools/mkimage.py).",
        "REM The card's numbers, all in one place. The PicoMem itself is on IRQ 7 (the",
        "REM jumper its firmware requires); Sound Blaster and GUS share IRQ 5 and",
        "REM DMA 1 (the DMA jumper on the board must say 1 too); the NE2000 is on",
        "REM IRQ 3 at port 300, the card's defaults (BIOS Setup, Other menu: a card",
        "REM set to another IRQ leaves the packet driver deaf and DHCP waiting).",
        "SET BLASTER=A220 I5 D1 T3",
        "REM The GUS (port,DMA,DMA,IRQ,IRQ): PMINIT reads ULTRASND, games read both",
        "REM variables and load their patches from ULTRADIR. Set only when the Gravis",
        "REM files are in C:\\ULTRASND, so a game without them falls back to the SB.",
        "SET PMGUS=",
        "IF EXIST C:\\ULTRASND\\ULTRASND.INI SET ULTRASND=240,1,1,5,5",
        "IF EXIST C:\\ULTRASND\\ULTRASND.INI SET ULTRADIR=C:\\ULTRASND",
        "IF EXIST C:\\ULTRASND\\ULTRASND.INI SET PMGUS=/GUS 1",
        "SET PKTINT=0x60",
        "SET NE_IRQ=3",
        "SET NE_PORT=0x300",
        "PATH C:\\WAVE86;C:\\DOS;C:\\4DOS;C:\\MTCP;C:\\PICOMEM",
        "REM A disk cache (8 MB of extended memory): the card's disk is read through",
        "REM its BIOS a sector at a time, so DIR and the launcher's list wait on it",
        "REM once, not every time. SMARTDRV on an MS-DOS image; LBACACHE under",
        "REM FreeDOS when the boot menu's item 2 was chosen (the kernel sets CONFIG).",
        "IF EXIST C:\\DOS\\SMARTDRV.EXE LH C:\\DOS\\SMARTDRV.EXE /X 8192",
        "IF \"%CONFIG%\"==\"2\" LH C:\\DOS\\LBACACHE.COM 8192",
        "REM Boot menu item 3: FreeCOM is the shell, and an eXoDOS start batch runs",
        "REM in a 4DOS of its own that swaps to XMS and ends with the batch (the",
        "REM launcher's batch runs %WAVESHELL% call RUN.BAT): the game gets the",
        "REM memory 4DOS would hold. Set it by hand under any item to try it.",
        "IF \"%CONFIG%\"==\"3\" SET WAVESHELL=C:\\4DOS\\4DOS.COM @C:\\4DOS\\SWAP.INI /C",
        "PROMPT $P$G",
        "SET TEMP=C:\\TEMP",
        "SET TMP=C:\\TEMP",
        "SET MTCPCFG=C:\\WAVE86\\MTCP.CFG",
        "REM D: for the disc, E: for NetDrive (NETDRIVE.SYS -d:2 in CONFIG.SYS)",
        "IF EXIST C:\\WAVE86\\DRVOFF.EXE C:\\WAVE86\\DRVOFF D:",
        "REM The PicoMem 2's sound cards, one PMINIT call (it takes any number of",
        "REM switches, in order): Sound Blaster 2.0 + OPL3 from BLASTER, the GUS",
        "REM from ULTRASND when the Gravis files are there, General MIDI on port",
        "REM 330. (Adlib, CMS and Tandy are BIOS Setup options.)",
        "IF EXIST C:\\PICOMEM\\PMINIT.EXE C:\\PICOMEM\\PMINIT /SB 1 %PMGUS% /MPU 1",
        "SET PMGUS=",
        "REM The SD card as W: (W:\\EXODOS holds the games, W:\\CDROM the discs)",
        "IF EXIST C:\\PICOMEM\\PMDFS.EXE C:\\PICOMEM\\PMDFS S-W",
        "REM The card's CD-ROM (CDMKE.SYS in CONFIG.SYS) on D:; ? = quietly none,",
        "REM which it is until the firmware has the emulation. WAVE86 asks for a",
        "REM game's disc to be loaded on the card (imgmount=PICOMEM, W:\\CDROM).",
        "IF EXIST C:\\WAVE86\\SHCDX86.COM LH C:\\WAVE86\\SHCDX86.COM /D:?MSCD000,D",
        "REM A PS/2 or serial mouse. For a USB mouse on the PicoMem (USB Host and",
        "REM USB Mouse on in BIOS Setup, Advanced) use PMMOUSE instead of CTMOUSE:",
        "REM IF EXIST C:\\PICOMEM\\PMMOUSE.EXE LH C:\\PICOMEM\\PMMOUSE",
        "IF EXIST C:\\WAVE86\\EXTRAS\\CTMOUSE.EXE LH C:\\WAVE86\\EXTRAS\\CTMOUSE",
        "REM The network: the card's NE2000 over Wi-Fi (wifi.txt on the SD card),",
        "REM the card author's packet driver PM2000 on INT 60h (it reads the port",
        "REM and IRQ off the card, and reads odd-sized packets right, which the",
        "REM stock NE2000 driver under it does not - a transfer that stalls at",
        "REM the same place every time is that), then DHCP: two tries of 10 s, so",
        "REM a boot with the Wi-Fi down reaches the menu after 20 s (Esc skips",
        "REM the wait). To go back to the stock driver, swap the REM below.",
        "IF EXIST C:\\PICOMEM\\PM2000.COM LH C:\\PICOMEM\\PM2000 %PKTINT%",
        "IF NOT EXIST C:\\PICOMEM\\PM2000.COM IF EXIST C:\\PICOMEM\\NE2000.COM LH C:\\PICOMEM\\NE2000 %PKTINT% %NE_IRQ% %NE_PORT%",
        "REM LH C:\\PICOMEM\\NE2000 %PKTINT% %NE_IRQ% %NE_PORT%",
        "IF EXIST C:\\WAVE86\\DHCP.EXE C:\\WAVE86\\DHCP -retries 2 -timeout 10",
        "SET PKTINT=",
        "SET NE_IRQ=",
        "SET NE_PORT=",
        "C:",
        "CD \\WAVE86",
        "CALL WAVE.BAT"])

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
            notes.append(f"{n} is not in {a.picomem}; the AUTOEXEC line for it will be skipped"
                         + ((" (NE2000.COM, the stock driver, loads instead)" if "NE2000.COM" in have
                             else " (nor is NE2000.COM: no packet driver will load)") if n == "PM2000.COM" else ""))
    if a.cdmke:
        for n in os.listdir(a.cdmke):
            if n.upper() == "CDMKE.SYS":
                shutil.copy(os.path.join(a.cdmke, n), os.path.join(p, "CDMKE.SYS"))
            elif n.upper() == "README.TXT":
                shutil.copy(os.path.join(a.cdmke, n), os.path.join(p, "CDMKE.TXT"))
    have_cdmke = any(n.upper() == "CDMKE.SYS" for n in os.listdir(p))
    if not have_cdmke:
        notes.append("no CDMKE.SYS: the CD-ROM DEVICE line in CONFIG.SYS is commented out")
    g = os.path.join(stage, "ULTRASND")
    if a.gus:
        copy_tree(a.gus, g)
        if not any(n.upper() == "ULTRASND.INI" for n in os.listdir(g)):
            notes.append(f"{a.gus} has no ULTRASND.INI, which AUTOEXEC.BAT looks for before"
                         " setting ULTRASND/ULTRADIR: the GUS stays off until it is there")
    else:
        os.makedirs(g)
        notes.append("C:\\ULTRASND is empty (no --gus folder: GUS_DIR= was empty or names"
                     " nothing); the GUS stays off until ULTRASND.INI and the rest are copied there")
    if a.dos and not a.dos_disks:       # a licensed DOS brings its own utilities
        copy_tree(a.dos, os.path.join(stage, "DOS"))
        for n in ("EDIT.EXE",):
            if not any(x.upper() == n for x in os.listdir(os.path.join(stage, "DOS"))):
                notes.append(f"{n} is not in {a.dos}")
    fourdos = False
    if a.fourdos:
        d = os.path.join(stage, "4DOS")
        copy_tree(a.fourdos, d)
        fourdos = any(x.upper() == "4DOS.COM" for x in os.listdir(d))
        if not fourdos:
            notes.append(f"no 4DOS.COM in {a.fourdos}: COMMAND.COM stays the shell")
        elif not any(x.upper() == "LICENSE.TXT" for x in os.listdir(d)):
            sys.exit(f"mkimage: {a.fourdos} has no LICENSE.TXT, which 4DOS's licence says must travel with it")
        else:
            open(os.path.join(d, "4DOS.INI"), "w", newline="").write(FOURDOS_INI)
            open(os.path.join(d, "NOSWAP.INI"), "w", newline="").write(FOURDOS_NOSWAP_INI)
            open(os.path.join(d, "SWAP.INI"), "w", newline="").write(FOURDOS_SWAP_INI)
    open(os.path.join(stage, "CONFIG.SYS"), "w", newline="").write(config_sys(have_cdmke, fourdos, a.flavour))
    open(os.path.join(stage, "AUTOEXEC.BAT"), "w", newline="").write(autoexec_bat())
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


def boot_test(img, off, geom, work, seconds, chs):
    """Boot a copy of the image in dosbox-x, headless, from the hard disk.
    With chs, the boot sector's INT 13h extension check is turned into a
    no (SYS /FORCE:CHS does the same byte), the path the card takes.
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
                "dir C:\\ > C:\\RESULTS\\DIRC.TXT",
                # the AUTOEXEC's PMINIT line built the same way, to see what it got
                "SET PMGUS=", "IF EXIST C:\\ULTRASND\\ULTRASND.INI SET PMGUS=/GUS 1",
                "echo /SB 1 %PMGUS% /MPU 1 > C:\\RESULTS\\PMINIT.TXT",
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
        problems.append("no XMS: JEMMEX from CONFIG.SYS did not load")
    env = re.search(r"Environ: \d+ of (\d+) bytes", diag)
    if not env or int(env.group(1)) < 2048:
        problems.append("the shell's environment is not the 2048 bytes CONFIG.SYS asks for")
    setting = results.get("SET.TXT", "").upper()
    for v in ("MTCPCFG=C:\\WAVE86\\MTCP.CFG", "BLASTER=A220", "PATH=C:\\WAVE86;C:\\DOS;C:\\4DOS;C:\\MTCP;C:\\PICOMEM\n"):
        if v not in setting:
            problems.append(f"AUTOEXEC.BAT did not leave {v.strip()} in the environment")
    if "PMGUS=" in setting:
        problems.append("PMGUS was not cleared after the PMINIT line")
    gus = "ULTRASND=" in setting
    pminit = results.get("PMINIT.TXT", "").strip()
    print(f"    GUS: ULTRASND {'set' if gus else 'not set'} (the Gravis files are "
          f"{'there' if gus else 'not there'}); PMINIT would get: {pminit}")
    if gus != ("ULTRADIR=C:\\ULTRASND" in setting):
        problems.append("ULTRASND and ULTRADIR were not set together")
    if gus != ("/GUS 1" in pminit) or "/SB 1" not in pminit or "/MPU 1" not in pminit:
        problems.append("the PMINIT line did not come out as meant")
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
    ap.add_argument("--4dos", dest="fourdos", help="folder with 4DOS 8.00 (4DOS.COM, 4DOS.HLP, LICENSE.TXT...): the shell")
    ap.add_argument("--kernel", choices=("freedos", "edrdos"), default="freedos",
                    help="the kernel under the FreeDOS boot sector: FreeDOS (default) or EDR-DOS (--edr folder)")
    ap.add_argument("--edr", help="folder with EDR-DOS's kernel.sys and command.com (the SvarDOS edrdos release zip)")
    ap.add_argument("--dos-disk", dest="dos_disks", action="append", default=[], metavar="IMG",
                    help="your own licensed DOS instead: its bootable floppy image first (with SYS.COM and EXPAND.EXE), "
                         "then the other setup disks; that DOS's SYS puts itself on the image and its EXPAND unpacks "
                         "its tools into C:\\DOS. MS-DOS 6.22 and PC DOS layouts.")
    ap.add_argument("--size", type=int, default=512, help="image size in MB (default 512)")
    ap.add_argument("-o", "--out", help="the image to write (default build/pmwave-<flavour>.img: fdos, edrdos, msdos, pcdos)")
    ap.add_argument("--seconds", type=int, default=180, help="how long each boot test may take (default 180)")
    ap.add_argument("--no-test", action="store_true", help="skip the dosbox-x boot tests")
    a = ap.parse_args()

    for tool in ("dosbox-x", "mcopy", "mtype"):
        if not shutil.which(tool):
            sys.exit(f"mkimage: {tool} not found (dosbox-x; brew install mtools)")
    if a.kernel == "edrdos" and not a.edr:
        sys.exit("mkimage: --kernel edrdos needs --edr <folder with kernel.sys and command.com>")
    for d in (a.dist, a.picomem, a.mtcp, a.cdmke, a.gus, a.dos, a.fourdos, a.edr):
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
        install_boot_sector(img, off)
        check_layout(img)
        fat16.fill_tree(img, off, stage)
    mtool("mlabel", img, off, "::" + LABEL)      # the root directory's label entry, what DIR shows
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
            good, problems = boot_test(img, off, geom, work, a.seconds, chs)
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
mkimage: {out}: {size:,} bytes, {FLAVOURS[a.flavour]}, boots into WAVE86.

On the SD card (SDHC, FAT16/FAT32/exFAT), from its root:
    HDD\\{sdname:<14} this image (the name before .img: 13 characters at most)
    EXODOS\\            the games: gamedir=W:\\EXODOS (the card's SD is W: through PMDFS)
    CDROM\\             the disc images: cdrom_storage=W:\\CDROM (mounted by SHSUCDHD for
                       now; the card's own CD-ROM emulation is not in its firmware yet)
    wifi.txt           the Wi-Fi: SSID on line 1, password on line 2 (for the NE2000)
Then in the PicoMem BIOS Setup (S at the card's "Press S for Setup" prompt):
Disk menu, HDD0 = this image and PicoMEM Boot Code on; Other menu, NE2000 on
at port 300, IRQ 3. Jumpers on the board: IRQ 7 (the card's own) and DMA 1
(Sound Blaster, GUS). C:\\AUTOEXEC.BAT on the image has those numbers at its
top, and WAVE86.INI's server= wants the address of the Mac running make
waveserve. The work files are in {work}.""")


if __name__ == "__main__":
    main()
