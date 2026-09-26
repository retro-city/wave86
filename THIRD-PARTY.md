# Third-party work in this repository

WAVE86 itself (the launcher in `src/`, the tools in `tools/`, WAVEGET, DRVOFF,
the soundtrack in `music/`) is under the GPL, version 3 or later, in `LICENSE`.
It ships with other people's programs, each under its own terms:

| What | Where | Licence | Origin |
| --- | --- | --- | --- |
| mTCP 2025-01-10, Michael Brutman: the TCP/IP library WAVEGET and DHCP are built on, the DHCP client, NetDrive's DOS driver and utility (`NETDRIVE.SYS`, `NETDRIVE.EXE`) | `net/mtcp`, `net/dhcp`, `net/NETDRIVE.*` | GPL v3 (`net/mtcp/COPYING.TXT`) | http://www.brutman.com/mTCP/ ; `net/mtcp/TCPLIB/UTILS.CPP` is altered (the far-heap check is skipped, see the file) |
| mTCP NetDrive server 2025-01-10 (Go), Michael Brutman's official build, every platform in one zip | `third-party/netdrive/`; `make netdrive` unpacks the one for this machine into `build/`, `make update-netdrive` refreshes the zip | GPL v3 (`copying.txt` in the zip) | http://www.brutman.com/mTCP/mTCP_NetDrive.html |
| SHSUCD suite 3.09/3.01, Jason Hood: SHSUCDX (MSCDEX replacement) and SHSUCDHD (image file as a CD-ROM) | `cdrom/` | zlib-style (`cdrom/LICENSE.txt`) | https://github.com/adoxa/shsucd, commit 5ea0787, assembled with NASM |
| SLOWDOWN 3.10, Bret Johnson: slows the machine for a game that runs too fast (`slowdown=` in a game's section) | `third-party/slowdown/` (`SLOWDOWN.COM` and `SLOWDOWN.DOC`, from the FreeDOS package); `make dist` and `U` carry both to the DOS machine (`SLOWDOWN_IN_DIST=0` leaves them out of the zip); `make update-slowdown` refreshes them | `SLOWDOWN.DOC`, page 70: "You can freely copy and distribute SLOWDOWN.COM, as long as it is distributed along with this SLOWDOWN.DOC, and neither file has been modified in any way. You cannot charge anyone in any way for SLOWDOWN" (costs of disks and shipping excepted) and "You do need my permission to distribute SLOWDOWN as a 'companion' to some other program" | https://bretjohnson.us/ (slodn310.zip); https://gitlab.com/FreeDOS/util/slowdown |
| EDR-DOS 20260309 (Enhanced DR-DOS, the SvarDOS kernel), for `make picomem-image KERNEL=edrdos` | `third-party/edrdos/`, the release zip; unpacked into `build/edrdos`, `make update-edrdos` refreshes it | a 2022 grant from DRDOS, Inc. ("use, distribute, modify... CP/M and its derivatives") published in the repository, with Caldera's 1997 OpenDOS terms (redistribution for non-commercial purposes) still in the tree; not OSI-approved, not for commercial use | https://github.com/SvarDOS/edrdos/releases |
| 4DOS 8.00, JP Software / Rex Conn: the shell on the PicoMem disk image | `third-party/4dos/4dos800.zip`, the official package; `make picomem-image` unpacks it into `build/4dos` and ships `LICENSE.TXT` with it in `C:\4DOS`; `make update-4dos` refreshes it | JP Software's 2004 notice licence (use, copy, modify, distribute for any purpose without fee, the notice kept with all copies and changes noted); not OSI-approved | https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/util/user/4dos/4dos800.zip ; https://4dos.info/ |
| FreeDOS utilities (EDIT, MEM, MORE, XCOPY, DELTREE, ATTRIB, FIND, TREE, LABEL) for the PicoMem disk image's C:\DOS | `third-party/dosutils/`, the 1.3 packages; `make picomem-image` unpacks their `BIN/` into `build/dosutils`, `make update-dosutils` refreshes them | GPL (each package's `DOC/` folder) | https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.3/base/ |
| Crynwr NE2000 packet driver 11.4.3 | `net/NE2000.COM` | GPL | Crynwr Software; the binary is the one dosbox-x carries (`src/builtin/ne2000bin.cpp`); sources at http://crynwr.com/drivers/ |
| FreeDOS 1.3: kernel 2043 and FreeCOM 0.85a on a boot floppy | `dos/FREEDOS.IMG` | GPL v2 | https://github.com/FDOS/kernel, https://github.com/FDOS/freecom; the floppy is `144m/x86BOOT.img` of the Floppy Edition reduced to kernel and shell, see `dos/README.md` |
| FreeDOS CHOICE 4.4a | `dos/CHOICE.EXE` | GPL v2 | FreeDOS 1.3 package repository, `base/choice.zip` |
| CuteMouse 2.1b4 | `dos/CTMOUSE.EXE` | GPL v2 | FreeDOS 1.3 package repository, `base/ctmouse.zip` |
| JEMM 5.84 (JEMMEX), Japheth, Tom Ehlert, Michael Devore | `dos/JEMMEX.EXE` | Artistic License | FreeDOS 1.3 package repository, `base/jemm.zip` |
| FreeDOS kernel boot sector, FAT16 build of `boot/boot.asm` (Svante Frey and the kernel's authors) | `dos/FAT16.BS`, with its source `dos/boot.asm` and `dos/magic.mac` (the latter public domain, E. C. Masloch) | GPL v2 | https://github.com/FDOS/kernel, commit d6791ad; assembled with NASM, see `dos/README.md` |
| The PicoMEM card's software, FreddyV: the D6 release package, whole, and the card repository's `drivers/` folder, whole (inventory below) | `third-party/picomem/PM_D6_CONFIG.zip` and `third-party/picomem/drivers/`; `make picomem-image` puts the package's PICOMEM folder and `drivers/NE2000.COM` in C:\PICOMEM; `make update-picomem` refetches `drivers/` (`PICOMEM_REF=` a branch or commit) | the repository's LICENSE is GPL v2; each file's own lines are listed below | https://github.com/FreddyVRetro/ISA-PicoMEM (`drivers/` on `main`, fetched 2026-09-26); the D6 package from the card's release channel |
| mTCP 2025-01-10 client programs (DHCP, FTP, HTGet, NetDrive, Ping, Telnet...) | `third-party/mtcp/`, the release zip; `make picomem-image` unpacks it into `build/mtcp`, `make update-mtcp` refreshes it | GPL v3 | https://www.brutman.com/mTCP/ |
| CDMKE.SYS 4.12, the Panasonic/MKE CD-ROM driver, with its README.TXT | `third-party/cdmke/cdmke.zip`, the PicoGUS project's copy (byte for byte the one in the PicoMEM D6 package's DOSDRV folder); `make picomem-image` unpacks it into `build/cdmke` unless `CDMKE_DIR=` is empty; `make update-cdmke` refreshes it | README.TXT: "Copyright (C) 1995-1996 Matsushita-Kotobuki Electronics Industries Ltd. All rights reserved"; no licence text in the zip | https://picogus.com/drivers/cdmke.zip |
| Gravis UltraSound software 4.11 (ULTRASND.INI, ULTRINIT, ULTRAMID, the MIDI patches...), Advanced Gravis | `third-party/picomem/drivers/ultrasnd.zip`, the PicoMEM repository's copy; `make picomem-image` unpacks it into `build/ultrasnd` unless `GUS_DIR=` is empty (`GUS_DIR=` also takes your own copy); `make update-gus` refreshes it | README in the zip: "Copyright (C) 1993,1994,1995 by Advanced Gravis Computer Technology Ltd. All Rights Reserved"; MEGAEM.DOC: "This software and documentation are protected by copyright law, with all rights reserved"; no licence grant text found in the zip | https://github.com/FreddyVRetro/ISA-PicoMEM, `drivers/ultrasnd.zip` |

The rows that say `third-party/` are copies of what the upstream sites
serve, kept in the repository because retro-software download sites come
and go; `third-party/SOURCES.txt` records each file's URL, SHA-256 and the
day it was fetched, `make update-third-party` refreshes them from upstream
and `make check-third-party` (CI runs it) verifies them. Whole folders and
packages are kept as they are upstream; the terms are each piece's own,
listed as the files state them.

## What third-party/picomem holds

`PM_D6_CONFIG.zip`, the PicoMEM D6 release package (folder dated 2026-09-21):

- `PICOMEM/`: PMINIT.EXE (banner "PicoMEM Init Rev 1.0.1 by FreddyV, 04/2026"),
  PMDFS.EXE ("PicoMEM DFS Driver for SD/USB access by FreddyV", "Based on
  EtherDFS v0.8.2 / Copyright (C) 2017,2018 Mateusz Viste"), PMMOUSE.EXE
  ("CuteMouse v2.1b4 for PicoMEM 1.0 [FreeDOS]"), PMEMM.EXE, PM2000.COM,
  PMTEST.EXE ("not supported" says its readme.txt), readme.txt.
- `AUTOEXEC.BAT`, `CONFIG.SYS`: example configuration files.
- `DISKTEST.EXE`: "DiskTest, by James Pearce"; "Portions Copyright (c) 1983,90 Borland".
- `DOSDRV/`: CDMKE.SYS 4.12 with README.TXT (Matsushita-Kotobuki, as above);
  CTMOUSE.EXE (CuteMouse, GPL); DOSKEY.COM with DOSKEY.TXT ("(C) Copyright 2011
  Paul Houle and 2018 Wengier", Enhanced DOSKEY 2.8); CRTFIX.EXE, CRTFIX.PAS,
  CRTFIX.TXT, CRT.TPU (CRTFix 1.16 "by Eugene Toder, 2001"); UNP.EXE with
  UNP.DOC and WHATSNEW.411 (UNP 4.11 "Written by Ben Castricum");
  UNIVBE67.EXE (SciTech UniVBE 6.7; the file carries "CauseWay DOS Extender
  v3.52 Copyright 1992-2000 M.E. Devore" and the Watcom run-time notice);
  SETUPSA.EXE, SAREADME.TXT, OPL3SA.INI, UNINSOPL.EXE, UNINSOPL.INF,
  MESSAGE.TBL, TEST.YMH, DEISL1.ISU (a Yamaha OPL3-SA sound-card setup;
  SETUPSA.EXE carries "Copyright 1993 Intel Corporation" (Configuration
  Manager 1.43) and a Microsoft run-time notice, DEISL1.ISU "Stirling
  Technologies, Inc. (c) 1990-1995"); VIDECDD.SYS ("IDE/ATAPI CD-ROM Device
  Driver Version 2.15", 1999); HWINFO.EXE with HWINFO.DAT, LTEMM.EXE,
  USE!UMBS.SYS ("Version 2.2"), USMP.EXE, TEST!UMB.EXE, MAKELIST.BAT,
  FILE_ID.DIZ: no author or licence line was found inside these.

`PMDFS.EXE`, straight in `third-party/picomem/`: a bug-fixed build of the
driver from FreddyV, received on 2026-09-26 outside the repository; it
replaces the D6 package's copy on the image (banner "PicoMEM DFS Driver for
SD/USB access by FreddyV (DOS 4+)", "Based on EtherDFS v0.8.2 / Copyright
(C) 2017,2018 Mateusz Viste").

`drivers/`, the card repository's folder on `main` (fetched 2026-09-26):

- FreddyV's programs: PMINIT.EXE ("Rev 1.0.2 by FreddyV, 06/2026"), PMDFS.EXE
  and PMDFS3.EXE (EtherDFS-based, as above; PMDFS3 for DOS 3.2-3.31),
  PMEMM.EXE, PM2000.COM, PMMOUSE.EXE ("CuteMouse v2.1b4 for PicoMEM [FreeDOS]"),
  PICOMEM.EXE and TEST/PICOMEM.EXE ("PicoMEM Test v0.2 by FreddyV"; Borland
  run-time notice), README.md.
- NE2000.COM: an NE2000 packet driver; its banner reads "Packet driver for NE2000, version 11.4.3" (Crynwr Software's driver, GPL, as in the `net/NE2000.COM` row).
- ASTCLOCK.COM: "ASTclock Version 1.21, (C)Copyright AST Research, Inc.".
- `SBCD/`: Creative's CD-ROM driver for DOS 4.19 (SBCD.SYS, SETUPCD.EXE,
  TESTCD.EXE, LOCKCD.EXE, UNLOCKCD.EXE, README.COM, README.TXT, README.NOW,
  FILE_ID.DIZ, FILE_ID.OLD) with LICENSE.TXT, the Creative Software License
  ("provided that (i) the Software is not distributed for profit; (ii) the
  Software is used only in conjunction with Creative's family of products;
  (iii) the Software may NOT be modified; (iv) all copyright notices are
  maintained"); CD_DOS.ZIP, the same set as a zip from the Metropoli BBS;
  readme.txt.
- `TEST/EMS/`: EMMSTAT.EXE ("Expanded Memory Status Routine - (c) 1988 Boca
  Research Inc."), EMSTEST.COM (Borland 1985 run-time notice, no author line),
  MOVETEST.COM (no line found), OEMSTEST.COM ("Written by Douglas Boling").
- ultrasnd.zip: the Gravis UltraSound software, see its row.

Not included, read from your own copies: the eXoDOS collection and the Total
DOS Collection (the server indexes them where you keep them, or reads the
collection's torrent), MS-DOS or PC DOS disks (`make dostest DOS=` boots
yours, `make picomem-image DOS_DISKS=` builds the disk image on them), and
the Open Watcom V2 toolchain (`toolchain/`, 50 MB of compilers that `make
toolchain` fetches from a dated release, see the README).

The DRVOFF trick of reserving two NetDrive letters and freeing D: is
DivByZero's, from the NetDrive thread on VOGONS.
